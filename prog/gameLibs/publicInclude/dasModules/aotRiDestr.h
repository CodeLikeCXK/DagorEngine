//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <dasModules/dasModulesCommon.h>
#include <dasModules/aotDacoll.h>
#include <rendInst/rendInstGen.h>
#include <gamePhys/phys/rendinstDestr.h>
#include <gamePhys/collision/cachedCollisionObject.h>
#include <gamePhys/phys/destructableObject.h>

MAKE_TYPE_FACTORY(DestructableObject, gamephys::DestructableObject);

namespace bind_dascript
{

inline void destroyRiExtra(rendinst::riex_handle_t riex_handle, const TMatrix &transform, bool create_destr_effects,
  const Point3 &impulse, const Point3 &impulse_po)
{
  rendinstdestr::destroyRiExtra(riex_handle, transform, create_destr_effects, impulse, impulse_po);
}

inline void destroyRendinstSimple(const rendinst::RendInstDesc &desc, rendinst::RendInstDesc &out_desc)
{
  out_desc = rendinstdestr::destroyRendinst(desc, // desc
    false,                                        // add restorable
    ZERO<Point3>(),                               // position
    ZERO<Point3>(),                               // impulse
    0.f,                                          // at time
    nullptr,                                      // collision info
    false,                                        // create destr
    nullptr,                                      // apex damage info
    -1                                            // destroy neighbour recursive depth
  );
}


inline void destroyRendinstForce(const rendinst::RendInstDesc &desc, bool add_restorable, bool create_destr,
  rendinst::RendInstDesc &out_desc)
{
  out_desc = rendinstdestr::destroyRendinst(desc, // desc
    add_restorable,                               // add restorable
    ZERO<Point3>(),                               // position
    ZERO<Point3>(),                               // impulse
    0.f,                                          // at time
    nullptr,                                      // collision info
    create_destr,                                 // create destr
    nullptr,                                      // apex damage info
    -1,                                           // destroy neighbour recursive depth
    0.f,                                          // impulse mult for child
    nullptr,                                      // destr callback
    rendinst::DestrOptionFlag::ForceDestroy);
}

inline void destroyRendinst(const rendinst::RendInstDesc &desc, bool add_restorable, const Point3 &pos, const Point3 &impulse,
  float at_time, bool create_destr, float impulse_mult_for_child, bool force_destroy, rendinst::RendInstDesc &out_desc)
{
  rendinst::DestrOptionFlags flags = rendinst::DestrOptionFlag::AddDestroyedRi;
  if (force_destroy)
    flags |= rendinst::DestrOptionFlag::ForceDestroy;
  out_desc = rendinstdestr::destroyRendinst(desc, add_restorable, pos, impulse, at_time,
    nullptr, // collision info
    create_destr,
    nullptr, // apex damage info
    -1, impulse_mult_for_child,
    nullptr, // destr callback
    flags);
}

inline void apply_impulse_to_riextra(const rendinst::riex_handle_t handle, const float impulse, const Point3 &dir, const Point3 &pos)
{
  CachedCollisionObjectInfo *obj = rendinstdestr::get_or_add_cached_collision_object(rendinst::RendInstDesc(handle), 0.f);
  if (!obj)
    return;
  obj->onImpulse(impulse, dir, pos, 0.f, -dir, /*flags*/ 0, /*user_data*/ -1);
}

inline void get_destructable_objects(
  const das::TBlock<void, const das::TTemporary<const das::TArray<gamephys::DestructableObject *>>> &block, das::Context *context,
  das::LineInfoArg *at)
{
  dag::ConstSpan<gamephys::DestructableObject *> destructables = destructables::getDestructableObjects();
  if (destructables.empty())
    return;

  das::Array arr;
  arr.data = (char *)destructables.data();
  arr.size = destructables.size();
  arr.capacity = destructables.size();
  arr.lock = 1;
  arr.flags = 0;
  vec4f arg = das::cast<das::Array *>::from(&arr);
  context->invoke(block, &arg, nullptr, at);
}

inline PhysSystemInstance *destructable_object_get_phys_sys_instance(const gamephys::DestructableObject &object)
{
  return object.physObj->getPhysSys();
}

} // namespace bind_dascript
