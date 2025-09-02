//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <rendInst/rendInstGen.h>
#include <rendInst/rendInstExtra.h>
#include <daECS/core/entityManager.h>
#include <ecs/rendInst/riExtra.h>
#include <gamePhys/collision/rendinstCollision.h>
#include <util/dag_delayedAction.h>
#include <rendInst/rendInstAccess.h>


inline bool move_ri_extra_tm_ex(rendinst::riex_handle_t handle, const TMatrix &prev_transform, const TMatrix &transform,
  const Point3 &vel, const Point3 &omega, bool do_not_lock)
{
  mat44f m;
  v_mat44_make_from_43cu_unsafe(m, transform.m[0]);
  if (rendinst::moveRIGenExtra44(handle, m, /*moved*/ true, do_not_lock))
  {
    auto eid = find_ri_extra_eid(handle);
    if (eid != ecs::INVALID_ENTITY_ID)
      g_entity_mgr->sendEventImmediate(eid, EventOnRendinstMovement(handle, prev_transform, transform));

    dacoll::move_ri_instance(rendinst::RendInstDesc(handle), vel, omega);
    return true;
  }
  else // lock failed
    return false;
}

struct MoveRiExtraDA final : public DelayedAction
{
  rendinst::riex_handle_t handle;
  TMatrix prevTm;
  TMatrix targetTm;
  Point3 vel;
  Point3 omega;
  MoveRiExtraDA(rendinst::riex_handle_t h, const TMatrix &ot, const TMatrix &t, const Point3 &v, const Point3 &o) :
    handle(h), prevTm(ot), targetTm(t), vel(v), omega(o)
  {}
  bool precondition() override { return rendinst::isRiExtraLoaded(); }
  void performAction() override
  {
    if (rendinst::isRiGenExtraValid(handle)) // rendinst might been destroyed since addition of this DA
      move_ri_extra_tm_ex(handle, prevTm, targetTm, vel, omega, /*do_not_lock*/ false);
  }
};

inline void move_ri_extra_tm_with_motion(rendinst::riex_handle_t handle, const TMatrix &transform, const Point3 &vel,
  const Point3 &omega)
{
  TMatrix origTm = rendinst::getRIGenMatrix(rendinst::RendInstDesc(handle));
  if (vel == Point3::ZERO && omega == Point3::ZERO)
  {
    // to avoid making ri dynamic, we skip move entirely if there is no change (dynamic ri have no static shadows)
    if (are_approximately_equal(transform, origTm))
      return;
  }

  // If rendinsts are not loaded yet or lock failed
  if (!rendinst::isRiExtraLoaded() || !move_ri_extra_tm_ex(handle, origTm, transform, vel, omega, /*do_not_lock*/ true))
    add_delayed_action(new MoveRiExtraDA(handle, origTm, transform, vel, omega)); // ...try again in delayed action
}

inline void move_ri_extra_tm(rendinst::riex_handle_t handle, const TMatrix &transform)
{
  move_ri_extra_tm_with_motion(handle, transform, ZERO<Point3>(), ZERO<Point3>());
}
