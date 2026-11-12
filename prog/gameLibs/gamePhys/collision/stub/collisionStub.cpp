// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <gamePhys/collision/collisionLib.h>
#include <gamePhys/collision/collisionCache.h>
#include <gamePhys/collision/collisionInstances.h>
#include "../collisionLibPrivate.h"
#include <gameLibs/gamePhys/bundleAllHits_api.h>
#include <gameRes/dag_collisionResource.h>
#include <gameRes/collisionResourceBuilder.h>
#include <math/dag_TMatrix.h>

namespace dacoll
{

static Ptr<CollisionResource> static_collision_res;

bool load_static_collision(IGenLoad &crd)
{
  Ptr<CollisionResource> res = new CollisionResource(crd, -1, crd.getTargetName());
  if (!res || res->getAllNodes().empty())
    return false;
  static_collision_res = res;
  return true;
}

bool load_static_collision_frt(IGenLoad *crd)
{
  if (!crd)
    return false;
  CollisionResource *res = nullptr;
  if (!load_frt_block_as_collision(*crd, res))
    return false;
  static_collision_res = res;
  return true;
}

void set_static_collision_resource(CollisionResource *res)
{
  static_collision_res = res;
}

CollisionResource *get_static_collision_resource()
{
  return static_collision_res.get();
}

void destroy_static_collision()
{
  static_collision_res = nullptr;
}

bool traceray_normalized(const Point3 &p, const Point3 &dir, real &t, int *out_pmid, Point3 *out_norm, int flags,
  rendinst::RendInstDesc *out_desc, int ray_mat_id, const TraceMeshFaces *handle, rendinst::riex_handle_t skip_riex_handle)
{
  G_UNUSED(flags);
  G_UNUSED(out_desc);
  G_UNUSED(ray_mat_id);
  G_UNUSED(handle);
  G_UNUSED(skip_riex_handle);

  if (static_collision_res)
  {
    float inOutT = t;
    Point3 norm(0, 1, 0);
    int matId = -1;
    alignas(EA_CACHE_LINE_SIZE) mat44f tm;
    v_mat44_make_from_43cu_unsafe(tm, TMatrix::IDENT.array);
    if (static_collision_res->traceRay(tm, p, dir, inOutT, out_norm ? &norm : nullptr, matId, -1,
          CollisionNode::TRACEABLE | CollisionNode::PHYS_COLLIDABLE))
    {
      t = inOutT;
      if (out_norm)
        *out_norm = norm;
      if (out_pmid)
        *out_pmid = matId;
      return true;
    }
  }
  return false;
}

bool traceray_normalized_coll_type(const Point3 &p, const Point3 &dir, real &t, int *out_pmid, Point3 *out_norm, int flags,
  rendinst::RendInstDesc *out_desc, int *out_coll_type, int ray_mat_id, const TraceMeshFaces *handle,
  rendinst::riex_handle_t skip_riex_handle)
{
  bool hit = traceray_normalized(p, dir, t, out_pmid, out_norm, flags, out_desc, ray_mat_id, handle, skip_riex_handle);
  if (hit && out_coll_type)
    *out_coll_type = ETF_FRT;
  return hit;
}

bool tracedown_normalized(const Point3 &p, real &t, int *out_pmid, Point3 *out_norm, int flags, rendinst::RendInstDesc *out_desc,
  int ray_mat_id, const TraceMeshFaces *handle)
{
  return traceray_normalized(p, Point3(0, -1, 0), t, out_pmid, out_norm, flags, out_desc, ray_mat_id, handle);
}

bool traceray_normalized_frt(const Point3 &p, const Point3 &dir, real &t, int *out_pmid, Point3 *out_norm)
{
  return traceray_normalized(p, dir, t, out_pmid, out_norm);
}

bool traceray_normalized_lmesh(const Point3 &p, const Point3 &dir, real &t, int *out_pmid, Point3 *out_norm)
{
  return traceray_normalized(p, dir, t, out_pmid, out_norm);
}

bool traceray_normalized_ri(const Point3 &p, const Point3 &dir, real &t, int *out_pmid, Point3 *out_norm, rendinst::TraceFlags,
  rendinst::RendInstDesc *out_desc, int ray_mat_id, const TraceMeshFaces *handle, rendinst::riex_handle_t skip_riex_handle)
{
  return traceray_normalized(p, dir, t, out_pmid, out_norm, ETF_RI, out_desc, ray_mat_id, handle, skip_riex_handle);
}

void validate_trace_cache(const bbox3f &, const vec3f &, float, TraceMeshFaces *, float) {}
bool append_static_collision_tris(bbox3f_cref, vec4f *, int &) { return false; }
int resolve_static_cache_tri_mat(int) { return -1; }
bool trace_game_objects(const Point3 &, const Point3 &, float &, Point3 &, int, int) { return false; }
bool trace_game_objects_with_grid_filter(const Point3 &, const Point3 &, float &, Point3 &, int, int, const dag::ConstSpan<uint32_t>)
{
  return false;
}
bool traceht_water(const Point3 &, float &) { return false; }
float traceht_lmesh(const Point2 &) { return 0.f; }
float traceht_hmap(const Point2 &) { return 0.f; }
bool is_valid_heightmap_pos(const Point2 &) { return false; }
bool is_valid_water_height(float) { return false; }
float traceht_water_at_time(const Point3 &, float, float, bool &, float) { return false; }
bool traceray_water_at_time(const Point3 &, const Point3 &, float, float &) { return false; }
bool get_min_max_hmap_in_circle(const Point2 &, float, float &, float &) { return false; }
void get_min_max_hmap_list_in_circle(const Point2 &, float, Tab<Point2> &) {};
bool rayhit_normalized_lmesh(const Point3 &p, const Point3 &dir, real t)
{
  real inOutT = t;
  return traceray_normalized(p, dir, inOutT, nullptr, nullptr);
}
int trace_bundle_all_hits(dag::ConstSpan<BundleSegment>, int, int, dag::Span<BundleHit>, dag::Span<BundleRange>, uint32_t, bool)
{
  return 0;
}
bool bundle_segment_any_hit(const Point3 &, const Point3 &, int, int) { return false; }
bool rayhit_normalized(const Point3 &p, const Point3 &dir, real t, int flags, int ray_mat_id, const TraceMeshFaces *handle,
  rendinst::riex_handle_t skip_riex_handle)
{
  real inOutT = t;
  return traceray_normalized(p, dir, inOutT, nullptr, nullptr, flags, nullptr, ray_mat_id, handle, skip_riex_handle);
}
bool sphere_cast(const Point3 &, const Point3 &, float, ShapeQueryOutput &, int, const TraceMeshFaces *)
{
  G_ASSERT_RETURN(false, false);
}
bool sphere_cast_land(const Point3 &, const Point3 &, float, ShapeQueryOutput &, int) { G_ASSERT_RETURN(false, false); }
bool sphere_cast_ex(const Point3 &, const Point3 &, float, ShapeQueryOutput &, int, dag::ConstSpan<CollisionObject>,
  const TraceMeshFaces *, int, int)
{
  G_ASSERT_RETURN(false, false);
}
bool sphere_query_ri(const Point3 &, const Point3 &, float, ShapeQueryOutput &, int, Tab<rendinst::RendInstDesc> *,
  const TraceMeshFaces *, RIFilterCB *)
{
  G_ASSERT_RETURN(false, false);
}
bool trace_sphere_cast_ex(const Point3 &, const Point3 &, float, int, dacoll::ShapeQueryOutput &, int, int, const TraceMeshFaces *,
  int)
{
  G_ASSERT_RETURN(false, false);
}
CollisionObject add_dynamic_collision_from_coll_resource(const DataBlock *, const CollisionResource *, void *, int, int, int,
  const TMatrix *, const char *, const TMatrix *)
{
  G_ASSERT_RETURN(false, CollisionObject());
}
CollisionObject add_simple_dynamic_collision_from_coll_resource(const DataBlock &, const CollisionResource *, GeomNodeTree *, float,
  float, Point3 &, TMatrix &, TMatrix &, bool)
{
  G_ASSERT_RETURN(false, CollisionObject());
}
CollisionObject add_dynamic_collision_with_mask(const DataBlock &, void *, bool, bool, bool, int, const TMatrix *)
{
  G_ASSERT_RETURN(false, CollisionObject());
}
CollisionObject add_dynamic_sphere_collision(const TMatrix &, float, void *, bool) { G_ASSERT_RETURN(false, CollisionObject()); }
CollisionObject add_dynamic_capsule_collision(const TMatrix &, float, float, void *, bool)
{
  G_ASSERT_RETURN(false, CollisionObject());
}
CollisionObject add_dynamic_cylinder_collision(const TMatrix &, float, float, void *, bool)
{
  G_ASSERT_RETURN(false, CollisionObject());
}

void destroy_dynamic_collision(CollisionObject) { G_ASSERT(0); }
bool test_pair_collision(dag::ConstSpan<CollisionObject>, uint64_t, const TMatrix &, dag::ConstSpan<CollisionObject>, uint64_t,
  const TMatrix &, Tab<gamephys::CollisionContactData> &, TestPairFlags, bool)
{
  G_ASSERT_RETURN(false, false);
}
bool test_collision_world(dag::ConstSpan<CollisionObject>, const TMatrix &, float, Tab<gamephys::CollisionContactData> &,
  const TraceMeshFaces *)
{
  G_ASSERT_RETURN(false, false);
}
int update_ri_cache_in_volume_to_phys_world(const BBox3 &) { return 0; }
bool check_ri_collision_filtered(const rendinst::RendInstDesc &, const TMatrix &, const TMatrix &, int)
{
  G_ASSERT_RETURN(false, false);
}
bool test_collision_ri(const CollisionObject &, const BBox3 &, Tab<gamephys::CollisionContactData> &, const TraceMeshFaces *, int)
{
  G_ASSERT_RETURN(false, false);
}
bool test_collision_ri(const CollisionObject &, const BSphere3 &, Tab<gamephys::CollisionContactData> &, const TraceMeshFaces *, int)
{
  G_ASSERT_RETURN(false, false);
}
bool test_collision_frt(const CollisionObject &, Tab<gamephys::CollisionContactData> &, int) { G_ASSERT_RETURN(false, false); }
bool test_sphere_collision_world(const Point3 &, float, int, Tab<gamephys::CollisionContactData> &, dacoll::PhysLayer, int)
{
  G_ASSERT_RETURN(false, false);
}
bool test_box_collision_world(const TMatrix &, int, Tab<gamephys::CollisionContactData> &, dacoll::PhysLayer, int)
{
  G_ASSERT_RETURN(false, false);
}
bool test_capsule_collision_world(const TMatrix &, float, float, int, Tab<gamephys::CollisionContactData> &, dacoll::PhysLayer, int)
{
  G_ASSERT_RETURN(false, false);
}
void shape_query_frt(const PhysBody *, class TMatrix const &, class TMatrix const &, dacoll::ShapeQueryOutput &) { G_ASSERT(0); }
void shape_query_lmesh(const PhysBody *, class TMatrix const &, class TMatrix const &, dacoll::ShapeQueryOutput &) { G_ASSERT(0); }
void shape_query_ri(const PhysBody *, class TMatrix const &, class TMatrix const &, float, dacoll::ShapeQueryOutput &, int,
  Tab<struct rendinst::RendInstDesc> *, struct TraceMeshFaces const *, RIFilterCB *)
{
  G_ASSERT(0);
}
void fetch_sim_res(bool) { G_ASSERT(0); }
void set_vert_capsule_shape_size(const CollisionObject &, float, float) { G_ASSERT(0); }
void set_collision_sphere_rad(const CollisionObject &, float) { G_ASSERT(0); }
void set_collision_box_extents(const CollisionObject &, const Point3 &) { G_ASSERT(0); }
void set_collision_object_tm(const CollisionObject &, const TMatrix &) { G_ASSERT(0); }
void draw_collision_object(const CollisionObject &) { G_ASSERT(0); }

int set_hmap_step(int) { G_ASSERT_RETURN(false, -1); }
int get_hmap_step() { G_ASSERT_RETURN(false, -1); }
void set_obj_motion(CollisionObject, const TMatrix &, const Point3 &, const Point3 &) { G_ASSERT(0); }
PhysMat::MatID get_lmesh_mat_id_at_point(const Point2 &) { G_ASSERT_RETURN(false, PHYSMAT_INVALID); }
static CollisionObject tmpObj;
CollisionObject &get_reusable_sphere_collision() { G_ASSERT_RETURN(false, tmpObj); }
CollisionObject &get_reusable_capsule_collision() { G_ASSERT_RETURN(false, tmpObj); }
CollisionObject &get_reusable_box_collision() { G_ASSERT_RETURN(false, tmpObj); }

void move_ri_instance(const rendinst::RendInstDesc &, const Point3 &, const Point3 &) { G_ASSERT(0); }
void enable_disable_ri_instance(const rendinst::RendInstDesc &, bool) { G_ASSERT(0); }
void flush_ri_instances() { G_ASSERT(0); }
ska::flat_hash_map<rendinst::RendInstDesc, int, RendInstDescHash> disabled_ri_instances;
bool is_ri_instance_disabled_outofline(const rendinst::RendInstDesc &) { G_ASSERT_RETURN(false, false); }
int get_link_name_id(const char *) { G_ASSERT_RETURN(false, -1); }
const char *get_link_name(const int) { G_ASSERT_RETURN(false, NULL); }

float exchange_ttl_for_collision_instances(float) { G_ASSERT_RETURN(false, -1.f); }

} // namespace dacoll

#include <gamePhys/collision/collisionResponse.h>
namespace daphys
{
void resolve_penetration(DPoint3 &, Quat &, dag::ConstSpan<gamephys::CollisionContactData>, double, const DPoint3 &, double, bool, int,
  float, float)
{
  G_ASSERT(0);
};
void resolve_contacts(const DPoint3 &, const Quat &, DPoint3 &, DPoint3 &, dag::ConstSpan<gamephys::CollisionContactData>, double,
  const DPoint3 &, const ResolveContactParams &, int)
{
  G_ASSERT(0);
};
} // namespace daphys