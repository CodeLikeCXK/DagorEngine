//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "prtgi_data.h"
#include "prtgi_residency.h"

namespace daprtgi
{

class PRTGIDebugRenderer
{
public:
  PRTGIDebugRenderer() = default;

  void drawWorldDebug(const PRTAsset &asset, const PRTSectorResidency &residency,
                      DebugVisualMode mode, const Point3 &camera_pos);

  void drawOnScreenHUD(const PRTGIStats &stats, const PRTGISettings &settings);

  void drawImGuiWindow(const PRTGIStats &stats, PRTGISettings &settings);
};

} // namespace daprtgi

