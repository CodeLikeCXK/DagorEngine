// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <ecs/camera/getActiveCameraSetup.h>
#include <ecs/core/entityManager.h>
#include <drv/3d/dag_viewScissor.h>
#include <drv/3d/dag_renderTarget.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <drv/3d/dag_driver.h>
#include <3d/dag_render.h>
#include <math/dag_mathBase.h>
#include <math/dag_mathUtils.h>
#include <startup/dag_globalSettings.h>

ECS_DEF_PULL_VAR(camera_view_es); // todo: this should be code-gened for ES, not only for types

static inline float deg_to_fov(float deg) { return 1.f / tanf(DEG_TO_RAD * 0.5f * deg); }

template <typename Callable>
static inline void process_active_camera_ecs_query(ecs::EntityManager &manager, Callable);

CameraSetup get_active_camera_setup(ecs::EntityManager &manager)
{
  CameraSetup camSetup;
  ecs::EntityId active_camera_eid = ecs::INVALID_ENTITY_ID;
  camSetup.transform.m[3][1] = 100; // use 100m height for missing camera to prevent 'underworld' artefacts

  int active_camera_found = 0;
  process_active_camera_ecs_query(manager,
    [&active_camera_eid, &active_camera_found, &camSetup](ECS_REQUIRE(eastl::true_type camera__active)
                                                            ECS_REQUIRE(ecs::Tag camera_view) const ecs::EntityManager &manager,
      ecs::EntityId eid, const TMatrix &transform, const DPoint3 *camera__accuratePos = nullptr, float fov = 90.0f,
      float fovSettings = 90.0f, float znear = 0.1f, float zfar = 5000.f, bool camera__fovHorPlus = true,
      bool camera__fovHybrid = false) {
      G_UNUSED(manager);
#if DAGOR_DBGLEVEL > 0
      if (active_camera_found)
        logerr("more than one active camera (eid[0]=%08X <%s> and eid[%d]=%08X <%s>)!", ecs::entity_id_t(active_camera_eid),
          manager.getEntityTemplateName(active_camera_eid), active_camera_found, ecs::entity_id_t(eid),
          manager.getEntityTemplateName(eid));
#endif
      G_ASSERTF(transform.getcol(3).lengthSq() < 1e10f, "Bad camera %d(%s) pos (%f,%f,%f)", (ecs::entity_id_t)eid,
        manager.getEntityTemplateName(eid), P3D(transform.getcol(3)));
      camSetup.transform = transform;
      if (camera__accuratePos)
        camSetup.accuratePos = *camera__accuratePos;
      else
        camSetup.accuratePos = transform.getcol(3);
      camSetup.fovSettings = fovSettings;
      camSetup.fov = fov;
      camSetup.znear = znear;
      camSetup.zfar = zfar;
      camSetup.fovMode = camera__fovHybrid ? EFM_HYBRID : camera__fovHorPlus ? EFM_HOR_PLUS : EFM_HOR_PLUS;
      camSetup.camEid = eid;
      if (!active_camera_found)
        active_camera_eid = eid;
      active_camera_found++;
    });

#if DAGOR_DBGLEVEL > 0
  if (!active_camera_found)
  {
    static unsigned last_warn_frame_no = 0;
    if (last_warn_frame_no == 0 || dagor_frame_no() - last_warn_frame_no > 1)
      logwarn("no active camera found!");
    last_warn_frame_no = dagor_frame_no();
  }
#endif

  // inverted, so we check for nans as well.
  if (!(fabsf(dot(camSetup.transform.getcol(0), camSetup.transform.getcol(1))) < 1e-6f &&
        fabsf(dot(camSetup.transform.getcol(0), camSetup.transform.getcol(2))) < 1e-6f &&
        fabsf(dot(camSetup.transform.getcol(1), camSetup.transform.getcol(2))) < 1e-6f))
  {
#if DAGOR_DBGLEVEL > 0
    static bool log_fired = false;
    if (!log_fired)
    {
      logerr("view matrix should be orthonormalized %@ %@ %@, eid %d <%s>", camSetup.transform.getcol(0), camSetup.transform.getcol(1),
        camSetup.transform.getcol(2), ecs::entity_id_t(active_camera_eid), manager.getEntityTemplateName(active_camera_eid));
      log_fired = true;
    }
#endif
    camSetup.transform.orthonormalize();
    if (check_nan(camSetup.transform))
      camSetup.transform = TMatrix::IDENT;
  }
  else if (!(fabsf(lengthSq(camSetup.transform.getcol(0)) - 1) < 1e-5f && fabsf(lengthSq(camSetup.transform.getcol(1)) - 1) < 1e-5f &&
             fabsf(lengthSq(camSetup.transform.getcol(2)) - 1) < 1e-5f))
  {
#if DAGOR_DBGLEVEL > 0
    static bool log_fired = false;
    if (!log_fired)
    {
      logerr("view matrix should be normalized %@ %@ %@, eid %d <%s>", camSetup.transform.getcol(0), camSetup.transform.getcol(1),
        camSetup.transform.getcol(2), ecs::entity_id_t(active_camera_eid), manager.getEntityTemplateName(active_camera_eid));
      log_fired = true;
    }
#endif
    camSetup.transform.setcol(0, normalize(camSetup.transform.getcol(0)));
    camSetup.transform.setcol(1, normalize(camSetup.transform.getcol(1)));
    camSetup.transform.setcol(2, normalize(camSetup.transform.getcol(2)));
  }
  else // all good
    return camSetup;
  // bad camera's tm (not (ortho)normalized basis)
  ECS_SET(camSetup.camEid, transform, camSetup.transform); // correct camera entity transform comp as well
  return camSetup;
}

CameraSetup get_active_camera_setup()
{
  return get_active_camera_setup(*g_entity_mgr); // still singleton
}

TMatrix4 calc_active_camera_globtm()
{
  CameraSetup cameraSetup = get_active_camera_setup();

  TMatrix viewTm;
  TMatrix4 projTm;
  Driver3dPerspective persp;
  int view_w, view_h;
  calc_camera_values(cameraSetup, viewTm, persp, view_w, view_h);
  d3d::calcproj(persp, projTm);
  return TMatrix4(viewTm) * projTm;
}

TMatrix calc_camera_view_tm(const TMatrix &view_itm)
{
  G_ASSERT(!check_nan(view_itm));

// we assume it is orthonormalized already
#if DAGOR_DBGLEVEL > 0
  if (fabsf(lengthSq(view_itm.getcol(0)) - 1) > 1e-5f || fabsf(lengthSq(view_itm.getcol(1)) - 1) > 1e-5f ||
      fabsf(lengthSq(view_itm.getcol(2)) - 1) > 1e-5f || fabsf(dot(view_itm.getcol(0), view_itm.getcol(1))) > 1e-6f ||
      fabsf(dot(view_itm.getcol(0), view_itm.getcol(2))) > 1e-6f || fabsf(dot(view_itm.getcol(1), view_itm.getcol(2))) > 1e-6f)
  {
    logerr("view matrix should be orthonormalized %@ %@ %@", view_itm.getcol(0), view_itm.getcol(1), view_itm.getcol(2));
  }
#endif

  return orthonormalized_inverse(view_itm);
}

HorVerFov calc_hor_ver_fov(const float fov_degree, const FovMode mode, const int view_w, const int view_h)
{
  float fovInTan = deg_to_fov(fov_degree);

  // we specify fov in horizontal axis, by 16:9 normalization
  // so we need to renormalize it to vert fov first and then
  // find our hor/vert fov
  // note that fov is inverse (as it's tangent) and thus we
  // inverse division too
  float verFov = fovInTan * CAMERA_FOV_REFERENCE_ASPECT_RATIO;
  float horFov = fovInTan;
  if (mode == EFM_HOR_PLUS)
    horFov = verFov * view_h / view_w;
  else if (mode == EFM_HYBRID)
  {
    // if it's less wide than 16:9 then we do ver+
    if (float(view_w) / float(view_h) < CAMERA_FOV_REFERENCE_ASPECT_RATIO)
      verFov = fovInTan * view_w / view_h;
    horFov = verFov * view_h / view_w;
  }

  HorVerFov res;
  res.horFov = horFov;
  res.verFov = verFov;

  return res;
}

Driver3dPerspective calc_camera_perspective(const CameraSetup &camera_setup, int view_w, int view_h)
{
  return calc_camera_perspective(camera_setup.fov, camera_setup.fovMode, camera_setup.znear, camera_setup.zfar, view_w, view_h);
}

Driver3dPerspective calc_camera_perspective(const float fov_degree, const FovMode fov_mode, const float z_near, const float z_far,
  int view_w, int view_h)
{
  const auto [horFov, verFov] = calc_hor_ver_fov(fov_degree, fov_mode, view_w, view_h);

  Driver3dPerspective persp;
  persp.zn = z_near;
  persp.zf = z_far;
  persp.ox = 0;
  persp.oy = 0;
  persp.wk = horFov;
  persp.hk = verFov;
  return persp;
}

TMatrix4D calc_camera_view(const TMatrix4 &view_itm, const DPoint3 &view_pos)
{
  TMatrix4D viewTm = orthonormalized_inverse(TMatrix4D(view_itm));
  // add one bit of precision
  DPoint3 row3 = -(viewTm.rotate43(view_pos));
  viewTm.setrow(3, row3.x, row3.y, row3.z, 1);

  return viewTm;
}

void calc_camera_values(const CameraSetup &camera_setup, TMatrix &view_tm, Driver3dPerspective &persp, int &view_w, int &view_h)
{
  d3d::get_screen_size(view_w, view_h);
  persp = calc_camera_perspective(camera_setup, view_w, view_h);
  view_tm = calc_camera_view_tm(camera_setup.transform);
}

void apply_camera_setup(const TMatrix &, const TMatrix &view_tm, const Driver3dPerspective &persp, int view_w, int view_h)
{
  d3d::settm(TM_VIEW, view_tm);
  d3d::setpersp(persp);
  d3d::setview(0, 0, view_w, view_h, 0, 1);
}

void apply_camera_setup(const CameraSetup &camera_setup, TMatrix &out_view_tm, TMatrix &out_view_itm)
{
  TMatrix viewTm;
  Driver3dPerspective persp;
  int view_w, view_h;
  calc_camera_values(camera_setup, viewTm, persp, view_w, view_h);

  apply_camera_setup(camera_setup.transform, viewTm, persp, view_w, view_h);
  out_view_tm = viewTm;
  out_view_itm = camera_setup.transform;
}
