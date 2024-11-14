//============================================================================================================
//   DO NOT REMOVE THIS HEADER UNDER QUALCOMM PRODUCT KIT LICENSE AGREEMENT
//
//                  Copyright (c) 2023 QUALCOMM Technologies Inc.
//                              All Rights Reserved.
//
//                       Snapdragon™ Game Super Resolution
//                       Snapdragon™ GSR
//
//                       Developed by Snapdragon Studios™ (https://www.qualcomm.com/snapdragon-studios)
//
//============================================================================================================
#pragma once
#include <shaders/dag_postFxRenderer.h>
#include <shaders/dag_shaderVar.h>
#include <generic/dag_carray.h>
#include <shaders/dag_shaders.h>
#include <drv/3d/dag_driver.h>

class SnapdragonSuperResolution : public PostFxRenderer
{
public:
    SnapdragonSuperResolution() :  PostFxRenderer("SnapdragonSuperResolution")
    {
        viewport[0] = 0.f;
        viewport[1] = 0.f;
        viewport[2] = 0.f;
        viewport[3] = 0.f;
        snapdragon_super_resolution_inputVarId = get_shader_variable_id("snapdragon_super_resolution_input");
        snapdragon_super_resolution_input_samplerstateVarId = get_shader_variable_id("snapdragon_super_resolution_input_samplerstate");
        snapdragon_super_resolution_ViewportInfoVarId = get_shader_variable_id("snapdragon_super_resolution_ViewportInfo");
    }

    ~SnapdragonSuperResolution() {}

    inline void SetViewport(int32_t x, int32_t y)
    {
        viewport[2] = (float)x;
        viewport[3] = (float)y;
        viewport[0] = viewport[2] != 0.f ? (1.f / viewport[2]) : 0.f;
        viewport[1] = viewport[3] != 0.f ? (1.f / viewport[3]) : 0.f;
    }

    inline void render(const ManagedTex &input, BaseTexture* output)
    {
        Color4 viewportData = Color4(viewport);

        d3d::set_render_target(output, 0);
        ShaderGlobal::set_texture(snapdragon_super_resolution_inputVarId, input);
        ShaderGlobal::set_sampler(snapdragon_super_resolution_input_samplerstateVarId, d3d::request_sampler({}));
        ShaderGlobal::set_color4(snapdragon_super_resolution_ViewportInfoVarId, viewportData);
        PostFxRenderer::render();
    }

    inline void render(const TextureIDPair &input)
    {
        Color4 viewportData = Color4(viewport);
        d3d::set_render_target();
        ShaderGlobal::set_texture(snapdragon_super_resolution_inputVarId, input.getId());
        ShaderGlobal::set_sampler(snapdragon_super_resolution_input_samplerstateVarId, d3d::request_sampler({}));
        ShaderGlobal::set_color4(snapdragon_super_resolution_ViewportInfoVarId, viewportData);
        PostFxRenderer::render();
    }

private:
    float viewport[4];
    int snapdragon_super_resolution_inputVarId, snapdragon_super_resolution_ViewportInfoVarId;
    int snapdragon_super_resolution_input_samplerstateVarId;
};
