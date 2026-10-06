// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/SGSR2.h>

#include <3d/dag_textureIDHolder.h>
#include <perfMon/dag_statDrv.h>
#include <render/daFrameGraph/daFG.h>
#include <render/antialiasing.h>
#include <render/cameraParams.h>

SGSR2::SGSR2(const IPoint2 &outputResolution) :
  AntiAliasing(computeInputResolution(outputResolution), outputResolution), available(false)
{
  if (!render::antialiasing::try_init_sgsr2(outputResolution, inputResolution))
    return;

  lodBias = render::antialiasing::get_mip_bias();

  applierNode = dafg::register_node("sgsr2", DAFG_PP_NODE_SRC, [outputResolution](dafg::Registry registry) {
    registry.multiplex(dafg::multiplexing::Mode::Viewport);
    auto opaqueFinalTargetHndl =
      registry.readTexture("target_for_transparency").atStage(dafg::Stage::PS).useAs(dafg::Usage::SHADER_RESOURCE).handle();

    registry.readTexture("depth_after_transparency").atStage(dafg::Stage::PS).bindToShaderVar("depth_gbuf");
    auto motionVecsHndl =
      registry.readTexture("motion_vecs_after_transparency").atStage(dafg::Stage::PS).useAs(dafg::Usage::SHADER_RESOURCE).handle();

    auto antialiasedHndl =
      registry.createTexture2d("frame_after_aa", {render::antialiasing::get_frame_after_aa_flags() | TEXCF_RTARGET, outputResolution})
        .atStage(dafg::Stage::PS)
        .useAs(dafg::Usage::COLOR_ATTACHMENT)
        .handle();

    auto camera = registry.readBlob<CameraParams>("current_camera").handle();
    auto cameraHistory = registry.readBlobHistory<CameraParams>("current_camera").handle();

    return [motionVecsHndl, opaqueFinalTargetHndl, antialiasedHndl, camera, cameraHistory] {
      render::antialiasing::ApplyContext ctx;
      ctx.motionTexture = motionVecsHndl.get();
      ctx.jitterPixelOffset = camera.ref().jitterOffset;
      ctx.persp = camera.ref().noJitterPersp;
      ctx.resetHistory = is_teleporting(camera.ref(), cameraHistory.ref());
      render::antialiasing::apply_sgsr2(opaqueFinalTargetHndl.get(), antialiasedHndl.get(), ctx.resetHistory);
    };
  });

  available = true;
}
