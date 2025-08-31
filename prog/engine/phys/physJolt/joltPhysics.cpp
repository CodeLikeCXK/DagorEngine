// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <supp/dag_math.h>
#if _TARGET_C1 | _TARGET_C2

#elif defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#endif
#include <phys/dag_physics.h>
#include <osApiWrappers/dag_cpuJobs.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include "shapes/HeightField16Shape.h"
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/ConfigurationString.h>
#include <heightmap/heightmapHandler.h>
#include <util/dag_threadPool.h>
#include <osApiWrappers/dag_spinlock.h>
#include <memory/dag_fixedBlockAllocator.h>
#include <memory/dag_framemem.h>
#include <generic/dag_relocatableFixedVector.h>
#include <ioSys/dag_dataBlock.h>
#include <startup/dag_globalSettings.h>
#include <osApiWrappers/dag_miscApi.h>
#include <perfMon/dag_statDrv.h>

namespace layers
{
enum : JPH::BroadPhaseLayer::Type
{
  STATIC,
  DYNAMIC,
  DEBRIS, // Debris collides only with STATIC

  NUM_LAYERS
};
}; // namespace layers

// FIXME: coalesce this with dacoll::PhysLayer (move it to engine)
static constexpr const uint16_t DefaultFilter = 1;
static constexpr const uint16_t StaticFilter = 2;
static constexpr const uint16_t KinematicFilter = 4;
static constexpr const uint16_t DebrisFilter = 8;
static constexpr const uint16_t AllFilter = 0x3F;

static constexpr float drawConstraintSize = 0.1f;

inline JPH::ObjectLayer make_objlayer_debris(JPH::BroadPhaseLayer::Type bl, uint16_t gm, uint16_t lm)
{
  JPH::BroadPhaseLayer::Type bphl = (bl == layers::DYNAMIC && gm == DebrisFilter) ? layers::DEBRIS : bl;
  return make_objlayer(bphl, gm, lm);
}

/// BroadPhaseLayerInterface implementation
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
  JPH::uint GetNumBroadPhaseLayers() const override { return layers::NUM_LAYERS; }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(objlayer_to_bphlayer(l)); }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  virtual const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
  {
    switch ((BroadPhaseLayer::Type)inLayer)
    {
      case layers::STATIC: return "STATIC";
      case layers::DYNAMIC: return "DYNAMIC";
      case layers::DEBRIS: return "DEBRIS";
      default: G_ASSERTF(false, "inLayer=%d", (BroadPhaseLayer::Type)inLayer); return "INVALID";
    }
  }
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED
};

static JPH::JobSystem *jobSystem = nullptr;
JPH::PhysicsSystem *jolt_api::physicsSystem = nullptr;
static BPLayerInterfaceImpl broadPhaseLayerInterface;
static JPH::PhysicsSettings physicsSettings;
static JPH::TempAllocatorImpl *tempAllocator = nullptr;
JPH::ShapeCastSettings PhysWorld::defShapeCastStg;
JPH::CollideShapeSettings PhysWorld::defCollideShapeStg;

static struct JoltBroadphaseCanCollide final : public JPH::ObjectVsBroadPhaseLayerFilter
{
  bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer _inLayer2) const override
  {
    auto inLayer2 = (JPH::BroadPhaseLayer::Type)_inLayer2;
    switch (objlayer_to_bphlayer(inLayer1))
    {
      case layers::STATIC: return inLayer2 != layers::STATIC;
      case layers::DYNAMIC: return inLayer2 != layers::DEBRIS;
      case layers::DEBRIS: return inLayer2 == layers::STATIC;
    }
    G_ASSERTF(false, "inLayer1=%d -> broadphaseLayer=%d", inLayer1, objlayer_to_bphlayer(inLayer1));
    return false;
  }
} broadphase_f;
static struct JoltObjsCanCollide final : public JPH::ObjectLayerPairFilter
{
  bool ShouldCollide(JPH::ObjectLayer o1, JPH::ObjectLayer o2) const override
  {
    return (objlayer_to_group_mask(o1) & objlayer_to_layer_mask(o2)) && (objlayer_to_group_mask(o2) & objlayer_to_layer_mask(o1));
  }
} objects_f;

static void jolt_trace(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  // Usually Jolt prints error related output unless some debugging enabled (so use error loglevel),
  // with exception of `AABBTreeBuilder: ...` which is rather benign warning that sometimes happens
  // "when the triangles in a batch are intersecting", see
  // https://github.com/jrouwe/JoltPhysics/issues/1127#issuecomment-2156730951
  int ll = LOGLEVEL_ERR;
  JPH_IF_DEBUG(ll = strstr(fmt, "AABBTreeBuilder: Doing random split") ? LOGLEVEL_WARN : ll);
  cvlogmessage(ll, fmt, ap);
  va_end(ap);
}

JPH_IF_ENABLE_ASSERTS(
  static bool joltAssertFailed(const char *inExpression, const char *inMessage, const char *inFile, JPH::uint inLine) {
    if (!dgs_assertion_handler)
      return true;
    return dgs_assertion_handler_inl(false, inFile, inLine, "", inExpression, inMessage);
  })

class JoltJobSystemImpl final : public JPH::JobSystem
{
  FixedBlockAllocator fbaJobs, fbaBarriers;
  OSSpinlock fbaJobsSL, fbaBarriersSL;
  unsigned maxThreads;
  void *chunkHoldPtr1, *chunkHoldPtr2; //< to prevent chunk release when last work block is released
  cpujobs::IJob *delayedFreeTail = nullptr;

public:
  JoltJobSystemImpl(unsigned max_jobs, unsigned max_barriers, unsigned max_threads) :
    maxThreads(max_threads), fbaJobs((uint32_t)sizeof(JobImpl), max_jobs), fbaBarriers((uint32_t)sizeof(BarrierImpl), max_barriers)
  {
    chunkHoldPtr1 = fbaJobs.allocateOneBlock();
    chunkHoldPtr2 = fbaBarriers.allocateOneBlock();
  }
  ~JoltJobSystemImpl()
  {
    fbaBarriers.freeOneBlock(chunkHoldPtr2);

    uint32_t _1, _2, usedMem = 0;
    {
      OSSpinlockScopedLock lock(fbaJobsSL);
      flushDelayedFreeList();
      fbaJobs.freeOneBlock(chunkHoldPtr1);
      fbaJobs.getMemoryStats(_1, _2, usedMem);
      if (DAGOR_LIKELY(usedMem == 0))
      {
        G_FAST_ASSERT(!delayedFreeTail);
        return;
      }
    }

    // Wait for rare jobs that wasn't added/waited by barrier
    spin_wait([&] {
      fbaJobsSL.lock();
      flushDelayedFreeList();
      fbaJobs.getMemoryStats(_1, _2, usedMem);
      fbaJobsSL.unlock();
      return usedMem != 0;
    });
  }

  struct JobImpl final : public cpujobs::IJob, public Job
  {
    JobImpl(const char *jName, JPH::ColorArg color, JPH::JobSystem *jSys, const JobFunction &jFunc, JPH::uint32 numDeps) :
      Job(jName, color, jSys, jFunc, numDeps)
    {}
    const char *getJobName(bool &) const override { return "JoltJobImpl"; }
    void doJob() override
    {
      Execute();
      Release();
    }
    JobImpl *prevInBarrier = nullptr;
    // Don't execute non jolt jobs on active wait as it causing lock-order-inversion on e.g. Jolt rw locks & rendinst rw-locks
#ifdef __SANITIZE_THREAD__
    static constexpr threadpool::JobPriority tprio = threadpool::PRIO_HIGH;
#else
    // Low prio in order to avoid interference with neighbouring parallel-for jobs
    static constexpr threadpool::JobPriority tprio = threadpool::PRIO_LOW;
#endif
    enum State : uint32_t
    {
      S_INITIAL,
      S_ADDING,
      S_SUBMITTED,
      S_WAITED_ON
    };
    union
    {
      State state = S_INITIAL;
      volatile int istate;
    };
    State getState() const { return (State)interlocked_acquire_load(istate); }
    void setState(State s) { interlocked_release_store(istate, s); }
  };

  class BarrierImpl final : public Barrier
  {
  public:
    ~BarrierImpl()
    {
      G_FAST_ASSERT(n2wait == 0);
      for (auto *j = interlocked_acquire_load_ptr(jTail); j;)
      {
        auto jprev = j->prevInBarrier;
        j->Release();
        j = jprev;
      }
    }

    void AddJob(const JobHandle &inJob) override
    {
      auto job = static_cast<JobImpl *>(inJob.GetPtr());
      if (job->SetBarrier(this)) // Note: this if is not taken if job is already done at this point
      {
        job->AddRef();
        G_ASSERT(!job->prevInBarrier);
        interlocked_increment(n2wait);
        if (job->CanBeExecuted())
          JoltJobSystemImpl::submitJob(job);

        JobImpl *prev = interlocked_acquire_load_ptr(jTail);
        while (1)
        {
          job->prevInBarrier = prev;
          if (JobImpl *tail = interlocked_compare_exchange_ptr(jTail, job, prev); tail == prev)
            break;
          else
            prev = tail;
        }
      }
    }

    void AddJobs(const JobHandle *inHandles, JPH::uint inNumHandles) override
    {
      bool should_wake_up = inNumHandles > 16;
      for (; inNumHandles; inHandles++, inNumHandles--)
        AddJob(*inHandles);
      if (should_wake_up)
        threadpool::wake_up_all();
    }

    void activeWait()
    {
      while (interlocked_acquire_load(n2wait) > 0) // Note: new jobs can keep being added during this wait
        for (auto *j = interlocked_acquire_load_ptr(jTail); j; j = j->prevInBarrier)
          if (j->getState() == j->S_SUBMITTED)
          {
            threadpool::wait(j, 0, j->tprio); // Active wait
            j->setState(j->S_WAITED_ON);
            interlocked_decrement(n2wait);
          }
    }

  protected:
    void OnJobFinished(Job * /*inJob*/) override {}
    int n2wait = 0;
    JobImpl *jTail = nullptr;
  };

  int GetMaxConcurrency() const override { return maxThreads; }

  void *allocJobMem()
  {
    OSSpinlockScopedLock lock(fbaJobsSL);
    return fbaJobs.allocateOneBlock();
  }
  JobHandle CreateJob(const char *inName, JPH::ColorArg inColor, const JobFunction &inJobFunction,
    JPH::uint32 inNumDependencies) override
  {
    auto j = ::new (allocJobMem(), _NEW_INPLACE) JobImpl(inName, inColor, this, inJobFunction, inNumDependencies);
    JobHandle h(j);
    if (inNumDependencies == 0)
      QueueJob(j);
    return h;
  }

  void *allocBarrierMem()
  {
    OSSpinlockScopedLock lock(fbaBarriersSL);
    return fbaBarriers.allocateOneBlock();
  }
  Barrier *CreateBarrier() override
  {
    if (DAGOR_UNLIKELY(interlocked_relaxed_load_ptr(delayedFreeTail)))
    {
      OSSpinlockScopedLock lock(fbaJobsSL);
      flushDelayedFreeList();
    }
    return ::new (allocBarrierMem(), _NEW_INPLACE) BarrierImpl;
  }

  /// Destroy a barrier when it is no longer used. The barrier should be empty at this point.
  void DestroyBarrier(Barrier *inBarrier) override
  {
    auto ptr = static_cast<BarrierImpl *>(inBarrier);
    ptr->~BarrierImpl();
    OSSpinlockScopedLock lock(fbaBarriersSL);
    fbaBarriers.freeOneBlock(ptr);
  }

  void WaitForJobs(Barrier *inBarrier) override { static_cast<BarrierImpl *>(inBarrier)->activeWait(); }

protected:
  static void submitJob(JobImpl *j)
  {
    if (interlocked_compare_exchange(j->istate, j->S_ADDING, j->S_INITIAL) != j->S_INITIAL)
      return;
    j->AddRef();
    G_FAST_ASSERT(interlocked_acquire_load(j->done)); // Freshly allocated job should have `done` set
    uint32_t qPos;
    threadpool::add(j, j->tprio, qPos, threadpool::AddFlags::IgnoreNotDone);
    j->setState(j->S_SUBMITTED);
  }
  void QueueJob(Job *inJob) override { submitJob(static_cast<JobImpl *>(inJob)); }
  void QueueJobs(Job **inJobs, JPH::uint inNumJobs) override
  {
    bool should_wake_up = inNumJobs > 4;
    for (; inNumJobs; inJobs++, inNumJobs--)
      submitJob(static_cast<JobImpl *>(*inJobs));
    if (should_wake_up)
      threadpool::wake_up_all();
  }

  void FreeJob(Job *inJob) override
  {
    auto jptr = static_cast<JobImpl *>(inJob);
    G_FAST_ASSERT(inJob->IsDone());
    jptr->~JobImpl();
    // Addition to barrier can be skipped if job was already done (IsDone()=true) at Jolt. It might cause
    // this method to be called from job itself causing it not to be `done' by threadpool side
    OSSpinlockScopedLock lock(fbaJobsSL);
    if (interlocked_acquire_load(jptr->done))
      fbaJobs.freeOneBlock(jptr);
    else // Add to list in order to free it later
    {
      jptr->next = delayedFreeTail;
      delayedFreeTail = jptr;
    }
  }

  void flushDelayedFreeList()
  {
    for (auto *j2free = interlocked_acquire_load_ptr(delayedFreeTail); j2free;)
    {
      auto jprev = j2free->next;
      threadpool::wait(j2free, 0, JobImpl::tprio);
      fbaJobs.freeOneBlock(j2free);
      j2free = jprev;
    }
    delayedFreeTail = nullptr;
  }
};

void PhysWorld::init_engine(bool single_threaded)
{
  using namespace JPH;
  using namespace jolt_api;
  if (physicsSystem)
    return;

  const DataBlock &phys_blk = *dgs_get_settings()->getBlockByNameEx("physJolt");
  // To consider: use different defaults for client & dedicated/offline
  uint32_t maxBodiesNum = phys_blk.getInt("maxBodiesNum", 24 << 10);
  uint32_t maxBodiesPairsNum = phys_blk.getInt("maxBodiesPairsNum", 65536);
  uint32_t maxContactConstraints = phys_blk.getInt("maxContactConstraints", 10240);
  uint32_t tempAllocSz = phys_blk.getInt("tempAllocatorSizeKB", 10 << 10) << 10;
  uint32_t maxJobs = phys_blk.getInt("maxJobs", 1024);
  uint32_t maxBarriers = phys_blk.getInt("maxBarriers", 2); // Note: only 1 barrier seems to be used, do we really need pool of them?
  uint32_t maxWorkers = phys_blk.getInt("maxWorkers", 16);
  uint32_t defNumBodyMutexes = (threadpool::get_num_workers() + 1) * phys_blk.getInt("numBodyMutexesMult", 2);
  uint32_t numBodyMutexes = phys_blk.getInt("numBodyMutexes", defNumBodyMutexes);
  float maxCollisionSeparationDistance = phys_blk.getReal("maxCollisionSeparationDistance", 0.05f);
  JPH::HeightField16Shape::sUseActiveEdges = phys_blk.getBool("htFieldBuildActiveEdges", true);

  JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssertFailed;)
  JPH::Trace = &jolt_trace;
  tempAllocator = new JPH::TempAllocatorImpl(tempAllocSz); // To consider: implement using framemem. Is cross-thread freeing required?
  JPH::Factory::sInstance = new JPH::Factory;
  JPH::RegisterTypes();
  JPH::HeightField16Shape::sRegister();
  physicsSystem = new JPH::PhysicsSystem();
  physicsSystem->Init(maxBodiesNum, numBodyMutexes, maxBodiesPairsNum, maxContactConstraints, broadPhaseLayerInterface, broadphase_f,
    objects_f);
  physicsSystem->SetPhysicsSettings(physicsSettings);
  physicsSystem->SetGravity(JPH::Vec3(0, -9.8065f, 0));
  // Compat with Bullet (see `btManifoldResult::calculateCombined{Friction,Restitution}`)
  physicsSystem->SetCombineFriction([](const Body &b1, const SubShapeID &, const Body &b2, const SubShapeID &) {
    return b1.GetFriction() * b2.GetFriction(); // To consider: clamp(..., /*MAX_FRICTION*/ 10)?
  });
  physicsSystem->SetCombineRestitution([](const Body &b1, const SubShapeID &, const Body &b2, const SubShapeID &) {
    return b1.GetRestitution() * b2.GetRestitution(); //
  });

  if (phys_blk.getInt("jobSysType", single_threaded ? 0 : 1))
    jobSystem = new JoltJobSystemImpl(maxJobs / 16, maxBarriers, clamp<int>(threadpool::get_num_workers(), 1, maxWorkers));
  else
    jobSystem = new JPH::JobSystemSingleThreaded(maxJobs);

  // defShapeCastStg.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
  defShapeCastStg.mReturnDeepestPoint = true;

  // defCollideShapeStg.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
  defCollideShapeStg.mMaxSeparationDistance = maxCollisionSeparationDistance;

  static bool printed_once = false;
  if (!printed_once)
  {
    debug("using phys engine: Jolt [%s], job system(%d) for %d threads (maxJobs=%d maxBarriers=%d), tmpAllocator=%dK, maxBodies=%d",
      JPH::GetConfigurationString(), phys_blk.getInt("jobSysType", 1), jobSystem->GetMaxConcurrency(), maxJobs, maxBarriers,
      tempAllocSz >> 10, maxBodiesNum);
    printed_once = true;
  }
}

void PhysWorld::term_engine()
{
  using namespace jolt_api;

  // Note: phys world which does `fetchSimRes` in its dtor is assumed to be destroyed at this point
  del_it(jobSystem);

  del_it(physicsSystem);
  del_it(tempAllocator);

  JPH::UnregisterTypes();
  del_it(JPH::Factory::sInstance);
}

JPH::Quat to_jQuat(const TMatrix &tm)
{
  // Note: vector convention gives different results somehow. Figure it out?
  /*
  mat44f m;
  v_mat44_make_from_43cu_unsafe(m, tm.array);
  return JPH::Quat(v_norm4(v_un_quat_from_mat4(m)));
  return JPH::Mat44(m.col0, m.col1, m.col2, V_C_UNIT_0001).GetQuaternion().Normalized(); */
  Quat q = normalize(Quat(tm));
  return JPH::Quat(q.x, q.y, q.z, q.w);
}

PhysBody::PhysBody(PhysWorld *w, float mass, const PhysCollision *coll, const TMatrix &tm, const PhysBodyCreationData &s) : world(w)
{
  userPtr = s.userPtr;

  JPH::BodyCreationSettings body;
  bool isDynamic = mass > 0.f;
  if (auto shape = PhysBody::create_jolt_collision_shape(coll, isDynamic))
    body.SetShape(shape);
  if (s.autoMask)
  {
    groupMask = isDynamic ? DefaultFilter : StaticFilter;
    layerMask = isDynamic ? AllFilter : (AllFilter ^ StaticFilter);
  }
  else
  {
    groupMask = s.group;
    layerMask = s.mask & AllFilter;
  }
  body.mObjectLayer = make_objlayer_debris(isDynamic ? layers::DYNAMIC : layers::STATIC, groupMask, layerMask);
  G_ASSERTF(objlayer_to_group_mask(body.mObjectLayer) == groupMask, "groupMask=0x%x -> objLayer=0x%x", groupMask,
    (unsigned)body.mObjectLayer);
  G_ASSERTF(objlayer_to_layer_mask(body.mObjectLayer) == layerMask, "layerMask=0x%x -> objLayer=0x%x", layerMask,
    (unsigned)body.mObjectLayer);

  body.mMotionType = isDynamic ? JPH::EMotionType::Dynamic : (s.kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static);
  if (body.mMotionType != JPH::EMotionType::Static)
  {
    body.mOverrideMassProperties =
      s.autoInertia ? JPH::EOverrideMassProperties::CalculateInertia : JPH::EOverrideMassProperties::MassAndInertiaProvided;
    body.mMassPropertiesOverride.mMass = (body.mMotionType == JPH::EMotionType::Kinematic) ? 1.0f : mass;
    body.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(to_jVec3(s.momentOfInertia));
  }
  body.mPosition = to_jVec3(tm.getcol(3));
  body.mRotation = to_jQuat(tm);
  body.mLinearDamping = s.linearDamping;
  body.mAngularDamping = s.angularDamping;
  body.mUserData = (uintptr_t)this;

  const PhysWorld::Material *material = world->getMaterial(s.materialId);
  if (material)
  {
    body.mFriction = material->friction;
    body.mRestitution = material->restitution;
    safeMaterialId = s.materialId;
  }
  else
    safeMaterialId = 0;
  if (s.friction >= 0.f)
    body.mFriction = s.friction;
  else if (!material)      // I.e. not assigned from material
    body.mFriction = 0.5f; // Bullet's default
  if (s.restitution >= 0.f)
    body.mRestitution = s.restitution;

  if (auto *b = api().CreateBody(body))
  {
    if (s.autoInertia && !b->IsStatic() && mass > 0)
    {
      auto &mp = *b->GetMotionProperties();
      float scale = safeinv(mass) / mp.GetInverseMass();
      mp.SetInverseMass(mp.GetInverseMass() * scale);
      mp.SetInverseInertia(mp.GetInverseInertiaDiagonal() * scale, mp.GetInertiaRotation());
    }
    bodyId = b->GetID();
    if (s.addToWorld)
      api().AddBody(bodyId, JPH::EActivation::Activate);
  }
  else
    LOGERR_ONCE("Failed to create body with collType=%d @ %@. cur/maxBodies=%d/%d", coll->collType, tm,
      PhysWorld::getScene()->GetNumBodies(), PhysWorld::getScene()->GetMaxBodies());
}
PhysBody::~PhysBody()
{
  G_ASSERTF_RETURN(jolt_api::physicsSystem, , "Attempt to destroy phys body after `term_engine`?");
  if (api().IsAdded(bodyId))
    api().RemoveBody(bodyId);
  if (isValid())
    api().DestroyBody(bodyId);
}

void PhysBody::setTm(const TMatrix &wtm)
{
  lockRW([&](JPH::Body &b) { // Lock explicitly to save read-lock call on `IsAdded`
    auto act = b.IsInBroadPhase() ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
    jolt_api::physicsSystem->GetBodyInterfaceNoLock().SetPositionAndRotation(bodyId, to_jVec3(wtm.getcol(3)), to_jQuat(wtm), act);
  });
}

void PhysBody::getTm(TMatrix &wtm) const
{
  JPH::Mat44 transform = api().GetWorldTransform(bodyId);
  wtm = to_tmatrix(transform);
}

void PhysBody::disableGravity(bool disable) { api().SetGravityFactor(bodyId, disable ? 0.f : 1.f); }
bool PhysBody::isGravityDisabled()
{
  return api().GetGravityFactor(bodyId) <= 0.f ? true /* gravity disabled */ : false /* gravity enabled */;
}


void PhysBody::setTmWithDynamics(const TMatrix &wtm, real dt, bool activate)
{
  api().MoveKinematic(bodyId, to_jVec3(wtm.getcol(3)), to_jQuat(wtm), dt);
}

// Zero mass makes body static.
void PhysBody::setMassMatrix(real _mass, real ixx, real iyy, real izz)
{
  lockRW([&](JPH::Body &b) {
    auto &mp = *b.GetMotionProperties();
    mp.SetInverseMass(safeinv(_mass));
    mp.SetInverseInertia(JPH::Vec3(ixx, iyy, izz).Reciprocal(), mp.GetInertiaRotation());
  });
}

void PhysBody::getMassMatrix(real &_mass, real &ixx, real &iyy, real &izz)
{
  lockRO([&](const JPH::Body &b) {
    if (!b.IsStatic())
    {
      auto &mp = *b.GetMotionProperties();
      _mass = safeinv(mp.GetInverseMass());
      ixx = safeinv(mp.GetInverseInertiaDiagonal().GetX());
      iyy = safeinv(mp.GetInverseInertiaDiagonal().GetY());
      izz = safeinv(mp.GetInverseInertiaDiagonal().GetZ());
    }
    else
      _mass = ixx = iyy = izz = 0.f;
  });
}

void PhysBody::getInvMassMatrix(real &_mass, real &ixx, real &iyy, real &izz)
{
  lockRO([&](const JPH::Body &b) {
    if (!b.IsStatic())
    {
      auto &mp = *b.GetMotionProperties();
      _mass = mp.GetInverseMass();
      ixx = mp.GetInverseInertiaDiagonal().GetX();
      iyy = mp.GetInverseInertiaDiagonal().GetY();
      izz = mp.GetInverseInertiaDiagonal().GetZ();
    }
    else
      _mass = ixx = iyy = izz = 0.f;
  });
}

struct JoltPhysNativeShape : public PhysCollision
{
  JPH::RefConst<JPH::Shape> shape;
  JPH::Vec3 localScale;

  JoltPhysNativeShape(JPH::RefConst<JPH::Shape> s, JPH::Vec3 ls) : PhysCollision(TYPE_NATIVE_SHAPE), shape(s), localScale(ls) {}
};
void PhysCollision::clearNativeShapeData(PhysCollision &c) { static_cast<JoltPhysNativeShape &>(c).shape = nullptr; }

JPH::Ref<JPH::Shape> check_and_return_shape(JPH::ShapeSettings::ShapeResult res, int ln)
{
  if (DAGOR_UNLIKELY(res.HasError()))
  {
    G_ASSERT_LOG(0, "Shape error: %s in %s:%d", res.GetError().c_str(), __FILE__, ln);
    JPH::EmptyShapeSettings empty;
    return empty.Create().Get();
  }
  return res.Get();
}

JPH::RefConst<JPH::Shape> PhysBody::create_jolt_collision_shape(const PhysCollision *c, bool dynamic)
{
  if (!c || c->collType == c->TYPE_EMPTY ||
      (c->collType == c->TYPE_COMPOUND && !static_cast<const PhysCompoundCollision *>(c)->coll.size()))
    return nullptr;
  auto zeroDensityIfStatic = [dynamic](JPH::Ref<JPH::Shape> shape) {
    if (!dynamic && shape)
    {
      G_FAST_ASSERT(shape->GetType() == JPH::EShapeType::Convex);
      static_cast<JPH::ConvexShape *>(shape.GetPtr())->SetDensity(0.f); // Note: this would zero mass & COM
    }
    return shape;
  };
  switch (c->collType)
  {
    case PhysCollision::TYPE_SPHERE:
    {
      auto sphereColl = static_cast<const PhysSphereCollision *>(c);
      return zeroDensityIfStatic(new JPH::SphereShape(sphereColl->radius));
    }
    break;
    case PhysCollision::TYPE_BOX:
    {
      auto boxColl = static_cast<const PhysBoxCollision *>(c);
      return zeroDensityIfStatic(new JPH::BoxShape(to_jVec3(boxColl->size / 2), jolt_api::min_convex_rad_for_box(boxColl->size)));
    }
    break;

    case PhysCollision::TYPE_CAPSULE:
    {
      auto capsuleColl = static_cast<const PhysCapsuleCollision *>(c);
      if (capsuleColl->height <= 0.f) // sphere is degenerate capsule
      {
        if (capsuleColl->height < 0)
          logerr("%s attempt to create capsule with height %.12f, fallback to sphere", __FUNCTION__, capsuleColl->height);
        return zeroDensityIfStatic(new JPH::SphereShape(capsuleColl->radius));
      }
      auto shape = zeroDensityIfStatic(new JPH::CapsuleShape(capsuleColl->height * 0.5f, capsuleColl->radius));
      switch (capsuleColl->dirIdx)
      {
        case 0: return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), JPH::Quat(0, 0, M_SQRT1_2, M_SQRT1_2), shape);
        case 1: return shape;
        case 2: return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), JPH::Quat(M_SQRT1_2, 0, 0, M_SQRT1_2), shape);
        default: G_ASSERTF_RETURN(false, nullptr, "capsuleColl->dirIdx=%d, must be [0..2]", capsuleColl->dirIdx);
      }
    }
    break;

    case PhysCollision::TYPE_CYLINDER:
    {
      auto cylColl = static_cast<const PhysCylinderCollision *>(c);
      float hh = cylColl->height * 0.5f;
      float convexRadius = eastl::min({hh, cylColl->radius, JPH::cDefaultConvexRadius});
      auto shape = zeroDensityIfStatic(new JPH::CylinderShape(hh, cylColl->radius, convexRadius));
      switch (cylColl->dirIdx)
      {
        case 0: return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), JPH::Quat(0, 0, M_SQRT1_2, M_SQRT1_2), shape);
        case 1: return shape;
        case 2: return new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), JPH::Quat(M_SQRT1_2, 0, 0, M_SQRT1_2), shape);
        default: G_ASSERTF_RETURN(false, nullptr, "cylColl->dirIdx=%d, must be [0..2]", cylColl->dirIdx);
      }
    }
    break;

    case PhysCollision::TYPE_TRIMESH:
    {
      auto meshColl = static_cast<const PhysTriMeshCollision *>(c);
      JPH::MeshShapeSettings shape;
      shape.mTriangleVertices.resize(meshColl->vnum);
      shape.mIndexedTriangles.resize(meshColl->inum / 3);
      Point3 scl = meshColl->scale;
      int vstride = meshColl->vstride;
      bool rev_face = meshColl->revNorm;

      if (meshColl->vtypeShort)
      {
        JPH::Float3 *d = shape.mTriangleVertices.data();
        for (auto s = (const char *)meshColl->vdata, se = s + meshColl->vnum * vstride; s < se; s += vstride, d++)
          d->x = ((uint16_t *)s)[0] * scl.x, d->y = ((uint16_t *)s)[1] * scl.y, d->z = ((uint16_t *)s)[2] * scl.z;
      }
      else
      {
        JPH::Float3 *d = shape.mTriangleVertices.data();
        for (auto s = (const char *)meshColl->vdata, se = s + meshColl->vnum * vstride; s < se; s += vstride, d++)
          d->x = ((float *)s)[0] * scl.x, d->y = ((float *)s)[1] * scl.y, d->z = ((float *)s)[2] * scl.z;
      }
      if (meshColl->istride == 2)
      {
        JPH::IndexedTriangle *d = shape.mIndexedTriangles.data();
        for (auto s = (const unsigned short *)meshColl->idata, se = s + meshColl->inum; s < se; s += 3, d++)
          d->mIdx[0] = s[0], d->mIdx[1] = s[rev_face ? 2 : 1], d->mIdx[2] = s[rev_face ? 1 : 2];
      }
      else
      {
        JPH::IndexedTriangle *d = shape.mIndexedTriangles.data();
        for (auto s = (const unsigned *)meshColl->idata, se = s + meshColl->inum; s < se; s += 3, d++)
          d->mIdx[0] = s[0], d->mIdx[1] = s[rev_face ? 2 : 1], d->mIdx[2] = s[rev_face ? 1 : 2];
      }

      auto res = shape.Create();
      if (DAGOR_LIKELY(res.IsValid()))
        return res.Get();

      logerr("Failed to create non sanitized mesh shape <%s>: %s", meshColl->debugName, res.GetError().c_str());

      decltype(shape) sanitizedShape;
      sanitizedShape.mTriangleVertices = eastl::move(shape.mTriangleVertices);
      sanitizedShape.mIndexedTriangles = eastl::move(shape.mIndexedTriangles);
      sanitizedShape.Sanitize();
      return check_and_return_shape(sanitizedShape.Create(), __LINE__);
    }
    break;

    case PhysCollision::TYPE_CONVEXHULL:
    {
      // convexColl->build is unused
      auto convexColl = static_cast<const PhysConvexHullCollision *>(c);
      if (convexColl->vnum < 3)
      {
        logerr("convex of %d points is replaced with point-sphere", convexColl->vnum);
        return new JPH::RotatedTranslatedShape(*(const JPH::Vec3 *)convexColl->vdata, JPH::Quat::sIdentity(),
          zeroDensityIfStatic(new JPH::SphereShape(1e-3)));
      }
      if (convexColl->vstride == sizeof(JPH::Vec3))
      {
        JPH::ConvexHullShapeSettings chullShapeSett((const JPH::Vec3 *)convexColl->vdata, convexColl->vnum);
        chullShapeSett.mHullTolerance = convexColl->hullTolerance;
        return zeroDensityIfStatic(check_and_return_shape(chullShapeSett.Create(), __LINE__));
      }

      int fstride = convexColl->vstride / 4;
      Tab<JPH::Vec3> v;
      v.resize(convexColl->vnum);
      JPH::Vec3 *d = v.data();
      for (auto s = (const float *)convexColl->vdata, se = s + convexColl->vnum * fstride; s < se; s += fstride, d++)
        d->SetX(s[0]), d->SetY(s[1]), d->SetZ(s[2]);

      JPH::ConvexHullShapeSettings chullShapeSett(v.data(), v.size());
      chullShapeSett.mHullTolerance = convexColl->hullTolerance;
      return zeroDensityIfStatic(check_and_return_shape(chullShapeSett.Create(), __LINE__));
    }
    break;

    case PhysCollision::TYPE_HEIGHTFIELD:
    {
      auto hmapColl = static_cast<const PhysHeightfieldCollision *>(c);
      auto hmap = hmapColl->hmap;
      G_ASSERTF_RETURN(hmap->getHeightmapSizeX() == hmap->getHeightmapSizeY(), nullptr, "HMAP %dx%d is not square",
        hmap->getHeightmapSizeX(), hmap->getHeightmapSizeY());
      float cellSz = hmap->getHeightmapCellSize();
      // hmapColl->hmStep ?
      return check_and_return_shape(JPH::HeightField16ShapeSettings(&hmap->getCompressedData(), hmapColl->holes,
                                      JPH::Vec3(hmapColl->ofs.x, hmap->getHeightMin(), hmapColl->ofs.y),
                                      JPH::Vec3(cellSz, hmap->getHeightScaleRaw() * hmapColl->scale, cellSz))
                                      .Create(),
        __LINE__);
    }
    break;

    case PhysCollision::TYPE_COMPOUND:
    {
      auto compColl = static_cast<const PhysCompoundCollision *>(c);
      if (compColl->coll.size() == 1)
      {
        const auto &cc = compColl->coll[0];
        const int ccType = cc.c->collType;
        const bool isIdentM3 = memcmp(&cc.m, &TMatrix::IDENT, sizeof(Point3) * 3) == 0; //-V1014 //-V1086
        const bool hasTranslation = cc.m.getcol(3).lengthSq() > 1e-6;
        JPH::RefConst<JPH::Shape> s = PhysBody::create_jolt_collision_shape(cc.c, dynamic);
        G_ASSERTF_RETURN(s.GetPtr(), nullptr, "failed to create collision type=%d", ccType);

        if (!hasTranslation && (isIdentM3 || ccType == c->TYPE_SPHERE))
          return s;
        return new JPH::RotatedTranslatedShape(to_jVec3(cc.m.getcol(3)), isIdentM3 ? JPH::Quat::sIdentity() : to_jQuat(cc.m), s);
      }

      JPH::StaticCompoundShapeSettings shape;
      for (auto &cc : compColl->coll)
        switch (cc.c->collType)
        {
          case PhysCollision::TYPE_SPHERE:
          case PhysCollision::TYPE_BOX:
          case PhysCollision::TYPE_CAPSULE:
          case PhysCollision::TYPE_CYLINDER:
          case PhysCollision::TYPE_TRIMESH:
          case PhysCollision::TYPE_CONVEXHULL:
          case PhysCollision::TYPE_NATIVE_SHAPE:
            if (auto subShape = PhysBody::create_jolt_collision_shape(cc.c, dynamic))
              shape.AddShape(to_jVec3(cc.m.getcol(3)), to_jQuat(cc.m), subShape);
            break;

          default: G_ASSERTF(0, "unsupported collision type %d (in compound)", cc.c->collType);
        }
      return check_and_return_shape(shape.Create(), __LINE__);
    }

    case PhysCollision::TYPE_NATIVE_SHAPE:
    {
      auto ns = static_cast<const JoltPhysNativeShape *>(c);
      if ((ns->localScale - JPH::Vec3::sReplicate(1.0f)).LengthSq() < 1e-6f)
        return ns->shape;
      return check_and_return_shape(JPH::ScaledShapeSettings(ns->shape, ns->localScale).Create(), __LINE__);
    }
    break;

    default: G_ASSERTF(0, "unsupported collision type %d", c->collType);
  }
  return nullptr;
}

void PhysBody::setCollision(const PhysCollision *coll, bool /*allow_fast_inaccurate_coll_tm*/)
{
  if (auto shape = PhysBody::create_jolt_collision_shape(coll, jolt_api::body_lock().TryGetBody(bodyId)->IsDynamic()))
    api().SetShape(bodyId, shape, false, isInWorld() ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}

// Note: some Jolt shapes (e.g. spheres/capsules/cylinders) can only be scaled uniformly, this code ensures that
static JPH::RefConst<JPH::Shape> create_uscaled_shape(const JPH::Shape *shape, JPH::Vec3 scale)
{
  using namespace JPH;
  G_FAST_ASSERT(shape->GetType() != EShapeType::Compound);
  const Shape *innerShape = shape;
  dag::RelocatableFixedVector<const Shape *, 2, true, framemem_allocator> shapes;
  for (const Shape *dsh = shape; dsh->GetSubType() == EShapeSubType::RotatedTranslated;
       dsh = static_cast<const DecoratedShape *>(dsh)->GetInnerShape())
  {
    innerShape = static_cast<const DecoratedShape *>(dsh)->GetInnerShape();
    shapes.push_back(innerShape);
  }
  Shape *scaledShape = nullptr;
  switch (innerShape->GetSubType())
  {
    case EShapeSubType::Sphere:
      scaledShape = new SphereShape(static_cast<const SphereShape *>(innerShape)->GetRadius() * scale.GetX());
      break;
    case EShapeSubType::Capsule:
    {
      auto capsh = static_cast<const CapsuleShape *>(innerShape);
      scaledShape = new CapsuleShape(capsh->GetHalfHeightOfCylinder() * scale.GetY(), capsh->GetRadius() * scale.GetX());
    }
    break;
    case EShapeSubType::Cylinder:
    {
      auto cylsh = static_cast<const CylinderShape *>(innerShape);
      float rad = cylsh->GetRadius() * scale.GetX(), hh = cylsh->GetHalfHeight() * scale.GetY();
      float cvxrad = eastl::min({hh, rad, cDefaultConvexRadius});
      scaledShape = new CylinderShape(hh, rad, cvxrad);
    }
    break;
    default: return check_and_return_shape(ScaledShapeSettings(shape, scale).Create(), __LINE__);
  }
  G_ASSERT(scaledShape);
  if (shape == innerShape) // No rt shapes
    shape = scaledShape;
  else // Reconstruct chain of rt shapes (e.g. "RT -> RT -> capsule")
  {
    G_ASSERT(!shapes.empty());
    G_ASSERT(shapes.back() == innerShape);
    G_ASSERT(innerShape->GetType() != EShapeType::Decorated);
    auto copyRTShape = [](const Shape *rtshape, const Shape *ishape) {
      G_ASSERT(rtshape->GetSubType() == EShapeSubType::RotatedTranslated);
      auto rtsh = static_cast<const RotatedTranslatedShape *>(rtshape);
      return new RotatedTranslatedShape(rtsh->GetPosition(), rtsh->GetRotation(), ishape);
    };
    for (int i = (int)shapes.size() - 2; i >= 0; --i)
      scaledShape = copyRTShape(shapes[i], scaledShape);
    shape = copyRTShape(shape, scaledShape);
  }
  return shape;
}

static JPH::RefConst<JPH::Shape> create_uscaled_shape_r(const JPH::Shape *shape, JPH::Vec3 scale)
{
  using namespace JPH;
  if (shape->GetType() != EShapeType::Compound)
    return create_uscaled_shape(shape, scale);
  else
  {
    auto compound = static_cast<const CompoundShape *>(shape);
    const Vec3 ccom = compound->GetCenterOfMass();
    StaticCompoundShapeSettings compShapeSett;
    bool hasAnyNonScaledShapes = false;
    for (auto &ss : compound->GetSubShapes())
    {
      Vec3 pos = (ss.GetPositionCOM() + ccom) - ss.GetRotation() * ss.mShape->GetCenterOfMass();
      RefConst<Shape> scaledShape = create_uscaled_shape_r(ss.mShape, scale);
      hasAnyNonScaledShapes |= scaledShape->GetSubType() != EShapeSubType::Scaled;
      compShapeSett.AddShape(pos, ss.GetRotation(), scaledShape);
    }
    if (hasAnyNonScaledShapes)
      return check_and_return_shape(compShapeSett.Create(), __LINE__);
    else // All are scaled -> no spheres/capsules/boxes -> just scale compound
      return check_and_return_shape(ScaledShapeSettings(shape, scale).Create(), __LINE__);
  }
}

PhysCollision *PhysBody::getCollisionScaledCopy(const Point3 &scale)
{
  using namespace JPH;
  auto shape = getShape();
  auto jscale = to_jVec3(scale);
  if (!shape || ScaleHelpers::IsUniformScale(jscale))
    return shape ? new JoltPhysNativeShape(shape, jscale) : nullptr;
  return new JoltPhysNativeShape(create_uscaled_shape_r(shape, jscale), Vec3::sReplicate(1.f));
}

void PhysBody::patchCollisionScaledCopy(const Point3 &s, PhysBody *)
{
  if (auto shape = getShape())
  {
    if (shape->GetSubType() == JPH::EShapeSubType::Scaled)
      shape = static_cast<const JPH::ScaledShape *>(shape.GetPtr())->GetInnerShape();
    api().SetShape(bodyId, check_and_return_shape(JPH::ScaledShapeSettings(shape, to_jVec3(s)).Create(), __LINE__), false,
      isInWorld() ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  }
}

void PhysBody::setSphereShapeRad(float rad)
{
  if (auto shape = getShape().GetPtr())
  {
    using namespace JPH;
    G_ASSERT(shape->GetSubType() == JPH::EShapeSubType::Sphere);
    auto sphere = (SphereShape *)shape;
    if (fabsf(sphere->GetRadius() - rad) < 1e-6f)
      return;
    G_ASSERT(sphere->GetRefCount() == 1); // Must be unique for next trick
    sphere->SetEmbedded();                // Don't complain in dtor about dangling ref
    sphere->Release();
    sphere->~SphereShape();
    (new (sphere, _NEW_INPLACE) SphereShape(rad))->AddRef();
    if (isInWorld())
      api().NotifyShapeChanged(bodyId, Vec3::sZero(), false, EActivation::Activate);
  }
}
void PhysBody::setBoxShapeExtents(const Point3 &ext)
{
  if (auto shape = getShape())
    if (shape->GetSubType() == JPH::EShapeSubType::Box &&
        lengthSq(to_point3(static_cast<const JPH::BoxShape *>(shape.GetPtr())->GetHalfExtent()) - ext * 0.5f) > 1e-12f)
      api().SetShape(bodyId, new JPH::BoxShape(to_jVec3(ext * 0.5f), jolt_api::min_convex_rad_for_box(ext)), false,
        isInWorld() ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}
void PhysBody::setVertCapsuleShapeSize(float cap_rad, float cap_cyl_ht)
{
  if (auto shape = getShape())
    if (shape->GetSubType() == JPH::EShapeSubType::Capsule &&
        ((fabsf(static_cast<const JPH::CapsuleShape *>(shape.GetPtr())->GetHalfHeightOfCylinder() - cap_cyl_ht * 0.5f) > 1e-6f) ||
          fabsf(static_cast<const JPH::CapsuleShape *>(shape.GetPtr())->GetRadius() - cap_rad) > 1e-6f))
      api().SetShape(bodyId, new JPH::CapsuleShape(cap_cyl_ht * 0.5f, cap_rad), false,
        isInWorld() ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}

bool PhysBody::isConvexCollisionShape() const
{
  if (auto shape = getShape())
    return shape->GetType() == JPH::EShapeType::Convex;
  return false;
}

void PhysBody::getShapeAabb(Point3 &out_bb_min, Point3 &out_bb_max) const
{
  if (auto shape = getShape())
  {
    JPH::AABox aabb = shape->GetLocalBounds();
    aabb.Translate(shape->GetCenterOfMass());
    out_bb_min = to_point3(aabb.mMin);
    out_bb_max = to_point3(aabb.mMax);
  }
}

void PhysBody::setMaterialId(int id)
{
  const PhysWorld::Material *material = world->getMaterial(safeMaterialId = id);

  if (!material)
  {
    material = world->getMaterial(0);
    G_ASSERTF_RETURN(material, , "material id %d", id);
  }
  if (!bodyId.IsInvalid())
    lockRW([m = material](JPH::Body &b) {
      b.SetFriction(m->friction);
      b.SetRestitution(m->restitution);
    });
}

// Should be called after setCollision() to set layer mask for all collision elements.
void PhysBody::setGroupAndLayerMask(unsigned int group, unsigned int mask)
{
  groupMask = group, layerMask = mask;
  JPH::ObjectLayer objl = api().GetObjectLayer(bodyId);
  api().SetObjectLayer(bodyId, make_objlayer_debris(objlayer_to_bphlayer(objl), groupMask, layerMask));
}

void PhysBody::setGravity(const Point3 &grav, bool activate)
{
  // Only scaling gravity along y axis is supported here.
  float currentGlobalGravity = jolt_api::physicsSystem->GetGravity().GetY();
  if (currentGlobalGravity == 0.f)
    return;
  float desiredGravityFactor = grav.y / currentGlobalGravity;
  api().SetGravityFactor(bodyId, desiredGravityFactor);
}

void PhysBody::setDamping(float linDamping, float angDamping)
{
  lockRW([&](JPH::Body &b) {
    if (!b.IsStatic())
    {
      auto &mp = *b.GetMotionProperties();
      mp.SetLinearDamping(linDamping);
      mp.SetAngularDamping(angDamping);
    }
  });
}

void PhysBody::addImpulse(const Point3 &p, const Point3 &force_x_dt, bool activate)
{
  api().AddImpulse(bodyId, to_jVec3(force_x_dt), to_jVec3(p));
}

void PhysBody::addForce(const Point3 &p, const Point3 &force, bool activate)
{
  if (!bodyId.IsInvalid())
    lockRW([p, force](JPH::Body &b) { b.AddForce(to_jVec3(force), to_jVec3(p)); });
}


void PhysBody::setContinuousCollisionMode(bool use)
{
  api().SetMotionQuality(bodyId, use ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
}

void PhysBody::setCcdParams(float rad, float threshold) {}

////////////////////////////////////////////////////////////

PhysJoint::~PhysJoint()
{
  G_ASSERT(joint);
  jolt_api::phys_sys().RemoveConstraint(joint);
}

PhysRagdollHingeJoint::PhysRagdollHingeJoint(PhysBody *body1, PhysBody *body2, const Point3 &pos, const Point3 &axis,
  const Point3 &mid_axis, const Point3 & /*x_axis*/, real ang_limit, real damping, real /*sleep_threshold*/) :
  PhysJoint(PJ_HINGE)
{
  // By default HingeConstraintSettings is in world space so we can provide world
  // space here without need for inverses
  JPH::HingeConstraintSettings hingeSettings;
  hingeSettings.mDrawConstraintSize = drawConstraintSize;
  hingeSettings.mPoint1 = hingeSettings.mPoint2 = to_jVec3(pos);
  hingeSettings.mHingeAxis1 = hingeSettings.mHingeAxis2 = to_jVec3(axis);
  hingeSettings.mNormalAxis1 = to_jVec3(mid_axis);
  hingeSettings.mNormalAxis2 = to_jVec3(makeTM(axis, ang_limit) % mid_axis);
  hingeSettings.mLimitsMin = -ang_limit;
  hingeSettings.mLimitsMax = +ang_limit;
  hingeSettings.mMaxFrictionTorque = damping * 0.2;

  JPH::Body *b1 = jolt_api::body_lock().TryGetBody(body1->bodyId);
  JPH::Body *b2 = jolt_api::body_lock().TryGetBody(body2->bodyId);
  G_ASSERTF(b1 && b2, "%p->bodyId=0x%x (%p)  %p->bodyId=0x%x (%p)", body1, body1->bodyId.GetIndexAndSequenceNumber(), b1, body2,
    body2->bodyId.GetIndexAndSequenceNumber(), b2);

  JPH::Mat44 body2Inertia = b2->GetMotionProperties()->GetLocalSpaceInverseInertia().Inversed3x3();
  float inertiaFromConstraint = (body2Inertia * to_jVec3(axis)).Length();

  JPH::HingeConstraint *hinge = static_cast<JPH::HingeConstraint *>(hingeSettings.Create(*b1, *b2));
  jolt_api::phys_sys().AddConstraint(joint = hinge);
}

PhysRagdollBallJoint::PhysRagdollBallJoint(PhysBody *body1, PhysBody *body2, const TMatrix &_tm, const Point3 &min_limit,
  const Point3 &max_limit, real damping, real /*twist_damping*/, real /*sleep_threshold*/) :
  PhysJoint(PJ_CONE_TWIST)
{
  // By default SwingTwistConstraintSettings is in world space so we can provide world
  // space here without need for inverses
  JPH::SwingTwistConstraintSettings ballSettings;
  TMatrix rot1 = makeTM(_tm.getcol(1), -(fabsf(max_limit.z) - fabsf(min_limit.z)) * 0.5f);
  Point3 axNorm = rot1 % _tm.getcol(2);
  TMatrix rot2 = makeTM(axNorm, -(fabsf(max_limit.y) - fabsf(min_limit.y)) * 0.5f);
  Point3 axTwist = rot2 % (rot1 % _tm.getcol(0));
  ballSettings.mDrawConstraintSize = drawConstraintSize;
  ballSettings.mPosition1 = ballSettings.mPosition2 = to_jVec3(_tm.getcol(3));
  ballSettings.mTwistAxis1 = to_jVec3(axTwist);
  ballSettings.mPlaneAxis1 = to_jVec3(axNorm);
  ballSettings.mTwistAxis2 = to_jVec3(_tm.getcol(0));
  ballSettings.mPlaneAxis2 = to_jVec3(_tm.getcol(2));
  ballSettings.mNormalHalfConeAngle = (fabsf(max_limit.y) + fabsf(min_limit.y)) * 0.5f;
  ballSettings.mPlaneHalfConeAngle = (fabsf(max_limit.z) + fabsf(min_limit.z)) * 0.5f;
  ballSettings.mTwistMinAngle = min_limit.x;
  ballSettings.mTwistMaxAngle = max_limit.x;
  ballSettings.mMaxFrictionTorque = damping * 0.1;
  // default motor settings taken from https://github.com/jrouwe/JoltPhysics/blob/master/Assets/Human.tof
  ballSettings.mSwingMotorSettings.mSpringSettings.mFrequency = 20.0;
  ballSettings.mSwingMotorSettings.mSpringSettings.mDamping = 2.0;
  ballSettings.mTwistMotorSettings.mSpringSettings.mFrequency = 20.0;
  ballSettings.mTwistMotorSettings.mSpringSettings.mDamping = 2.0;

  JPH::Body *b1 = jolt_api::body_lock().TryGetBody(body1->bodyId);
  JPH::Body *b2 = jolt_api::body_lock().TryGetBody(body2->bodyId);
  G_ASSERTF(b1 && b2, "%p->bodyId=0x%x (%p)  %p->bodyId=0x%x (%p)", body1, body1->bodyId.GetIndexAndSequenceNumber(), b1, body2,
    body2->bodyId.GetIndexAndSequenceNumber(), b2);

  JPH::SwingTwistConstraint *constr = static_cast<JPH::SwingTwistConstraint *>(ballSettings.Create(*b1, *b2));
  jolt_api::phys_sys().AddConstraint(joint = constr);
}

void PhysRagdollBallJoint::setTargetOrientation(const TMatrix &tm)
{
  auto constraint = static_cast<JPH::SwingTwistConstraint *>(joint);
  constraint->SetSwingMotorState(JPH::EMotorState::Position);
  constraint->SetTwistMotorState(JPH::EMotorState::Position);
  constraint->SetTargetOrientationBS(to_jQuat(tm));
}

void PhysRagdollBallJoint::setTwistSwingMotorSettings(float twistFrequency, float twistDamping, float swingFrequency,
  float swingDamping)
{
  auto constraint = static_cast<JPH::SwingTwistConstraint *>(joint);

  JPH::MotorSettings &twistMotorSettings = constraint->GetTwistMotorSettings();
  twistMotorSettings.mSpringSettings.mFrequency = twistFrequency;
  twistMotorSettings.mSpringSettings.mDamping = twistDamping;

  JPH::MotorSettings &swingMotorSettings = constraint->GetSwingMotorSettings();
  swingMotorSettings.mSpringSettings.mFrequency = swingFrequency;
  swingMotorSettings.mSpringSettings.mDamping = swingDamping;
}

Phys6DofJoint::Phys6DofJoint(PhysBody *body1, PhysBody *body2, const TMatrix &frame1, const TMatrix &frame2) : PhysJoint(PJ_6DOF)
{
  G_ASSERT(body1);

  // "You can use Body::sFixedToWorld for inBody1 if you want to attach inBody2 to the world"
  if (!body2)
    eastl::swap(body1, body2);

  JPH::Body *b1 = body1 ? jolt_api::body_lock().TryGetBody(body1->bodyId) : &JPH::Body::sFixedToWorld;
  JPH::Body *b2 = jolt_api::body_lock().TryGetBody(body2->bodyId);
  G_ASSERTF(b1 && b2, "%p->bodyId=0x%x (%p)  %p->bodyId=0x%x (%p)", body1, body1 ? body1->bodyId.GetIndexAndSequenceNumber() : 0, b1,
    body2, body2 ? body2->bodyId.GetIndexAndSequenceNumber() : 0, b2);

  JPH::SixDOFConstraintSettings sixDofSettings;
  sixDofSettings.mDrawConstraintSize = drawConstraintSize;
  sixDofSettings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

  // FIXME: rework to init using struct instead of methods (setLimit, etc..) and remove this hardcode
  using EAxis = JPH::SixDOFConstraint::EAxis;
  sixDofSettings.MakeFixedAxis(EAxis::TranslationX);
  sixDofSettings.SetLimitedAxis(EAxis::TranslationY, 0.f, 1.f);
  sixDofSettings.MakeFixedAxis(EAxis::TranslationZ);

  if (!body1)
  {
    JPH::Mat44 b2tm = b2->GetCenterOfMassTransform() * to_jMat44(frame1);
    sixDofSettings.mPosition1 = b2tm.GetColumn3(3);
    sixDofSettings.mAxisX1 = b2tm.GetColumn3(0);
    sixDofSettings.mAxisY1 = b2tm.GetColumn3(1);
    sixDofSettings.mPosition2 = to_jVec3(frame1.getcol(3));
    sixDofSettings.mAxisX2 = to_jVec3(frame1.getcol(0));
    sixDofSettings.mAxisY2 = to_jVec3(frame1.getcol(1));
  }
  else
  {
    sixDofSettings.mPosition1 = to_jVec3(frame1.getcol(3));
    sixDofSettings.mAxisX1 = to_jVec3(frame1.getcol(0));
    sixDofSettings.mAxisY1 = to_jVec3(frame1.getcol(1));
    sixDofSettings.mPosition2 = to_jVec3(frame2.getcol(3));
    sixDofSettings.mAxisX2 = to_jVec3(frame2.getcol(0));
    sixDofSettings.mAxisY2 = to_jVec3(frame2.getcol(1));
  }

  auto constr = static_cast<JPH::SixDOFConstraint *>(sixDofSettings.Create(*b1, *b2));
  jolt_api::phys_sys().AddConstraint(joint = constr);
}
void Phys6DofJoint::setLimit(int index, const Point2 &limits)
{
  using EAxis = JPH::SixDOFConstraint::EAxis;
  auto constr = static_cast<JPH::SixDOFConstraint *>(joint);
  G_ASSERT((unsigned)index < EAxis::Num);
  if (index < EAxis::NumTranslation)
    constr->SetTranslationLimits(JPH::Vec3(index == EAxis::TranslationX ? limits[0] : constr->GetLimitsMin(EAxis::TranslationX),
                                   index == EAxis::TranslationY ? limits[0] : constr->GetLimitsMin(EAxis::TranslationY),
                                   index == EAxis::TranslationZ ? limits[0] : constr->GetLimitsMin(EAxis::TranslationZ)),
      JPH::Vec3(index == EAxis::TranslationX ? limits[1] : constr->GetLimitsMax(EAxis::TranslationX),
        index == EAxis::TranslationY ? limits[1] : constr->GetLimitsMax(EAxis::TranslationY),
        index == EAxis::TranslationZ ? limits[1] : constr->GetLimitsMax(EAxis::TranslationZ)));
  else
    constr->SetRotationLimits(JPH::Vec3(index == EAxis::RotationX ? limits[0] : constr->GetLimitsMin(EAxis::RotationX),
                                index == EAxis::RotationY ? limits[0] : constr->GetLimitsMin(EAxis::RotationY),
                                index == EAxis::RotationZ ? limits[0] : constr->GetLimitsMin(EAxis::RotationZ)),
      JPH::Vec3(index == EAxis::RotationX ? limits[1] : constr->GetLimitsMax(EAxis::RotationX),
        index == EAxis::RotationY ? limits[1] : constr->GetLimitsMax(EAxis::RotationY),
        index == EAxis::RotationZ ? limits[1] : constr->GetLimitsMax(EAxis::RotationZ)));
}
void Phys6DofJoint::setParam(int num, float value, int axis)
{
  //==
}
void Phys6DofJoint::setAxisDamping(int index, float damping)
{
  using EAxis = JPH::SixDOFConstraint::EAxis;
  auto constr = static_cast<JPH::SixDOFConstraint *>(joint);
  G_ASSERT((unsigned)index < EAxis::Num);

  static const float jointAngularDampingToJoltFrictionMul = 10.0f;
  if (index >= EAxis::NumTranslation)
  { // Jolt friction is set in Torque units, so 1 is not the maximum value (unlike damping)
    // TODO: Linear axes likely also need some correction here
    damping *= jointAngularDampingToJoltFrictionMul;
  }
  constr->SetMaxFriction((EAxis)index, damping);
}


Phys6DofSpringJoint::Phys6DofSpringJoint(PhysBody *body1, PhysBody *body2, const TMatrix &frame1, const TMatrix &frame2) :
  PhysJoint(PJ_6DOF_SPRING)
{
  JPH::SixDOFConstraintSettings sixDofSettings;
  sixDofSettings.mDrawConstraintSize = drawConstraintSize;
  sixDofSettings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

  // FIXME: rework to init using struct instead of methods (setLimit, etc..) and remove this hardcode
  using EAxis = JPH::SixDOFConstraint::EAxis;
  for (int i = 0; i < EAxis::NumTranslation; ++i)
    sixDofSettings.MakeFixedAxis((EAxis)i);
  sixDofSettings.MakeFixedAxis(EAxis::RotationY);

  sixDofSettings.mPosition1 = to_jVec3(frame1.getcol(3));
  sixDofSettings.mAxisX1 = to_jVec3(frame1.getcol(0));
  sixDofSettings.mAxisY1 = to_jVec3(frame1.getcol(1));
  sixDofSettings.mPosition2 = to_jVec3(frame2.getcol(3));
  sixDofSettings.mAxisX2 = to_jVec3(frame2.getcol(0));
  sixDofSettings.mAxisY2 = to_jVec3(frame2.getcol(1));

  JPH::Body *b1 = body1 ? jolt_api::body_lock().TryGetBody(body1->bodyId) : &JPH::Body::sFixedToWorld;
  JPH::Body *b2 = body2 ? jolt_api::body_lock().TryGetBody(body2->bodyId) : &JPH::Body::sFixedToWorld;
  G_ASSERTF(b1 && b2, "%p->bodyId=0x%x (%p)  %p->bodyId=0x%x (%p)", body1, body1 ? body1->bodyId.GetIndexAndSequenceNumber() : 0, b1,
    body2, body2 ? body2->bodyId.GetIndexAndSequenceNumber() : 0, b2);

  auto constr = static_cast<JPH::SixDOFConstraint *>(sixDofSettings.Create(*b1, *b2));
  jolt_api::phys_sys().AddConstraint(joint = constr);
}
Phys6DofSpringJoint::Phys6DofSpringJoint(PhysBody *body1, PhysBody *body2, const TMatrix &in_tm) :
  Phys6DofSpringJoint(body1, body2, inverse(to_tmatrix(jolt_api::body_api().GetWorldTransform(body1->bodyId))) * in_tm,
    inverse(to_tmatrix(jolt_api::body_api().GetWorldTransform(body2->bodyId))) * in_tm)
{}
void Phys6DofSpringJoint::setSpring(int index, bool on, float stiffness, float damping, const Point2 &limits)
{
  using EAxis = JPH::SixDOFConstraint::EAxis;
  auto constr = static_cast<JPH::SixDOFConstraint *>(joint);
  // "Only soft translation limits are supported, soft rotation limits are not currently supported."
  if ((unsigned)index < EAxis::NumTranslation)
    constr->SetLimitsSpringSettings((EAxis)index,
      on ? JPH::SpringSettings{JPH::ESpringMode::StiffnessAndDamping, stiffness, damping} : JPH::SpringSettings{});
  if (on)
    setLimit(index, limits);
}
void Phys6DofSpringJoint::setLimit(int index, const Point2 &limits)
{
  using EAxis = JPH::SixDOFConstraint::EAxis;
  auto constr = static_cast<JPH::SixDOFConstraint *>(joint);
  G_ASSERT((unsigned)index < EAxis::Num);
  if (index < EAxis::NumTranslation)
    constr->SetTranslationLimits(JPH::Vec3(index == EAxis::TranslationX ? limits[0] : constr->GetLimitsMin(EAxis::TranslationX),
                                   index == EAxis::TranslationY ? limits[0] : constr->GetLimitsMin(EAxis::TranslationY),
                                   index == EAxis::TranslationZ ? limits[0] : constr->GetLimitsMin(EAxis::TranslationZ)),
      JPH::Vec3(index == EAxis::TranslationX ? limits[1] : constr->GetLimitsMax(EAxis::TranslationX),
        index == EAxis::TranslationY ? limits[1] : constr->GetLimitsMax(EAxis::TranslationY),
        index == EAxis::TranslationZ ? limits[1] : constr->GetLimitsMax(EAxis::TranslationZ)));
  else
    constr->SetRotationLimits(JPH::Vec3(index == EAxis::RotationX ? limits[0] : constr->GetLimitsMin(EAxis::RotationX),
                                index == EAxis::RotationY ? limits[0] : constr->GetLimitsMin(EAxis::RotationY),
                                index == EAxis::RotationZ ? limits[0] : constr->GetLimitsMin(EAxis::RotationZ)),
      JPH::Vec3(index == EAxis::RotationX ? limits[1] : constr->GetLimitsMax(EAxis::RotationX),
        index == EAxis::RotationY ? limits[1] : constr->GetLimitsMax(EAxis::RotationY),
        index == EAxis::RotationZ ? limits[1] : constr->GetLimitsMax(EAxis::RotationZ)));
}
void Phys6DofSpringJoint::setEquilibriumPoint()
{
  //==
}
void Phys6DofSpringJoint::setParam(int num, float value, int axis)
{
  //==
}
void Phys6DofSpringJoint::setAxisDamping(int index, float damping)
{
  using EAxis = JPH::SixDOFConstraint::EAxis;
  auto constr = static_cast<JPH::SixDOFConstraint *>(joint);
  G_ASSERT((unsigned)index < EAxis::Num);
  constr->SetMaxFriction((EAxis)index, damping);
}
////////////////////////////


PhysRayCast::PhysRayCast(const Point3 &p, const Point3 &d, real max_length, PhysWorld *world)
{
  ownWorld = world;
  pos = to_jVec3(p);
  dir = to_jVec3(d);
  maxLength = max_length;
}


void PhysRayCast::forceUpdate(PhysWorld *world)
{
  if (!world)
    world = ownWorld;
  G_ASSERT(world);

  using namespace JPH;
  RRayCast ray{pos, dir * maxLength};
  hit = RayCastResult{};
  BroadPhaseLayer blr((filterMask == StaticFilter) ? layers::STATIC : (filterMask == DebrisFilter ? layers::DEBRIS : layers::DYNAMIC));
  SpecifiedBroadPhaseLayerFilter sbflt(blr);
  BroadPhaseLayerFilter nobflt;
  BroadPhaseLayerFilter &flt = __popcount(filterMask) == 1 ? sbflt : nobflt;
  hasHit = jolt_api::phys_sys().GetNarrowPhaseQuery().CastRay(ray, hit, flt, PhysBody::ObjFilter(filterMask, filterMask));
  // debug("ray(from=%@, dir=%@, maxlen=%g)=%d f=%g", to_point3(pos), to_point3(dir), maxLength, hasHit, hit.mFraction);
}
Point3 PhysRayCast::getNormal() const
{
  if (hit.mBodyID.IsInvalid())
    return Point3(0, 1, 0);
  JPH::Vec3 normal;
  PhysBody::lock_body_ro(hit.mBodyID,
    [&](const JPH::Body &hit_body) { normal = hit_body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, pos + dir * hit.mFraction); });
  return to_point3(normal);
}

PhysWorld::PhysWorld(real default_static_friction, real default_restitution)
{
  Material &material = materials.emplace_back();
  material.friction = default_static_friction;
  material.restitution = default_restitution;
}

PhysWorld::~PhysWorld() { fetchSimRes(true); }

// interaction layers
void PhysWorld::setInteractionLayers(unsigned int mask1, unsigned int mask2, bool make_contacts) {}

struct JoltSimJob final : public cpujobs::IJob
{
  uint8_t maxSubSteps = 3;
  bool allowVariable = false;
  float fixedTimeStep = 1. / 60.f;
  uint32_t qPos = 0;
  float time = 0;

  void setup(int mss, float fts, float dt)
  {
    maxSubSteps = mss;
    fixedTimeStep = fabsf(fts);
    allowVariable = fts < 0;
    time = (maxSubSteps > 0) ? (time + dt) : dt;
  }

  void update(float dt, int nsteps = 1) { jolt_api::phys_sys().Update(dt, nsteps, tempAllocator, jobSystem); }

  const char *getJobName(bool &) const override { return "JoltSimJob"; }

  void doJob() override
  {
    if (maxSubSteps > 0)
    {
      int numSubSteps = int(time / fixedTimeStep);
      time -= numSubSteps * fixedTimeStep;
      numSubSteps = min(numSubSteps, (int)maxSubSteps);
      if (numSubSteps || !allowVariable)
        update(numSubSteps * fixedTimeStep, max(numSubSteps, 1)); // Still update even with 0 dt for broadphase update/contact removal
      else                                                        // Update with less then `fixedTimeStep` time
        update(eastl::exchange(time, 0.f), 1);
      DA_PROFILE_TAG(JoltSimJob, "ncollsteps: %d", numSubSteps);
    }
    else
      update(time);
  }
};
static JoltSimJob sim_job;

void PhysWorld::startSim(real dt, bool wake_up_thread)
{
  G_ASSERT(interlocked_acquire_load(sim_job.done) == 1);
  sim_job.setup(maxSubSteps, fixedTimeStep, dt);
  threadpool::AddFlags flags = threadpool::AddFlags::IgnoreNotDone;
  if (wake_up_thread)
    flags |= threadpool::AddFlags::WakeOnAdd;
  threadpool::add(&sim_job, threadpool::PRIO_DEFAULT, sim_job.qPos, flags);
}
bool PhysWorld::fetchSimRes(bool wait, PhysBody *)
{
  if (wait && interlocked_acquire_load(sim_job.done) == 0)
  {
    threadpool::barrier_active_wait_for_job(&sim_job, threadpool::PRIO_DEFAULT, sim_job.qPos);
    threadpool::wait(&sim_job);
  }
  return interlocked_acquire_load(sim_job.done) == 1;
}


int PhysWorld::createNewMaterialId()
{
  Material material = materials[0];
  materials.push_back(material);
  return materials.size() - 1;
}


int PhysWorld::createNewMaterialId(real static_friction, real /*dynamic_friction*/, real restitution, real /*softness*/)
{
  Material material;
  material.friction = static_friction;
  material.restitution = restitution;
  materials.push_back(material);
  return materials.size() - 1;
}

void PhysWorld::setMaterialPairInteraction(int mat_id1, int mat_id2, bool collide) {}

void *PhysWorld::loadSceneCollision(IGenLoad &crd, int scene_mat_id) { return nullptr; }
bool PhysWorld::unloadSceneCollision(void *handle) { return true; }

void PhysWorld::addBody(PhysBody *b, bool kinematic, bool /*update_aabb*/)
{
  if (kinematic)
  {
    body_api().SetObjectLayer(b->bodyId,
      make_objlayer(layers::STATIC, b->groupMask, b->layerMask = AllFilter & (~(KinematicFilter | StaticFilter))));
    body_api().SetMotionType(b->bodyId, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
  }
  body_api().AddBody(b->bodyId, JPH::EActivation::Activate);
}
void PhysWorld::removeBody(PhysBody *b) { body_api().RemoveBody(b->bodyId); }

const PhysWorld::Material *PhysWorld::getMaterial(int id) const { return (id < materials.size()) ? &materials[id] : nullptr; }

void PhysWorld::updateContactReports() {}

PhysJoint *PhysWorld::regJoint(PhysJoint *j)
{
  // nop
  return j;
}

static Point3 get_velocity_from_hit(const JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> &cb)
{
  if (cb.mHit.mBodyID2.IsInvalid())
    return Point3(0, 0, 0);
  return to_point3(jolt_api::body_api().GetLinearVelocity(cb.mHit.mBodyID2));
}

void PhysWorld::shape_query(const JPH::Shape *shape, const TMatrix &from, const Point3 &dir, PhysWorld *,
  dag::ConstSpan<PhysBody *> bodies, PhysShapeQueryResult &out, int filter_grp, int filter_mask)
{
  G_ASSERTF(shape->GetSubType() == JPH::EShapeSubType::ConvexHull || shape->GetCenterOfMass().LengthSq() < 1e-6f,
    "shape %d:%d comOfs=%@", (int)shape->GetType(), (int)shape->GetSubType(), to_point3(shape->GetCenterOfMass()));
  // Special case for ConvexHull in order to save on translating to zero COM
  JPH::ShapeCast shape_cast =
    (shape->GetSubType() != JPH::EShapeSubType::ConvexHull)
      ? JPH::ShapeCast(shape, JPH::Vec3::sReplicate(1.0f), to_jMat44(from), to_jVec3(dir))
      : JPH::ShapeCast::sFromWorldTransform(shape, JPH::Vec3::sReplicate(1.0f), to_jMat44(from), to_jVec3(dir));

  JPH::TransformedShape tshape;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> closestCb;

  for (auto b : bodies)
  {
    tshape = body_api().GetTransformedShape(b->bodyId);
    closestCb.SetContext(&tshape);
    if ((b->getGroupMask() & filter_mask) && (filter_grp & b->getInteractionLayer()))
      JPH::CollisionDispatch::sCastShapeVsShapeWorldSpace(shape_cast, defShapeCastStg, b->getShape(), JPH::Vec3::sReplicate(1.0f), {},
        body_api().GetCenterOfMassTransform(b->bodyId), {}, {}, closestCb);
  }

  // debug("shape_query(from=%@, dir=%@, out.t=%g)=%d f=%g", from, dir, out.t, closestCb.HadHit(), closestCb.mHit.mFraction);
  if (closestCb.HadHit() && closestCb.mHit.mFraction < out.t)
  {
    out.t = closestCb.mHit.mFraction;
    out.res = to_point3(closestCb.mHit.mContactPointOn1);
    out.norm = to_point3(-closestCb.mHit.mPenetrationAxis.Normalized());
    out.vel = get_velocity_from_hit(closestCb);
  }
}
bool PhysWorld::convex_shape_sweep_test(const JPH::Shape *shape, const Point3 &from, const Point3 &to,
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> &closestCb, PhysShapeQueryResult &out,
  const JPH::ObjectLayerFilter &obj_filter, const JPH::BodyFilter &body_filter)
{
  JPH::RShapeCast shape_cast(shape, JPH::Vec3::sReplicate(1.0f), JPH::Mat44::sTranslation(to_jVec3(from)), to_jVec3(to - from));
  closestCb.mHit.mFraction = out.t;
  phys_sys().GetNarrowPhaseQuery().CastShape(shape_cast, defShapeCastStg, JPH::Vec3::sZero(), closestCb, {}, obj_filter, body_filter);
  if (closestCb.HadHit() && closestCb.mHit.mFraction < out.t)
  {
    out.t = closestCb.mHit.mFraction;
    out.res = to_point3(closestCb.mHit.mContactPointOn1);
    out.norm = to_point3(-closestCb.mHit.mPenetrationAxis.Normalized());
    out.vel = get_velocity_from_hit(closestCb);
  }
  return out.t < 1.f;
}

void PhysWorld::shapeQuery(const PhysBody *body, const TMatrix &from, const TMatrix &to, dag::ConstSpan<PhysBody *> bodies,
  PhysShapeQueryResult &out, int filter_grp, int filter_mask)
{
  auto shape = body->getShape();
  if (!shape || shape->GetType() != JPH::EShapeType::Convex)
    return;
  shape_query(shape, from, to.getcol(3) - from.getcol(3), this, bodies, out, filter_grp, filter_mask);
}
void PhysWorld::shapeQuery(const PhysCollision &shape, const TMatrix &from, const TMatrix &to, dag::ConstSpan<PhysBody *> bodies,
  PhysShapeQueryResult &out, int filter_grp, int filter_mask)
{
  switch (shape.collType)
  {
    case PhysCollision::TYPE_SPHERE:
    {
      JPH::SphereShape convex(static_cast<const PhysSphereCollision &>(shape).getRadius());
      return shape_query(&convex, from, to.getcol(3) - from.getcol(3), this, bodies, out, filter_grp, filter_mask);
    }
    case PhysCollision::TYPE_BOX:
    {
      auto ext = static_cast<const PhysBoxCollision &>(shape).getSize();
      JPH::BoxShape convex(to_jVec3(ext / 2), jolt_api::min_convex_rad_for_box(ext));
      return shape_query(&convex, from, to.getcol(3) - from.getcol(3), this, bodies, out, filter_grp, filter_mask);
    }
    default: G_ASSERTF(0, "unsupported collType=%d in %s", shape.collType, __FUNCTION__);
  }
}

#include <phys/dag_physSysInst.h>
void PhysSystemInstance::phys_jolt_setup_collision_groups_for_joints(int grpId, Tab<PhysSystemInstance::Body> &b, PhysicsResource *r)
{
  JPH::Ref<JPH::GroupFilterTable> group_filter = new JPH::GroupFilterTable(b.size());
  for (const auto &jnt : r->rdBallJoints)
  {
    group_filter->DisableCollision(jnt.body2, jnt.body1);
    b[jnt.body1].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
    b[jnt.body2].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
  }
  for (const auto &jnt : r->rdHingeJoints)
  {
    group_filter->DisableCollision(jnt.body2, jnt.body1);
    b[jnt.body1].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
    b[jnt.body2].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
  }
  for (const auto &jnt : r->revoluteJoints)
  {
    group_filter->DisableCollision(jnt.body2, jnt.body1);
    b[jnt.body1].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
    b[jnt.body2].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
  }
  for (const auto &jnt : r->sphericalJoints)
  {
    group_filter->DisableCollision(jnt.body2, jnt.body1);
    b[jnt.body1].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
    b[jnt.body2].body->lockRW([&](JPH::Body &b) { b.GetCollisionGroup().SetGroupFilter(group_filter); });
  }
  for (int i = 0; i < b.size(); ++i)
    b[i].body->lockRW([&](JPH::Body &b) {
      if (b.GetCollisionGroup().GetGroupFilter() == group_filter)
      {
        b.GetCollisionGroup().SetGroupID(grpId);
        b.GetCollisionGroup().SetSubGroupID(i);
      }
    });
}

int phys_body_get_hmap_step(PhysBody *b)
{
  //==
  return -1;
}
int phys_body_set_hmap_step(PhysBody *b, int step)
{
  //==
  return -1;
}
