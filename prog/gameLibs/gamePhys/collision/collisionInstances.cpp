// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <gamePhys/collision/collisionInstances.h>
#include <gamePhys/collision/physLayers.h>

#include <phys/dag_physDecl.h>
#include <gamePhys/collision/collisionLib.h>
#include <gamePhys/collision/rendinstCollision.h>
#include <phys/dag_physics.h>
#include <rendInst/rendInstCollision.h>
#include <rendInst/rendInstAccess.h>


namespace dacoll
{

static bool add_instances_to_world = false;
static float default_time_to_live = 0.5f;

static CollisionObject create_bullet_scaled_copy(const CollisionObject &obj, const Point3 &scale, CollisionObjectUserData *user_ptr);

ScaledBulletInstance::ScaledBulletInstance(CICollisionObject original_obj, vec3f scl, const rendinst::RendInstDesc &in_desc,
  float time_of_death) :
  timeOfDeath(time_of_death), desc(in_desc)
{
  v_stu_p3(&scale.x, scl);
  userData.matId = rendinst::getRIGenMaterialId(desc);
  obj = create_bullet_scaled_copy(original_obj, scale, &userData);
}

ScaledBulletInstance::~ScaledBulletInstance()
{
  if (obj)
    destroy_dynamic_collision(obj);
}

void ScaledBulletInstance::updateUserDataPtr()
{
  if (obj.body)
    obj.body->setUserData(&userData);
}

static CollisionObject create_bullet_scaled_copy(const CollisionObject &obj, const Point3 &scale, CollisionObjectUserData *user_ptr)
{
  PhysCollision *newShape = obj.body->getCollisionScaledCopy(scale);
  int physMatId = PhysMat::getPhysBodyMaterial(user_ptr && user_ptr->matId != PHYSMAT_INVALID ? user_ptr->matId : PHYSMAT_DEFAULT);
  CollisionObject co = create_coll_obj_from_shape(*newShape, user_ptr, /*kinematic*/ false, /*add_to_world*/ false,
    /* auto mask */ true, dacoll::EPL_KINEMATIC, dacoll::DEFAULT_DYN_COLLISION_MASK, nullptr, physMatId);
  PhysCollision::clearAllocatedData(*newShape);
  delete newShape;
  return co;
}

void set_add_instances_to_world(bool flag) { add_instances_to_world = flag; }

void set_ttl_for_collision_instances(float value) { default_time_to_live = value; }

extern void push_non_empty_ri_instance_list(CollisionInstances &ci);

ScaledBulletInstance *CollisionInstances::getScaledInstance(const rendinst::RendInstDesc &desc, vec3f scale, bool &out_created)
{
  float timeOfDeath = get_ri_instances_time() + default_time_to_live;
  for (auto &si : bulletInstances)
  {
    if (desc != si.desc)
      continue;

    si.timeOfDeath = timeOfDeath;
    out_created = false;
    return &si;
  }
  //
  // Nothing found, create a new one
  //
  auto prevData = bulletInstances.data();
  auto &ret = bulletInstances.emplace_back(originalObj, scale, desc, timeOfDeath);
  if (DAGOR_UNLIKELY(prevData && bulletInstances.data() != prevData)) // reallocated?
    for (auto it = bulletInstances.begin(); it != &ret; ++it)
      it->updateUserDataPtr();

  if (DAGOR_LIKELY(ret.obj && ret.obj.body->getShape())) // Phys body alloc failed or bad shape?
    ;
  else
  {
    bulletInstances.pop_back();
    return nullptr;
  }

#ifndef ENABLE_APEX
  if (bulletInstances.size() == 1)
    push_non_empty_ri_instance_list(*this);
#endif

  out_created = true;
  return &ret;
}

extern void remove_empty_ri_instance_list(CollisionInstances &ci);

void CollisionInstances::removeCollisionObject(const rendinst::RendInstDesc &desc)
{
  bool removed = false;
  for (auto it = bulletInstances.begin(); it != bulletInstances.end();)
  {
    if (it->desc.doesReflectedInstanceMatches(desc))
    {
      removed = true;
      it = bulletInstances.erase_unsorted(it);
      if (it != bulletInstances.end())
        it->updateUserDataPtr();
    }
    else
      ++it;
  }
#ifndef ENABLE_APEX
  if (removed && bulletInstances.empty())
    remove_empty_ri_instance_list(*this);
#endif
}

void CollisionInstances::enableDisableCollisionObject(const rendinst::RendInstDesc &desc, bool flag)
{
  const auto it = eastl::find(disabledInstances.begin(), disabledInstances.end(), desc);
  const bool isEnabled = it == disabledInstances.end();
  if (isEnabled == flag)
    return;

  if (!flag)
    disabledInstances.emplace_back(desc);
  else
    disabledInstances.erase_unsorted(it);
}

bool CollisionInstances::isCollisionObjectEnabled(const rendinst::RendInstDesc &desc) const
{
  const auto it = eastl::find(disabledInstances.begin(), disabledInstances.end(), desc);
  const bool isEnabled = it == disabledInstances.end();
  return isEnabled;
}

CollisionInstances::CollisionInstances(CollisionInstances &&rhs) :
  bulletInstances(eastl::move(rhs.bulletInstances)),
  disabledInstances(eastl::move(rhs.disabledInstances)),
  originalObj(rhs.originalObj)
{
  rhs.originalObj.clear_ptrs();
}

CollisionInstances::~CollisionInstances()
{
  if (originalObj)
    destroy_dynamic_collision(originalObj);
}

void CollisionInstances::clear()
{
  clearInstances();
  destroy_dynamic_collision(originalObj);
  originalObj.clear_ptrs();
}

void CollisionInstances::clearInstances() { bulletInstances.clear(); }

void CollisionInstances::unlink() { prevNotEmpty = nextNotEmpty = -1; }

CollisionObject CollisionInstances::updateTm(const rendinst::RendInstDesc &desc, const TMatrix &tm, TMatrix *out_ntm,
  bool update_scale, bool instant)
{
  bool created = false;
  vec3f ls = v_sqrt(v_perm_xzac(v_perm_xycd(v_length3_sq(v_ldu(&tm.getcol(0).x)), v_length3_sq(v_ldu(&tm.getcol(1).x))),
    v_length3_sq(v_ldu(&tm.getcol(2).x))));
  if (DAGOR_UNLIKELY(!v_test_all_bits_zeros(v_cmp_ge(v_zero(), ls)))) // Note: .w == .z
  {
    // Note: ri grid is not suppose to output destroyed (zero-basis) tms
#if DAGOR_DBGLEVEL <= 0
    static
#endif
      bool logged = false;
    G_ASSERT_LOG(eastl::exchange(logged, true), "Attempt to collide with invalid scale(%f,%f,%f) ri%s[%d]=%s @ %@", v_extract_x(ls),
      v_extract_y(ls), v_extract_z(ls), desc.isRiExtra() ? "Extra" : "", desc.pool, rendinst::getRIGenResName(desc), tm.getcol(3));
    return {};
  }
  ScaledBulletInstance *scaledInstance = getScaledInstance(desc, ls, created);
  if (!scaledInstance) // PhysBody alloc failed?
    return {};
  CollisionObject cobj = scaledInstance->obj;

  TMatrix ntm;
  TMatrix &normTm = out_ntm ? *out_ntm : ntm;
  vec3f ils = v_rcp(ls);
  v_mat_43cu_from_mat44(normTm.m[0], {v_mul(v_ldu(&tm.getcol(0).x), v_splat_x(ils)), v_mul(v_ldu(&tm.getcol(1).x), v_splat_y(ils)),
                                       v_mul(v_ldu(&tm.getcol(2).x), v_splat_z(ils)), v_ldu_p3(&tm.getcol(3).x)});

#ifndef USE_JOLT_PHYSICS
  get_phys_world()->fetchSimRes(true);
#endif

  // To consider: use scale difference instead of raw compare
  if (update_scale && (v_signmask(v_cmp_eq(ls, v_ldu(&scaledInstance->scale.x))) & 0b111) != 0b111)
  {
    v_stu(&scaledInstance->scale.x, ls);
    cobj.body->patchCollisionScaledCopy(scaledInstance->scale, originalObj.body);
  }

  if (!instant)
    cobj.body->setTm(normTm);
  else
    cobj.body->setTmInstant(normTm);

  if (created)
  {
    cobj.body->setGroupAndLayerMask(EPL_STATIC, EPL_ALL & ~(EPL_KINEMATIC | EPL_STATIC));
    if (add_instances_to_world)
      get_phys_world()->addBody(cobj.body, /*kinematic*/ false); // After initial tm setup
  }

  return cobj;
}

CollisionObject CollisionInstances::updateTm(const rendinst::RendInstDesc &desc, const Point3 &vel, const Point3 &omega)
{
  if (!rendinst::isRiGenDescValid(desc) || !isCollisionObjectEnabled(desc))
    return CollisionObject();
  TMatrix tm = rendinst::getRIGenMatrix(desc);
  if (CollisionObject cobj = updateTm(desc, tm))
  {
    cobj.body->setVelocity(vel);
    cobj.body->setAngularVelocity(omega);
    return cobj;
  }
  return {};
}

} // namespace dacoll
