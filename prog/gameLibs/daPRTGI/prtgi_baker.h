//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "prtgi_data.h"
#include <EASTL/functional.h>

namespace daprtgi
{

class PRTBaker
{
public:
  PRTBaker() = default;

  // Callback for progress reporting: (step_name, progress_0_to_1)
  using ProgressCallback = eastl::function<void(const char *, float)>;

  bool bakeCurrentScene(PRTAsset &out_asset, float spacing = 2.0f, int sector_size = 32,
                        ProgressCallback progress_cb = nullptr, const char *output_dir = "prtgi");

  uint64_t computeCurrentSceneGeometryHash();

private:
  void generateSampleDirections(eastl::vector<Point3> &out_dirs, eastl::vector<float> &out_weights, int count = 32);
};

} // namespace daprtgi

