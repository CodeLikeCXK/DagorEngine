// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "pixelPerfectSelection.h"
#include <3d/dag_lockTexture.h>
#include <drv/3d/dag_viewScissor.h>
#include <drv/3d/dag_renderTarget.h>
#include <drv/3d/dag_vertexIndexBuffer.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <drv/3d/dag_shaderConstants.h>
#include <drv/3d/dag_buffers.h>
#include <drv/3d/dag_texture.h>
#include <drv/3d/dag_lock.h>
#include <EditorCore/ec_interface.h>
#include <de3_dynRenderService.h>
#include <image/dag_texPixel.h>
#include <rendInst/rendInstExtraRender.h>
#include <rendInst/rendInstGenRender.h>
#include <rendInst/riShaderConstBuffers.h>
#include <shaders/dag_rendInstRes.h>
#include <shaders/dag_shaderBlock.h>
#include <shaders/dag_shaderResUnitedData.h>

int PixelPerfectSelection::global_frame_block_id = -1;
int PixelPerfectSelection::rendinst_render_pass_var_id = -1;
int PixelPerfectSelection::rendinst_scene_block_id = -1;

void PixelPerfectSelection::init()
{
  global_frame_block_id = ShaderGlobal::getBlockId("global_frame");
  rendinst_render_pass_var_id = ::get_shader_glob_var_id("rendinst_render_pass");
  rendinst_scene_block_id = ShaderGlobal::getBlockId("rendinst_scene");

  rendinstMatrixBuffer.reset(
    d3d::create_sbuffer(sizeof(Point4), 4U, SBCF_BIND_SHADER_RES, TEXFMT_A32B32G32R32F, "simple_selection_matrix_buffer"));

  depthRt.set(d3d::create_tex(nullptr, 1, 1, TEXCF_RTARGET | TEXFMT_DEPTH32, 1, "simple_selection_depth_rt"),
    "simple_selection_depth_rt");
}

TMatrix4 PixelPerfectSelection::makeProjectionMatrixForViewRegion(int viewWidth, int viewHeight, float fov, float zNear, float zFar,
  int regionLeft, int regionTop, int regionWidth, int regionHeight)
{
  // Normal frustum.
  const float verticalFov = 2.f * atanf(tanf(0.5f * fov) * (static_cast<float>(viewHeight) / viewWidth));
  const float aspect = static_cast<float>(viewWidth) / viewHeight;
  const float top = tanf(verticalFov * 0.5f) * zNear;
  const float bottom = -top;
  const float left = aspect * bottom;
  const float right = aspect * top;
  const float width = fabs(right - left);
  const float height = fabs(top - bottom);

  // Frustum that covers the required region.
  const float subLeft = left + ((regionLeft * width) / viewWidth);
  const float subTop = top - ((regionTop * height) / viewHeight);
  const float subWidth = (width * regionWidth) / viewWidth;
  const float subHeight = (height * regionHeight) / viewHeight;
  return matrix_perspective_off_center_reverse(subLeft, subLeft + subWidth, subTop - subHeight, subTop, zNear, zFar);
}

bool PixelPerfectSelection::getHitFromDepthBuffer(float &hitZ)
{
  bool hasBeenHit = false;

  Texture *depthTexture = depthRt.getTex2D();
  if (depthTexture)
  {
    LockedImage2DView<const float> lockedTexture = lock_texture<const float>(depthTexture, 0, TEXLOCK_READ);
    if (lockedTexture)
    {
      hitZ = lockedTexture.at(0, 0);
      hasBeenHit = hitZ != 0.0f;
    }
  }

  return hasBeenHit;
}

void PixelPerfectSelection::renderLodResource(const RenderableInstanceLodsResource &riLodResource, const TMatrix &transform)
{
  const bool hasImpostor = false;
  const uint32_t impostorBufferOffset = 0;

  SCENE_LAYER_GUARD(rendinst_scene_block_id);

  rendinst::render::startRenderInstancing();

  ShaderGlobal::set_int(rendinst_render_pass_var_id, eastl::to_underlying(rendinst::RenderPass::Depth));
  d3d::setind(unitedvdata::riUnitedVdata.getIB());
  d3d::set_buffer(STAGE_VS, rendinst::render::instancingTexRegNo, rendinstMatrixBuffer.get());

  rendinst::render::RiShaderConstBuffers cb;
  cb.setBBoxZero();
  cb.setOpacity(0, 1);
  cb.setBoundingSphere(0, 0, 1, 1, 0);
  cb.setInstancing(hasImpostor ? 3 : 0, hasImpostor ? 1 : 3, 0, impostorBufferOffset);
  cb.flushPerDraw();
  d3d::set_immediate_const(STAGE_VS, ZERO_PTR<uint32_t>(), 1);

  RenderableInstanceResource *riRes = riLodResource.lods[riLodResource.getQlBestLod()].scene;
  ShaderMesh *mesh = riRes->getMesh()->getMesh()->getMesh();
  if (mesh)
  {
    mesh->render();
    mesh->render_trans();
  }

  rendinst::render::endRenderInstancing();
}

void PixelPerfectSelection::getHitsAt(IGenViewportWnd &wnd, int pickX, int pickY,
  dag::Vector<IPixelPerfectSelectionService::Hit> &hits)
{
  if (hits.empty())
    return;

  if (!isInited())
    init();

  d3d::GpuAutoLock gpuLock;

  Driver3dRenderTarget prevRT;
  d3d::get_render_target(prevRT);

  int prevViewX, prevViewY, prevViewWidth, prevViewHeight;
  float prevViewMinZ, prevViewMaxZ;
  d3d::getview(prevViewX, prevViewY, prevViewWidth, prevViewHeight, prevViewMinZ, prevViewMaxZ);

  ScopeViewProjMatrix scopeViewProjMatrix;

  int viewportWidth, viewportHeight;
  wnd.getViewportSize(viewportWidth, viewportHeight);

  float zNear;
  float zFar;
  wnd.getZnearZfar(zNear, zFar);

  const float fov = wnd.getFov();
  const TMatrix4 projMatrix = makeProjectionMatrixForViewRegion(viewportWidth, viewportHeight, fov, zNear, zFar, pickX, pickY, 1, 1);

  d3d::set_render_target(nullptr, 0); // We only use the depth buffer.
  d3d::set_depth(depthRt.getTex2D(), DepthAccess::RW);
  d3d::setview(0, 0, 1, 1, prevViewMinZ, prevViewMaxZ);
  d3d::settm(TM_PROJ, &projMatrix);

  const int lastFrameBlockId = ShaderGlobal::getBlock(ShaderGlobal::LAYER_FRAME);
  ShaderGlobal::setBlock(global_frame_block_id, ShaderGlobal::LAYER_FRAME);

  IDynRenderService *rs = EDITORCORE->queryEditorInterface<IDynRenderService>();
  for (int i = 0; i < hits.size(); ++i)
  {
    IPixelPerfectSelectionService::Hit &hit = hits[i];

    d3d::clearview(CLEAR_ZBUFFER, 0, 0.0f, 0);

    if (hit.riLodResource || hit.rendInstExtraResourceIndex >= 0)
    {
      Point4 data[4];
      data[0] = Point4(hit.transform.getcol(0).x, hit.transform.getcol(1).x, hit.transform.getcol(2).x, hit.transform.getcol(3).x),
      data[1] = Point4(hit.transform.getcol(0).y, hit.transform.getcol(1).y, hit.transform.getcol(2).y, hit.transform.getcol(3).y),
      data[2] = Point4(hit.transform.getcol(0).z, hit.transform.getcol(1).z, hit.transform.getcol(2).z, hit.transform.getcol(3).z);
      data[3] = Point4(0, 0, 0, 0);
      rendinstMatrixBuffer.get()->updateData(0, sizeof(Point4) * 4U, &data, 0);
    }

    if (hit.riLodResource)
    {
      renderLodResource(*hit.riLodResource, hit.transform);
    }
    else if (hit.rendInstExtraResourceIndex >= 0)
    {
      IPoint2 offsAndCnt(0, 1);
      uint16_t riIdx = hit.rendInstExtraResourceIndex;
      uint32_t zeroLodOffset = 0;
      SCENE_LAYER_GUARD(rendinst_scene_block_id);

      rendinst::render::ensureElemsRebuiltRIGenExtra(/*gpu_instancing = */ false);

      rendinst::render::renderRIGenExtraFromBuffer(rendinstMatrixBuffer.get(), dag::ConstSpan<IPoint2>(&offsAndCnt, 1),
        dag::ConstSpan<uint16_t>(&riIdx, 1), dag::ConstSpan<uint32_t>(&zeroLodOffset, 1), rendinst::RenderPass::Depth,
        rendinst::OptimizeDepthPass::Yes, rendinst::OptimizeDepthPrepass::No, rendinst::IgnoreOptimizationLimits::No,
        rendinst::LayerFlag::Opaque);
    }
    else
    {
      G_ASSERT(hit.sceneInstance);
      if (rs)
        rs->renderOneDynModelInstance(hit.sceneInstance, IDynRenderService::Stage::STG_RENDER_DYNAMIC_OPAQUE);
    }

    if (!getHitFromDepthBuffer(hit.z))
    {
      hits.erase_unsorted(&hit);
      --i;
    }
  }

  ShaderGlobal::setBlock(lastFrameBlockId, ShaderGlobal::LAYER_FRAME);

  d3d::setview(prevViewX, prevViewY, prevViewWidth, prevViewHeight, prevViewMinZ, prevViewMaxZ);
  d3d::set_render_target(prevRT);

  eastl::sort(hits.begin(), hits.end(), [](const IPixelPerfectSelectionService::Hit &a, const IPixelPerfectSelectionService::Hit &b) {
    if (a.z > b.z)
      return true;
    if (a.z < b.z)
      return false;
    if (a.userData > b.userData)
      return true;
    if (a.userData < b.userData)
      return false;
    return &a > &b;
  });
}
