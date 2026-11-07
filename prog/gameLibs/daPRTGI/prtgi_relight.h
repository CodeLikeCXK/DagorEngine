//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "prtgi_data.h"
#include "prtgi_residency.h"
#include <shaders/dag_computeShaders.h>

namespace daprtgi
{

class PRTRelightPass
{
public:
  PRTRelightPass() = default;
  ~PRTRelightPass() { close(); }

  void init();
  void close();

  void relightSectors(const eastl::vector<int> &selected_sectors, PRTAsset &asset,
                      PRTSectorResidency &residency, const Point3 &camera_pos,
                      float sky_intensity);

  void updateGlobalShaderConstants(const PRTGridDesc &grid, const PRTSectorResidency &residency,
                                   const Point3 &camera_pos, int cascade_count);

private:
  ComputeShaderElement *brickRelightCS = nullptr;
  ComputeShaderElement *probeRelightCS = nullptr;
  ComputeShaderElement *pyramidCS = nullptr;
  ComputeShaderElement *publishCS = nullptr;

  int prtGridOriginVarId = -1;
  int prtGridMinVarId = -1;
  int prtGridCountVarId = -1;
  int prtSlotCountVarId = -1;
  int prtGridSpacingVarId = -1;
  int prtCascadeCountVarId = -1;
  int prtVolumeEnabledVarId = -1;
  int prtCoefficientVoxel3DVarId = -1;
};

} // namespace daprtgi

