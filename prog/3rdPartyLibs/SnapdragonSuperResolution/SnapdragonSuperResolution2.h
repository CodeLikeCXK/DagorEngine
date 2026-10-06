//============================================================================================================
//   DO NOT REMOVE THIS HEADER UNDER QUALCOMM PRODUCT KIT LICENSE AGREEMENT
//
//                  Copyright (c) 2024 QUALCOMM Technologies Inc.
//                              All Rights Reserved.
//
//                       Snapdragon(TM) Game Super Resolution 2
//                       Snapdragon(TM) GSR 2
//
//============================================================================================================
#pragma once
#include <shaders/dag_postFxRenderer.h>
#include <shaders/dag_shaderVar.h>
#include <shaders/dag_shaders.h>
#include <3d/dag_resPtr.h>
#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_renderTarget.h>
#include <render/antialiasing.h>
#include <math/dag_Point2.h>
#include <math/integer/dag_IPoint2.h>

class SnapdragonSuperResolution2 : public render::antialiasing::AppGlue::SGSR2Interface
{
public:
  SnapdragonSuperResolution2(const IPoint2 &input_res, const IPoint2 &output_res) :
    inputRes(input_res),
    outputRes(output_res),
    convertRenderer("sgsr2_2pass_convert_ps"),
    upscaleRenderer("sgsr2_2pass_upscale_ps"),
    frameCount(0)
  {
    motionDepthClipAlphaBuffer = dag::create_tex(nullptr, inputRes.x, inputRes.y,
      TEXFMT_A16B16G16R16F | TEXCF_RTARGET, 1, "sgsr2_motion_depth_clip_alpha");
    historyBuffer = dag::create_tex(nullptr, outputRes.x, outputRes.y,
      TEXFMT_A16B16G16R16F | TEXCF_RTARGET, 1, "sgsr2_history");

    depth_gbufVarId = get_shader_variable_id("depth_gbuf", true);
    resolved_motion_vectorsVarId = get_shader_variable_id("resolved_motion_vectors", true);
    sgsr2_prev_outputVarId = get_shader_variable_id("sgsr2_prev_output", true);
    sgsr2_motion_depth_clip_alpha_bufferVarId = get_shader_variable_id("sgsr2_motion_depth_clip_alpha_buffer", true);
    sgsr2_input_colorVarId = get_shader_variable_id("sgsr2_input_color", true);
    sgsr2_render_sizeVarId = get_shader_variable_id("sgsr2_render_size", true);
    sgsr2_output_sizeVarId = get_shader_variable_id("sgsr2_output_size", true);
    sgsr2_jitter_offsetVarId = get_shader_variable_id("sgsr2_jitter_offset", true);
    sgsr2_upscale_ratioVarId = get_shader_variable_id("sgsr2_upscale_ratio", true);
    sgsr2_camera_fovVarId = get_shader_variable_id("sgsr2_camera_fov", true);
    sgsr2_min_lerp_contribVarId = get_shader_variable_id("sgsr2_min_lerp_contrib", true);
    sgsr2_resetVarId = get_shader_variable_id("sgsr2_reset", true);
    sgsr2_same_cameraVarId = get_shader_variable_id("sgsr2_same_camera", true);
    sgsr2_same_camera_frame_numVarId = get_shader_variable_id("sgsr2_same_camera_frame_num", true);

    lodBias = log2(float(inputRes.y) / float(outputRes.y));
  }

  ~SnapdragonSuperResolution2() override = default;

  float getLodBias() const override { return lodBias; }

  void beforeRenderView(const render::RenderView &) const override {}

  Point2 getJitterOffset() const override { return jitter; }

  void setJitterOffset(const Point2 &j) { jitter = j; }
  void setCameraFov(float fov_val) { fov = fov_val; }

  void apply(Texture *sourceTex, Texture *destTex, bool reset = false) override
  {
    if (!sourceTex || !destTex)
      return;

    if (reset)
      frameCount = 0;
    else
      frameCount++;

    ShaderGlobal::set_float4(sgsr2_render_sizeVarId, inputRes.x, inputRes.y,
      inputRes.x > 0 ? 1.0f / inputRes.x : 0.0f, inputRes.y > 0 ? 1.0f / inputRes.y : 0.0f);
    ShaderGlobal::set_float4(sgsr2_output_sizeVarId, outputRes.x, outputRes.y,
      outputRes.x > 0 ? 1.0f / outputRes.x : 0.0f, outputRes.y > 0 ? 1.0f / outputRes.y : 0.0f);
    ShaderGlobal::set_float4(sgsr2_jitter_offsetVarId, jitter.x, jitter.y, 0, 0);
    ShaderGlobal::set_float4(sgsr2_upscale_ratioVarId,
      inputRes.x > 0 ? float(outputRes.x) / float(inputRes.x) : 1.0f,
      inputRes.y > 0 ? float(outputRes.y) / float(inputRes.y) : 1.0f, 0, 0);
    ShaderGlobal::set_float(sgsr2_camera_fovVarId, fov > 0.0f ? fov : 1.0f);
    ShaderGlobal::set_float(sgsr2_min_lerp_contribVarId, 0.05f);
    ShaderGlobal::set_int(sgsr2_resetVarId, (reset || frameCount <= 1) ? 1 : 0);
    ShaderGlobal::set_int(sgsr2_same_cameraVarId, reset ? 0 : 1);
    ShaderGlobal::set_int(sgsr2_same_camera_frame_numVarId, frameCount);

    static int global_frameBlockId = ShaderGlobal::getBlockId("global_frame");
    ShaderGlobal::setBlock(-1, ShaderGlobal::LAYER_FRAME);

    // Pass 1: Convert PS -> motionDepthClipAlphaBuffer
    if (motionDepthClipAlphaBuffer.getTex2D())
    {
      d3d::set_render_target({}, DepthAccess::RW, {{motionDepthClipAlphaBuffer.getTex2D(), 0, 0}});
      convertRenderer.render();
    }

    // Pass 2: Upscale PS -> destTex (SV_Target0) + historyBuffer (SV_Target1)
    ShaderGlobal::set_texture(sgsr2_input_colorVarId, sourceTex);
    ShaderGlobal::set_texture(sgsr2_prev_outputVarId, historyBuffer.getTex2D());
    ShaderGlobal::set_texture(sgsr2_motion_depth_clip_alpha_bufferVarId, motionDepthClipAlphaBuffer.getTex2D());

    d3d::set_render_target({}, DepthAccess::RW, {{destTex, 0, 0}, {historyBuffer.getTex2D(), 0, 0}});
    upscaleRenderer.render();

    // Clean up
    ShaderGlobal::set_texture(sgsr2_input_colorVarId, nullptr);
    ShaderGlobal::set_texture(sgsr2_prev_outputVarId, nullptr);
    ShaderGlobal::set_texture(sgsr2_motion_depth_clip_alpha_bufferVarId, nullptr);
    d3d::set_render_target();

    ShaderGlobal::setBlock(global_frameBlockId, ShaderGlobal::LAYER_FRAME);
  }

private:
  IPoint2 inputRes;
  IPoint2 outputRes;
  float lodBias = 0.0f;
  Point2 jitter = Point2::ZERO;
  float fov = 1.0f;
  int frameCount = 0;

  PostFxRenderer convertRenderer;
  PostFxRenderer upscaleRenderer;
  UniqueTex motionDepthClipAlphaBuffer;
  UniqueTex historyBuffer;

  int depth_gbufVarId;
  int resolved_motion_vectorsVarId;
  int sgsr2_prev_outputVarId;
  int sgsr2_motion_depth_clip_alpha_bufferVarId;
  int sgsr2_input_colorVarId;
  int sgsr2_render_sizeVarId;
  int sgsr2_output_sizeVarId;
  int sgsr2_jitter_offsetVarId;
  int sgsr2_upscale_ratioVarId;
  int sgsr2_camera_fovVarId;
  int sgsr2_min_lerp_contribVarId;
  int sgsr2_resetVarId;
  int sgsr2_same_cameraVarId;
  int sgsr2_same_camera_frame_numVarId;
};
