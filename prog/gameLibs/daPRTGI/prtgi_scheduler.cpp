//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_scheduler.h"
#include <math/dag_mathBase.h>
#include <EASTL/algorithm.h>

namespace daprtgi
{

const eastl::vector<int> &PRTSectorScheduler::select(const Point3 &camera_pos, int budget)
{
  requestedSectors.clear();
  updatedCount = 0;

  if (!assetPtr || assetPtr->sectors.empty())
  {
    nearSector = -1;
    return requestedSectors;
  }

  const auto &grid = assetPtr->grid;
  Point3 coord = (camera_pos - grid.origin) / grid.spacing - Point3((float)grid.minCoord.x, (float)grid.minCoord.y, (float)grid.minCoord.z);
  int x = clamp((int)floorf(coord.x), 0, grid.count.x - 1);
  int y = clamp((int)floorf(coord.y), 0, grid.count.y - 1);
  int z = clamp((int)floorf(coord.z), 0, grid.count.z - 1);

  uint32_t probeIdx = grid.getProbeIndex(IPoint3(x, y, z) + grid.minCoord);
  if (probeIdx < assetPtr->probeToSectorMap.size())
    nearSector = assetPtr->probeToSectorMap[probeIdx];
  else
    nearSector = 0;

  int totalSectors = (int)assetPtr->sectors.size();

  if (budget <= 1)
  {
    // Keep only the near sector hot. Alternating it with a rotating background sector forced a
    // fresh ~20 MB sector upload every other frame, which the residency LRU then evicted again.
    // The other sectors are published once by the full relight on load; re-publish them in bulk
    // at scripted lighting changes instead of streaming them continuously.
    if (nearSector >= 0 && nearSector < totalSectors)
      requestedSectors.push_back(nearSector);
  }
  else
  {
    if (nearSector >= 0 && nearSector < totalSectors)
      requestedSectors.push_back(nearSector);

    if (backgroundSector != nearSector && backgroundSector >= 0 && backgroundSector < totalSectors)
      requestedSectors.push_back(backgroundSector);

    for (int offset = 1; (int)requestedSectors.size() < min(budget, totalSectors) && offset < totalSectors; ++offset)
    {
      int index = (backgroundSector + offset) % totalSectors;
      if (eastl::find(requestedSectors.begin(), requestedSectors.end(), index) == requestedSectors.end())
        requestedSectors.push_back(index);
    }
  }

  return requestedSectors;
}

void PRTSectorScheduler::skip(int sector)
{
  if (!assetPtr || assetPtr->sectors.empty()) return;
  if (sector == backgroundSector)
    backgroundSector = (backgroundSector + 1) % (int)assetPtr->sectors.size();
}

void PRTSectorScheduler::updated(int sector)
{
  if (!assetPtr || assetPtr->sectors.empty()) return;
  updatedCount++;
  if (sector == backgroundSector)
    backgroundSector = (backgroundSector + 1) % (int)assetPtr->sectors.size();
}

} // namespace daprtgi

