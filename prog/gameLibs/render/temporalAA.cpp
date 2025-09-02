// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/temporalAA.h>
#include <drv/3d/dag_renderTarget.h>
#include <drv/3d/dag_driver.h>
#include <math/dag_TMatrix4D.h>
#include <math/random/dag_halton.h>
#include <shaders/dag_shaders.h>
#include <shaders/dag_postFxRenderer.h>
#include <perfMon/dag_cpuFreq.h>
#include <util/dag_convar.h>
#include <ioSys/dag_dataBlock.h>
#include <shaders/dag_shaderBlock.h>


#define GLOBAL_VARS_LIST                       \
  VAR(taa_input_resolution)                    \
  VAR(taa_output_resolution)                   \
  VAR(taa_history_tex)                         \
  VAR(taa_history_tex_samplerstate)            \
  VAR(taa_frame_tex)                           \
  VAR(taa_frame_tex_samplerstate)              \
  VAR(taa_was_dynamic_tex)                     \
  VAR(taa_was_dynamic_tex_samplerstate)        \
  VAR(taa_clamping_gamma_factor)               \
  VAR(taa_new_frame_weight)                    \
  VAR(taa_new_frame_weight_for_motion)         \
  VAR(taa_new_frame_weight_dynamic)            \
  VAR(taa_new_frame_weight_dynamic_for_motion) \
  VAR(taa_new_frame_weight_motion_lerp0)       \
  VAR(taa_new_frame_weight_motion_lerp1)       \
  VAR(taa_sharpening_factor)                   \
  VAR(taa_motion_difference_max_inv)           \
  VAR(taa_motion_difference_max_weight)        \
  VAR(taa_luminance_max)                       \
  VAR(taa_restart_temporal)                    \
  VAR(taa_filter0)                             \
  VAR(taa_filter1)                             \
  VAR(taa_filter2)                             \
  VAR(reproject_psf_0)                         \
  VAR(reproject_psf_1)                         \
  VAR(reproject_psf_2)                         \
  VAR(reproject_psf_3)                         \
  VAR(taa_jitter_offset)                       \
  VAR(taa_adaptive_filter)                     \
  VAR(taa_scale_aabb_with_motion_steepness)    \
  VAR(taa_scale_aabb_with_motion_max)          \
  VAR(taa_precomputed_weights_samplerstate)

#define VAR(a) static int gv_##a = -1;
GLOBAL_VARS_LIST
#undef VAR

static void init_gvars(){
#define VAR(a) gv_##a = ::get_shader_variable_id(#a, true);
  GLOBAL_VARS_LIST
#undef VAR
}


TemporalAA::TemporalAA(const char *shader, const IPoint2 &input_resolution, const IPoint2 &output_resolution, int resolve_tex_fmt,
  bool low_quality, bool req_dynamic_tex, bool mobile_taa, const char *name) :
  inputResolution(input_resolution),
  outputResolution(output_resolution),
  frame(0, 0),
  lodBias(-log2(float(output_resolution.y) / float(input_resolution.y))),
  precomputedWeightsReady(false)
{
  G_UNUSED(low_quality);
  render.init(shader);
  init_gvars();

  const char defaultName[] = "taa";
  if (!name)
    name = defaultName;

  int historyFmt = mobile_taa ? resolve_tex_fmt : TEXFMT_A16B16G16R16F;
  historyFmt |= TEXCF_RTARGET;

  historyTexPool = RTargetPool::get(outputResolution.x, outputResolution.y, historyFmt, 1);

  wasDynamicTexPool =
    req_dynamic_tex ? RTargetPool::get(outputResolution.x, outputResolution.y, TEXCF_RTARGET | TEXFMT_R8, 1) : nullptr;

  if (mobile_taa)
  {
    const int precomputedWeightsSize = 64;
    taaPrecomputedWeights.set(d3d::create_tex(nullptr, precomputedWeightsSize, precomputedWeightsSize,
                                TEXFMT_A16B16G16R16F | TEXCF_RTARGET, 1, "taa_precomputed_weights"),
      "taa_precomputed_weights");
    d3d::SamplerInfo smpInfo;
    smpInfo.filter_mode = d3d::FilterMode::Linear;
    d3d::SamplerHandle smp = d3d::request_sampler(smpInfo);
    ShaderGlobal::set_sampler(gv_taa_precomputed_weights_samplerstate, smp);
  }
  {
    d3d::SamplerInfo smpInfo;
    smpInfo.address_mode_u = smpInfo.address_mode_v = smpInfo.address_mode_w = d3d::AddressMode::Clamp;
    smpInfo.filter_mode = d3d::FilterMode::Linear;
    d3d::SamplerHandle smp = d3d::request_sampler(smpInfo);
    ShaderGlobal::set_sampler(gv_taa_frame_tex_samplerstate, smp);
    ShaderGlobal::set_sampler(gv_taa_history_tex_samplerstate, smp);
    ShaderGlobal::set_sampler(gv_taa_was_dynamic_tex_samplerstate, smp);
  }
}

bool TemporalAA::beforeRenderFrame()
{
  static int64_t reft = 0;
  dtUsecs = get_time_usec(reft);
  reft = ref_time_ticks();

  return isValid();
}

bool TemporalAA::beforeRenderView(const TMatrix4 &uv_reproject_tm_no_jitter)
{
  jitterPixelOfs = get_taa_jitter(frame, params);
  jitterTexelOfs.x = jitterPixelOfs.x * (2.f / inputResolution.x);
  jitterTexelOfs.y = jitterPixelOfs.y * (2.f / inputResolution.y);

  set_temporal_parameters(uv_reproject_tm_no_jitter, jitterPixelOfs, frame == 0 ? 10.f : dtUsecs / 1e6, inputResolution.x, params);

  ShaderGlobal::set_color4(gv_taa_input_resolution, inputResolution.x, inputResolution.y, 0, 0);
  ShaderGlobal::set_color4(gv_taa_output_resolution, outputResolution.x, outputResolution.y, 0, 0);

  return isValid();
}

void TemporalAA::applyImpl(TEXTUREID currentFrameId)
{
  if (taaPrecomputedWeights.getTex2D() && !precomputedWeightsReady)
  {
    SCOPE_RENDER_TARGET;
    PostFxRenderer precompute;
    precompute.init("taa_precompute");
    SCOPE_RESET_SHADER_BLOCKS;
    d3d::set_render_target();
    d3d::set_render_target(0, taaPrecomputedWeights.getTex2D(), 0);
    ShaderGlobal::set_color4(gv_taa_input_resolution, inputResolution.x, inputResolution.y, 0, 0);
    ShaderGlobal::set_color4(gv_taa_output_resolution, outputResolution.x, outputResolution.y, 0, 0);
    precompute.render();
    taaPrecomputedWeights.setVar();
    precomputedWeightsReady = true;
  }

  RTarget::Ptr nextHistory = historyTexPool->acquire();
  d3d::set_render_target(1, nextHistory->getTex2D(), 0);

  RTarget::Ptr nextWasDynamic;
  if (wasDynamicTexPool)
  {
    nextWasDynamic = wasDynamicTexPool->acquire();
    d3d::set_render_target(2, nextWasDynamic->getTex2D(), 0);
  }
  d3d::clearview(CLEAR_DISCARD_TARGET, 0, 0, 0);

  const TEXTUREID historyTexId = historyTex.current() ? historyTex.current()->getTexId() : BAD_TEXTUREID;
  const TEXTUREID wasDynamicTexId = wasDynamicTex.current() ? wasDynamicTex.current()->getTexId() : BAD_TEXTUREID;
  ShaderGlobal::set_texture(gv_taa_history_tex, historyTexId);
  ShaderGlobal::set_texture(gv_taa_was_dynamic_tex, wasDynamicTexId);

  ShaderGlobal::set_texture(gv_taa_frame_tex, currentFrameId);
  render.render();
  ShaderGlobal::set_texture(gv_taa_frame_tex, BAD_TEXTUREID);

  historyTex = nextHistory;
  wasDynamicTex = nextWasDynamic;

  frame++;
}

void TemporalAA::apply(TEXTUREID currentFrameId, Texture *target)
{
  SCOPE_RENDER_TARGET;

  d3d::set_render_target(target, 0);

  applyImpl(currentFrameId);
}

void TemporalAA::applyToCurrentTarget(TEXTUREID currentFrameId)
{
  SCOPE_RENDER_TARGET;
  applyImpl(currentFrameId);
}

void TemporalAA::setCurrentView(int view)
{
  historyTex.setCurrentView(view);
  wasDynamicTex.setCurrentView(view);
  frame.setCurrentView(view);
}

TemporalAA::Params TemporalAA::getDefaultParams() const
{
  Params result;

  bool isUpsampling = outputResolution != inputResolution;

  if (isUpsampling)
  {
    Point2 upsamplingRatio(float(outputResolution.x) / float(inputResolution.x), float(outputResolution.y) / float(inputResolution.y));
    result.useAdaptiveFilter = true;
    result.subsampleScale = 1.0f;

    // when upsamling relaxation of variance clipping results in sharper still images.
    result.scaleAabbWithMotionSteepness = 1e5f;
    result.scaleAabbWithMotionMax = 0.1f;
    constexpr int baseJitterSequenceLength = 16;
    result.subsamples = ceil(baseJitterSequenceLength * upsamplingRatio.x * upsamplingRatio.y);
  }

  return result;
}

void TemporalAA::loadParamsFromBlk(const DataBlock *taaBlk)
{
  G_ASSERT(taaBlk);

#define LOAD_INT(var)  p.var = taaBlk->getInt(#var, p.var)
#define LOAD_BOOL(var) p.var = taaBlk->getBool(#var, p.var)
#define LOAD_REAL(var) p.var = taaBlk->getReal(#var, p.var)

  Params p = getDefaultParams();

  LOAD_INT(subsamples);
  LOAD_BOOL(useHalton);
  LOAD_REAL(subsampleScale);
  LOAD_REAL(newframeFalloffStill);
  LOAD_REAL(newframeFalloffMotion);
  LOAD_REAL(newframeFalloffDynamicStill);
  LOAD_REAL(newframeFalloffDynamicMotion);
  LOAD_REAL(frameWeightMinStill);
  LOAD_REAL(frameWeightMinMotion);
  LOAD_REAL(frameWeightMinDynamicStill);
  LOAD_REAL(frameWeightMinDynamicMotion);
  LOAD_REAL(frameWeightMotionThreshold);
  LOAD_REAL(frameWeightMotionMax);
  LOAD_REAL(motionDifferenceMax);
  LOAD_REAL(motionDifferenceMaxWeight);
  LOAD_REAL(sharpening);
  LOAD_REAL(clampingGamma);
  LOAD_REAL(scaleAabbWithMotionMax);
  LOAD_REAL(scaleAabbWithMotionMaxForTAAU);

  if (outputResolution != inputResolution && p.scaleAabbWithMotionMaxForTAAU > 0)
    p.scaleAabbWithMotionMax = p.scaleAabbWithMotionMaxForTAAU;

  params = p;

#undef LOAD_INT
#undef LOAD_BOOL
#undef LOAD_REAL
}

static float gaussianKernel(const Point2 &uv) { return expf(-2.29f * lengthSq(uv)); }

extern Point2 get_halton_jitter(int counter, int subsamples, float subsample_scale)
{
  // +1 is because halton sequence returns 0 when index is 0. This causes first jitter to be (-0.5,-0.5).
  // Halton sequence is correct when the index starts from 1.
  int index = counter % subsamples + 1;
  return Point2(halton_sequence(index, 2) - 0.5f, halton_sequence(index, 3) - 0.5f) * subsample_scale;
}

Point2 get_taa_jitter(int counter, const TemporalAAParams &p)
{
  static const Point2 SSAA8x[8] = {Point2(0.0625, -0.1875), Point2(-0.0625, 0.1875), Point2(0.3125, 0.0625), Point2(-0.1875, -0.3125),
    Point2(-0.3125, 0.3125), Point2(-0.4375, -0.0625), Point2(0.1875, 0.4375), Point2(0.4375, -0.4375)};

  if (p.useHalton)
    return get_halton_jitter(counter, p.subsamples, p.subsampleScale);
  else
  {
    int max_count = min(p.subsamples, 8);
    return SSAA8x[counter % max_count] * p.subsampleScale;
  }
}

void set_temporal_reprojection_matrix(const TMatrix4 &uv_reproject_tm_no_jitter)
{
  if (gv_reproject_psf_0 < 0)
    init_gvars();

  ShaderGlobal::set_color4(gv_reproject_psf_0, uv_reproject_tm_no_jitter.getcol(0));
  ShaderGlobal::set_color4(gv_reproject_psf_1, uv_reproject_tm_no_jitter.getcol(1));
  ShaderGlobal::set_color4(gv_reproject_psf_2, uv_reproject_tm_no_jitter.getcol(2));
  ShaderGlobal::set_color4(gv_reproject_psf_3, uv_reproject_tm_no_jitter.getcol(3));
}

void set_temporal_resampling_filter_parameters(const Point2 &temporal_jitter_proj_offset)
{
  const size_t FILTER_WEIGHT_COUNT = 9;
  Point2 neighbourOffsets[FILTER_WEIGHT_COUNT] = {Point2{0.0f, 0.0f}, Point2{-1.0f, 0.0f}, Point2{0.0f, -1.0f}, Point2{1.0f, 0.0f},
    Point2{0.0f, 1.0f}, Point2{-1.0f, -1.0f}, Point2{1.0f, -1.0f}, Point2{-1.0f, 1.0f}, Point2{1.0f, 1.0f}};

  float filter[FILTER_WEIGHT_COUNT];
  for (size_t i = 0; i < FILTER_WEIGHT_COUNT; ++i)
    filter[i] = gaussianKernel(neighbourOffsets[i] - temporal_jitter_proj_offset);

  if (gv_taa_filter0 < 0)
    init_gvars();

  ShaderGlobal::set_color4(gv_taa_filter0, filter[0], filter[1], filter[2], filter[3]);
  ShaderGlobal::set_color4(gv_taa_filter1, filter[4], filter[5], filter[6], filter[7]);
  ShaderGlobal::set_real(gv_taa_filter2, filter[8]);
  ShaderGlobal::set_color4(gv_taa_jitter_offset, temporal_jitter_proj_offset.x, temporal_jitter_proj_offset.y, 0, 0);
}

void set_temporal_miscellaneous_parameters(float dt, int wd, const TemporalAAParams &p)
{
  bool shouldRestart = dt >= 0.5 || dt < 0;
  dt = clamp(dt, 0.f, .4f);
  const float taaMotionDifferenceMax = p.motionDifferenceMax,
              taaMotionDifferenceMaxWeight = shouldRestart ? 1 : p.motionDifferenceMaxWeight, taaLuminanceMax = 100.f;

  // Computes the frame weight using an exponential dt function
  const float newFrameWeight =
    shouldRestart ? 1.f : min(1.0f - expf(-dt / max(p.newframeFalloffStill, FLT_EPSILON)), p.frameWeightMinStill);
  const float newFrameWeightForMotion =
    shouldRestart ? 1.f : min(1.0f - expf(-dt / max(p.newframeFalloffMotion, FLT_EPSILON)), p.frameWeightMinMotion);

  const float newFrameWeightDynamic =
    shouldRestart ? 1.f : min(1.0f - expf(-dt / max(p.newframeFalloffDynamicStill, FLT_EPSILON)), p.frameWeightMinDynamicStill);
  const float newFrameWeightDynamicForMotion =
    shouldRestart ? 1.f : min(1.0f - expf(-dt / max(p.newframeFalloffDynamicMotion, FLT_EPSILON)), p.frameWeightMinDynamicMotion);

  const float sharpening = max(0.5f + p.sharpening, 0.5f); // Catmull-rom sharpening baseline is 0.5.
  const float motionDifferenceMaxInverse = (float)wd / max(taaMotionDifferenceMax, FLT_EPSILON);
  const float motionDifferenceMaxWeight = clamp(taaMotionDifferenceMaxWeight, 0.0f, 1.0f);
  const float luminanceMax = max(taaLuminanceMax, 0.0f);

  if (gv_taa_clamping_gamma_factor < 0)
    init_gvars();

  ShaderGlobal::set_real(gv_taa_clamping_gamma_factor, p.clampingGamma);
  ShaderGlobal::set_real(gv_taa_new_frame_weight, newFrameWeight);
  ShaderGlobal::set_real(gv_taa_new_frame_weight_for_motion, newFrameWeightForMotion);
  ShaderGlobal::set_real(gv_taa_new_frame_weight_dynamic, newFrameWeightDynamic);
  ShaderGlobal::set_real(gv_taa_new_frame_weight_dynamic_for_motion, newFrameWeightDynamicForMotion);
  ShaderGlobal::set_real(gv_taa_new_frame_weight_motion_lerp0, p.frameWeightMotionThreshold);
  ShaderGlobal::set_real(gv_taa_new_frame_weight_motion_lerp1, p.frameWeightMotionMax);

  ShaderGlobal::set_real(gv_taa_sharpening_factor, sharpening);
  ShaderGlobal::set_real(gv_taa_motion_difference_max_inv, motionDifferenceMaxInverse);
  ShaderGlobal::set_real(gv_taa_motion_difference_max_weight, motionDifferenceMaxWeight);
  ShaderGlobal::set_real(gv_taa_luminance_max, luminanceMax);
  ShaderGlobal::set_int(gv_taa_restart_temporal, shouldRestart);

  ShaderGlobal::set_int(gv_taa_adaptive_filter, p.useAdaptiveFilter);
  ShaderGlobal::set_real(gv_taa_scale_aabb_with_motion_steepness, p.scaleAabbWithMotionSteepness);
  ShaderGlobal::set_real(gv_taa_scale_aabb_with_motion_max, p.scaleAabbWithMotionMax);
}

void set_temporal_parameters(const TMatrix4 &uv_reproject_tm_no_jitter, const Point2 &temporal_jitter_proj_offset, float dt, int wd,
  const TemporalAAParams &p)
{
  set_temporal_reprojection_matrix(uv_reproject_tm_no_jitter);
  set_temporal_resampling_filter_parameters(temporal_jitter_proj_offset);
  set_temporal_miscellaneous_parameters(dt, wd, p);
}
