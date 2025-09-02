// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/viewVecs.h>
#include <render/daFrameGraph/daFG.h>
#include <render/world/cameraParams.h>
#include "frameGraphNodes.h"

dafg::NodeHandle makeUnderWaterFogNode()
{
  return dafg::register_node("under_water_fog_node", DAFG_PP_NODE_SRC, [](dafg::Registry registry) {
    registry.orderMeAfter("darg_in_world_panels_trans_node");
    registry.requestRenderPass().color({"target_for_transparency"});
    registry.requestState().setFrameBlock("global_frame");
    auto underwaterHndl = registry.readBlob<bool>("is_underwater").handle();
    auto cameraHndl = registry.readBlob<CameraParams>("current_camera").handle();
    registry.readTexture("depth_for_transparency").atStage(dafg::Stage::PS).bindToShaderVar("depth_gbuf");
    registry.read("gbuf_sampler").blob<d3d::SamplerHandle>().bindToShaderVar("depth_gbuf_samplerstate");
    return [renderer = PostFxRenderer("underwater_fog"), underwaterHndl, cameraHndl] {
      if (!underwaterHndl.ref())
        return;
      // todo: render underwater fog with volumeLoight if it is on
      set_viewvecs_to_shader(cameraHndl.ref().viewTm, cameraHndl.ref().jitterProjTm);
      renderer.render();
    };
  });
}
