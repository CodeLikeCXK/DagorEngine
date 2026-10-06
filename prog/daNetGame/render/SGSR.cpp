// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/SGSR.h>

#include <3d/dag_textureIDHolder.h>
#include <perfMon/dag_statDrv.h>
#include <render/daFrameGraph/daFG.h>
#include <render/antialiasing.h>

SGSR::SGSR(const IPoint2 &outputResolution) :
  AntiAliasing(computeInputResolution(outputResolution), outputResolution), available(false)
{
  if (!render::antialiasing::try_init_sgsr(outputResolution, inputResolution))
    return;

  lodBias = log2(float(inputResolution.y) / float(outputResolution.y));

  applierNode = dafg::register_node("sgsr", DAFG_PP_NODE_SRC, [outputResolution](dafg::Registry registry) {
    registry.multiplex(dafg::multiplexing::Mode::Viewport);
    auto opaqueFinalTargetHndl =
      registry.readTexture("target_for_transparency").atStage(dafg::Stage::PS).useAs(dafg::Usage::SHADER_RESOURCE).handle();

    auto antialiasedHndl =
      registry.createTexture2d("frame_after_aa", {render::antialiasing::get_frame_after_aa_flags() | TEXCF_RTARGET, outputResolution})
        .atStage(dafg::Stage::PS)
        .useAs(dafg::Usage::COLOR_ATTACHMENT)
        .handle();

    return [opaqueFinalTargetHndl, antialiasedHndl] {
      render::antialiasing::apply_sgsr(opaqueFinalTargetHndl.get(), antialiasedHndl.get());
    };
  });

  available = true;
}
