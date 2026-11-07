//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "prtgi_data.h"
#include <EASTL/vector.h>

namespace daprtgi
{

class PRTSectorScheduler
{
public:
  PRTSectorScheduler() = default;

  void reset(const PRTAsset *asset)
  {
    assetPtr = asset;
    requestedSectors.clear();
    backgroundSector = 0;
    backgroundTurn = false;
    nearSector = -1;
    updatedCount = 0;
  }

  const eastl::vector<int> &select(const Point3 &camera_pos, int budget);
  void skip(int sector);
  void updated(int sector);

  int getNearSector() const { return nearSector; }
  int getBackgroundSector() const { return backgroundSector; }
  int getUpdatedCount() const { return updatedCount; }

private:
  const PRTAsset *assetPtr = nullptr;
  eastl::vector<int> requestedSectors;
  int backgroundSector = 0;
  bool backgroundTurn = false;
  int nearSector = -1;
  int updatedCount = 0;
};

} // namespace daprtgi

