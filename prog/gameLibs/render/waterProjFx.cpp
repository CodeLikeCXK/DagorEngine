// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/waterProjFx.h>
#include <render/viewVecs.h>
#include <ioSys/dag_dataBlock.h>
#include <drv/3d/dag_renderTarget.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <drv/3d/dag_lock.h>
#include <drv/3d/dag_info.h>
#include <3d/dag_textureIDHolder.h>

#include <math/dag_TMatrix.h>
#include <math/dag_TMatrix4more.h>
#include <math/dag_frustum.h>
#include <math/dag_mathUtils.h>
#include <3d/dag_render.h>

#include <shaders/dag_shaders.h>
#include <shaders/dag_shaderBlock.h>
#include <generic/dag_carray.h>

#include <perfMon/dag_statDrv.h>

#define MIN_WAVE_HEIGHT               3.0f
#define CAMERA_PLANE_ELEVATION        3.0f
#define CAMERA_PLANE_BOTTOM_MIN_ANGLE 0.999f
#define FRUSTUM_CROP_MIN_HEIGHT       0.6f

WaterProjectedFx::WaterProjectedFx(int frame_width, int frame_height, dag::Span<TargetDesc> target_descs,
  const char *taa_reprojection_blend_shader_name, bool own_textures) :
  newProjTM(TMatrix4::IDENT),
  newProjTMJittered(TMatrix4::IDENT),
  newViewTM(TMatrix::IDENT),
  newViewItm(TMatrix::IDENT),
  waterLevel(0),
  numIntersections(0),
  savedCamPos(0, 0, 0),
  prevGlobTm(TMatrix4::IDENT),
  frameWidth(frame_width),
  frameHeight(frame_height),
  nTargets(target_descs.size()),
  taaEnabled(taa_reprojection_blend_shader_name),
  ownTextures(own_textures),
  globalFrameId(ShaderGlobal::getBlockId("global_frame")),
  targetsCleared(false),
  internalTargetsTexVarIds()
{
  G_ASSERT_RETURN(nTargets <= MAX_TARGETS, );

  if (taaEnabled)
    taaRenderer.init(taa_reprojection_blend_shader_name);

  d3d::SamplerInfo smpInfo;
  smpInfo.filter_mode = d3d::FilterMode::Linear;
  smpInfo.address_mode_u = smpInfo.address_mode_v = smpInfo.address_mode_w = d3d::AddressMode::Clamp;
  d3d::SamplerHandle smp = d3d::request_sampler(smpInfo);

  for (int i = 0; i < nTargets; ++i)
  {
    targetClearColors[i] = target_descs[i].clearColor;
    internalTargetsTexVarIds[i] = ::get_shader_variable_id(target_descs[i].texName, true);
    if (own_textures)
    {
      uint32_t texCreationFlags = getTargetAdditionalFlags() | (target_descs[i].texFmt & TEXFMT_MASK);
      ShaderGlobal::set_sampler(::get_shader_variable_id(String(0, "%s_samplerstate", target_descs[i].texName).c_str(), true), smp);
      tempRTPools[i] = ResizableRTargetPool::get(frameWidth, frameHeight, texCreationFlags, 1);

      internalTargets[i] = nullptr;

      String emptyTexName(target_descs[i].texName);
      emptyTexName.append("_empty");
      emptyInternalTextures[i] = dag::create_tex(nullptr, 4, 4, texCreationFlags, 1, emptyTexName.c_str());
    }
  }
  if (own_textures)
  {
    // Since clear colors can be anything, we can't get by with just TEXCF_CLEAR_ON_CREATE.
    d3d::GpuAutoLock gpuLock;
    clear();
  }
}

void WaterProjectedFx::setResolution(int frame_width, int frame_height)
{
  frameWidth = frame_width;
  frameHeight = frame_height;
  for (int i = 0; i < nTargets; ++i)
    if (internalTargets[i])
      internalTargets[i]->resize(frameWidth, frameHeight);
}

uint32_t WaterProjectedFx::getTargetAdditionalFlags() const
{
  uint32_t texFlags = TEXCF_SRGBREAD | TEXCF_SRGBWRITE | TEXCF_RTARGET;
  return texFlags;
}

void WaterProjectedFx::setTextures()
{
  G_ASSERT(ownTextures);
  for (int i = 0; i < nTargets; ++i)
  {
    if (internalTargets[i])
    {
      ShaderGlobal::set_texture(internalTargetsTexVarIds[i], internalTargets[i]->getTexId());
    }
    else
    {
      ShaderGlobal::set_texture(internalTargetsTexVarIds[i], emptyInternalTextures[i].getTexId());
    }
  }
}

void WaterProjectedFx::setView() { setView(newViewTM, newProjTM); }

bool WaterProjectedFx::getView(TMatrix4 &view_tm, TMatrix4 &proj_tm, Point3 &camera_pos)
{
  if (!isValidView())
    return false;

  view_tm = newViewTM;
  proj_tm = newProjTM;
  camera_pos = newViewItm.getcol(3);
  return true;
}

bool WaterProjectedFx::isValidView() const { return numIntersections > 0; }

void WaterProjectedFx::prepare(const TMatrix &view_tm, const TMatrix &view_itm, const TMatrix4 &proj_tm, const TMatrix4 &glob_tm,
  float water_level, float significant_wave_height, int frame_no, bool change_projection)
{
  waterLevel = water_level;
  numIntersections = 0;

  newViewTM = view_tm;
  newProjTM = proj_tm;
  newViewItm = view_itm;
  savedCamPos = view_itm.getcol(3);

  if (change_projection)
  {
    float wavesDeltaH = max(significant_wave_height * 2.2f, MIN_WAVE_HEIGHT);
    float waterHeightTop = water_level + wavesDeltaH;
    float waterHeightBottom = water_level - wavesDeltaH;

    Point3 cameraDir = newViewItm.getcol(2);
    Point3 cameraPos = newViewItm.getcol(3);
    Point4 bottomPlane;
    v_stu(&bottomPlane.x, Frustum(glob_tm).camPlanes[Frustum::BOTTOM]);
    float cosA = min(bottomPlane.y, CAMERA_PLANE_BOTTOM_MIN_ANGLE);
    cameraPos += -normalize(Point3(cameraDir.x, 0.0f, cameraDir.z)) *
                 max(waterHeightTop + CAMERA_PLANE_ELEVATION - abs(cameraPos.y), 0.0f) * safediv(cosA, safe_sqrt(1.0f - sqr(cosA)));
    cameraPos.y = max(abs(cameraPos.y), waterHeightTop + CAMERA_PLANE_ELEVATION) * (cameraPos.y > 0 ? 1.0f : -1.0f);
    newViewItm.setcol(3, cameraPos);

    newViewTM = orthonormalized_inverse(newViewItm);
    TMatrix4 currentGlobTm = TMatrix4(newViewTM) * newProjTM;

    // We have current camera frustum - find intersection with water bounds (upper & lower water planes)
    int i, j;
    vec4f points[8];
    Point3 frustumPoints[8];
    Frustum frustum = Frustum(currentGlobTm);
    // Points order (it is important) - for edges_vid
    // -1 1 1
    // -1 1 0
    // -1 -1 1
    // -1 -1 0
    // 1 1 1
    // 1 1 0
    // 1 -1 1
    // 1 -1 0
    frustum.generateAllPointFrustm(points);

    Point4 p;
    for (i = 0; i < 8; i++)
    {
      v_stu(&p.x, points[i]);
      frustumPoints[i] = Point3::xyz(p);
    }
    int edges_vid[] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 4, 4, 6, 6, 2, 2, 0, 1, 5, 5, 7, 7, 3, 3, 1};

    // Total 12 frustum edges in frustum * 2 planes
    carray<Point3, 12 + 12> intersectionPoints;

    for (j = 0; j < 2; j++)
    {
      float waterPlaneHeight = j == 0 ? waterHeightTop : waterHeightBottom;
      for (i = 0; i < 24; i += 2)
      {
        // Frustum edge
        Point3 &p1 = frustumPoints[edges_vid[i]];
        Point3 &p2 = frustumPoints[edges_vid[i + 1]];

        float planeDist1 = p1.y - waterPlaneHeight;
        float planeDist2 = p2.y - waterPlaneHeight;

        if (planeDist1 * planeDist2 < 0.f) // Points are opposite side of the plane? - then edge intersects the plane
        {
          float k = safediv(planeDist1, planeDist1 - planeDist2); // Should be positive
          Point3 intersectionPoint = p1 + (p2 - p1) * k;          // Frustum-edge intersection point with water plane
          intersectionPoint.y = water_level;                      // Project point exactly on plane which we use for rendering
          intersectionPoints[numIntersections++] = intersectionPoint;
        }
      }
    }

    if (numIntersections > 0)
    {
      // Project all points on screen & calculate x&y bounds
      Point2 boxMin = Point2(10000.f, 10000.f);
      Point2 boxMax = Point2(-10000.f, -10000.f);
      for (i = 0; i < numIntersections; i++)
      {
        currentGlobTm.transform(intersectionPoints[i], p);
        boxMin.x = min(boxMin.x, safediv(p.x, p.w));
        boxMin.y = min(boxMin.y, safediv(p.y, p.w));

        boxMax.x = max(boxMax.x, safediv(p.x, p.w));
        boxMax.y = max(boxMax.y, safediv(p.y, p.w));
      }

      // When camera is low the resulting frustum crop can sometimes degenerate into almost a line
      // which leads to some of the projected fx missing from the final image.
      // Having a minimum height for the frustum crop box helps to avoid this most of the time.
      float boxYAvg = (boxMin.y + boxMax.y) * 0.5f;
      float boxYSize = boxMax.y - boxMin.y;
      if (boxYSize < FRUSTUM_CROP_MIN_HEIGHT)
      {
        boxMin.y = boxYAvg - FRUSTUM_CROP_MIN_HEIGHT / 2.f;
        boxMax.y = boxYAvg + FRUSTUM_CROP_MIN_HEIGHT / 2.f;
      }

      boxMin.x = clamp(boxMin.x, -2.0f, 2.0f);
      boxMin.y = clamp(boxMin.y, -2.0f, 2.0f);
      boxMax.x = clamp(boxMax.x, -2.0f, 2.0f) + 0.001f;
      boxMax.y = clamp(boxMax.y, -2.0f, 2.0f) + 0.001f;
      TMatrix4 cropMatrix = matrix_perspective_crop(boxMin.x, boxMax.x, boxMin.y, boxMax.y, 0.0f, 1.0f);
      newProjTM = newProjTM * cropMatrix;
    }
  }
  else
  {
    numIntersections = 1;
  }

  if (isValidView() && taaEnabled)
  {
    const float watJitterx = 0.2f / ((float)frameWidth);
    const float watJittery = 0.2f / ((float)frameHeight);

    float xShift = (float)(frame_no % 2);
    float yShift = (float)((frame_no % 4) / 2);
    curJitter = Point2((-3.0f + 4.0f * xShift + 2.0f * yShift) * watJitterx, (-1.0f - 2.0f * xShift + 4.0f * yShift) * watJittery);
    TMatrix4 jitterMatrix;
    jitterMatrix.setcol(0, 1.f, 0.f, 0.f, 0.f);
    jitterMatrix.setcol(1, 0.f, 1.f, 0.f, 0.f);
    jitterMatrix.setcol(2, 0.f, 0.f, 1.f, 0.f);
    jitterMatrix.setcol(3, curJitter.x, curJitter.y, 0.f, 1.f);
    newProjTMJittered = newProjTM * jitterMatrix.transpose();
  }

  TMatrix4 currentGlobTm = TMatrix4(newViewTM) * newProjTM;
  process_tm_for_drv_consts(currentGlobTm);
  setWaterMatrix(currentGlobTm);
}

void WaterProjectedFx::clear(bool forceClear)
{
  G_ASSERT(ownTextures);
  if (!forceClear && targetsCleared)
    return;

  SCOPE_RENDER_TARGET;
  for (int i = 0; i < nTargets; ++i)
  {
    if (internalTargets[i])
    {
      d3d::set_render_target(internalTargets[i]->getTex2D(), 0);
      d3d::clearview(CLEAR_TARGET, targetClearColors[i], 0.f, 0);
      d3d::resource_barrier({internalTargets[i]->getTex2D(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
    }
    if (emptyInternalTextures[i])
    {
      d3d::set_render_target(emptyInternalTextures[i].getTex2D(), 0);
      d3d::clearview(CLEAR_TARGET, targetClearColors[i], 0, 0);
      d3d::resource_barrier({emptyInternalTextures[i].getTex2D(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
    }
  }
  targetsCleared = true;
}

bool WaterProjectedFx::render(IWwaterProjFxRenderHelper *render_helper)
{
  G_ASSERT(ownTextures);
  G_ASSERT(render_helper);

  bool renderedAnything = false;

  renderedAnything = renderImpl(render_helper);

  for (int i = 0; i < nTargets; ++i)
  {
    if (internalTargets[i])
      d3d::resource_barrier({internalTargets[i]->getTex2D(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
  }

  return renderedAnything;
}

bool WaterProjectedFx::renderImpl(IWwaterProjFxRenderHelper *render_helper)
{
  G_ASSERT(render_helper && nTargets <= MAX_TARGETS);

  if (!isValidView())
  {
    setWaterMatrix(TMatrix4::IDENT);
    return false;
  }

  // We don't need TAA if there is nothing to antialiase.
  // There will be a delay in one frame, but it's Ok, cause it's just one frame without AA.
  bool taaEnabledForThisFrame = taaEnabled && !targetsCleared;
  // With TBR it's more effective to clear the texture. But it makes sense only if we have just one target
  // (so clearing and subsequent drawing will be in one render pass).
  bool isTileBasedRendering = d3d::get_driver_code().is(d3d::vulkan) || d3d::get_driver_code().is(d3d::metal);
  bool needClearTargets = !targetsCleared || (isTileBasedRendering && nTargets == 1);

  RTarget::Ptr taaTemp[MAX_TARGETS];
  RTarget::Ptr taaHistory[MAX_TARGETS];
  if (taaEnabledForThisFrame)
  {
    for (int i = 0; i < nTargets; ++i)
    {
      if ((taaTemp[i] = tempRTPools[i]->acquire()) == nullptr)
        return false; // device is in broken state, nothing to be done here

      G_ASSERT(internalTargets[i]);
      taaHistory[i] = internalTargets[i];
      internalTargets[i] = nullptr;
    }
  }
  for (int i = 0; i < nTargets; ++i)
  {
    if (internalTargets[i] == nullptr)
    {
      if ((internalTargets[i] = tempRTPools[i]->acquire()) != nullptr)
      {
        internalTargets[i]->resize(frameWidth, frameHeight);
      }
      else
        return false; // device is in broken state
      needClearTargets = true;
    }
  }

  SCOPE_VIEW_PROJ_MATRIX;
  setView(newViewTM, taaEnabledForThisFrame ? newProjTMJittered : newProjTM);

  SCOPE_RENDER_TARGET;
  if (needClearTargets)
  {
    TIME_D3D_PROFILE(clear);
    // Since d3d::clearview() clears all targets with the same color,
    // we have to clear each target separately.
    for (int i = 0; i < nTargets; ++i)
    {
      d3d::set_render_target(taaEnabledForThisFrame ? taaTemp[i]->getTex2D() : internalTargets[i]->getTex2D(), 0);
      d3d::clearview(CLEAR_TARGET, targetClearColors[i], 0.f, 0);
    }
  }
  for (int i = 0; i < nTargets; ++i)
    d3d::set_render_target(i, taaEnabledForThisFrame ? taaTemp[i]->getTex2D() : internalTargets[i]->getTex2D(), 0);

  static int renderWaterProjectibleDecalsVarId = get_shader_variable_id("render_water_projectible_decals", true);
  ShaderGlobal::set_int(renderWaterProjectibleDecalsVarId, 1);

  bool renderedAnything = false;
  renderedAnything |= render_helper->render_geometry(taaEnabledForThisFrame ? -1.1f : 0.f); // a = 0.3, so that's about -1.1 mipbias.
  if (!taaEnabledForThisFrame)
    renderedAnything |= render_helper->render_geometry_without_aa();

  ShaderGlobal::set_int(renderWaterProjectibleDecalsVarId, 0);

  if (taaEnabledForThisFrame)
  {
    if (renderedAnything)
    {
      static const int prev_globtm0VarId = ::get_shader_variable_id("prev_globtm_0");
      static const int prev_globtm1VarId = ::get_shader_variable_id("prev_globtm_1");
      static const int prev_globtm2VarId = ::get_shader_variable_id("prev_globtm_2");
      static const int prev_globtm3VarId = ::get_shader_variable_id("prev_globtm_3");
      static const int world_view_posVarId = ::get_shader_variable_id("world_view_pos");
      static const int cur_jitterVarId = ::get_shader_variable_id("cur_jitter");

      for (int i = 0; i < nTargets; ++i)
        d3d::set_render_target(i, internalTargets[i]->getTex2D(), 0);

      ShaderGlobal::setBlock(-1, ShaderGlobal::LAYER_FRAME);

      ShaderGlobal::set_color4(prev_globtm0VarId, Color4(prevGlobTm[0]));
      ShaderGlobal::set_color4(prev_globtm1VarId, Color4(prevGlobTm[1]));
      ShaderGlobal::set_color4(prev_globtm2VarId, Color4(prevGlobTm[2]));
      ShaderGlobal::set_color4(prev_globtm3VarId, Color4(prevGlobTm[3]));

      prevGlobTm = TMatrix4(newViewTM) * newProjTM;
      process_tm_for_drv_consts(prevGlobTm);

      set_viewvecs_to_shader(newViewTM, newProjTM);
      Color4 prevWorldViewPos = ShaderGlobal::get_color4(world_view_posVarId);
      ShaderGlobal::set_color4(world_view_posVarId, Color4::xyz1(newViewItm.getcol(3)));
      ShaderGlobal::set_color4(cur_jitterVarId, curJitter.x, curJitter.y, 0, 0);

      TEXTUREID prevFrameTargets[MAX_TARGETS];
      TEXTUREID curFrameTargets[MAX_TARGETS];
      for (int i = 0; i < nTargets; ++i)
      {
        prevFrameTargets[i] = taaHistory[i]->getTexId();
        curFrameTargets[i] = taaTemp[i]->getTexId();
      }
      render_helper->prepare_taa_reprojection_blend(prevFrameTargets, curFrameTargets);
      {
        TIME_D3D_PROFILE(taa);
        taaRenderer.render();
      }

      ShaderGlobal::set_color4(world_view_posVarId, prevWorldViewPos);
    }
    else if (needClearTargets)
    {
      TIME_D3D_PROFILE(clear);
      for (int i = 0; i < nTargets; ++i)
      {
        d3d::set_render_target(internalTargets[i]->getTex2D(), 0);
        d3d::clearview(CLEAR_TARGET, targetClearColors[i], 0.f, 0);
      }
    }

    ShaderGlobal::setBlock(globalFrameId, ShaderGlobal::LAYER_FRAME);

    for (int i = 0; i < nTargets; ++i)
      d3d::set_render_target(i, internalTargets[i]->getTex2D(), 0);

    setView(newViewTM, newProjTM);
    renderedAnything |= render_helper->render_geometry_without_aa();

    ShaderGlobal::setBlock(globalFrameId, ShaderGlobal::LAYER_FRAME);
  }

  if (taaEnabled)
  {
    for (int i = 0; i < nTargets; ++i)
      taaHistory[i] = nullptr;
  }

  targetsCleared = !renderedAnything && ownTextures; // Preserve 'false' state if the textures aren't owned.

  if (targetsCleared)
  {
    for (int i = 0; i < nTargets; ++i)
      internalTargets[i] = nullptr;
  }

  return renderedAnything;
}

void WaterProjectedFx::setView(const TMatrix &view_tm, const TMatrix4 &proj_tm)
{
  d3d::settm(TM_VIEW, view_tm);
  d3d::settm(TM_PROJ, &proj_tm);
}

void WaterProjectedFx::setWaterMatrix(const TMatrix4 &glob_tm)
{
  // set matrix to water shader
  static int waterEeffectsProjTmLine0VarId = get_shader_variable_id("water_effects_proj_tm_line_0", true);
  static int waterEeffectsProjTmLine1VarId = get_shader_variable_id("water_effects_proj_tm_line_1", true);
  static int waterEeffectsProjTmLine2VarId = get_shader_variable_id("water_effects_proj_tm_line_2", true);
  static int waterEeffectsProjTmLine3VarId = get_shader_variable_id("water_effects_proj_tm_line_3", true);

  ShaderGlobal::set_color4(waterEeffectsProjTmLine0VarId, Color4(glob_tm[0]));
  ShaderGlobal::set_color4(waterEeffectsProjTmLine1VarId, Color4(glob_tm[1]));
  ShaderGlobal::set_color4(waterEeffectsProjTmLine2VarId, Color4(glob_tm[2]));
  ShaderGlobal::set_color4(waterEeffectsProjTmLine3VarId, Color4(glob_tm[3]));
}
