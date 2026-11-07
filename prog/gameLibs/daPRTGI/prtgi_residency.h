//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "prtgi_data.h"
#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_tex3d.h>
#include <drv/3d/dag_buffers.h>
#include <3d/dag_resPtr.h>
#include <EASTL/vector.h>
#include <EASTL/hash_map.h>

namespace daprtgi
{

struct SectorGPUData
{
  int sectorIndex = -1;
  uint32_t probeCount = 0;
  uint32_t factorCount = 0;
  uint32_t brickCount = 0;
  uint32_t surfelCount = 0;

  Sbuffer *probeDataBuf = nullptr;
  Sbuffer *factorsBuf = nullptr;
  Sbuffer *skyVisBuf = nullptr;
  Sbuffer *brickInfoBuf = nullptr;
  Sbuffer *surfelsBuf = nullptr;
  Sbuffer *brickRadianceBuf = nullptr;
  Sbuffer *probeSHBuf = nullptr;

  void release();
};

class PRTSectorResidency
{
public:
  PRTSectorResidency() = default;
  ~PRTSectorResidency() { release(); }

  void init(const PRTGridDesc &grid, int cascade_count = 3);
  void release();

  SectorGPUData *getOrUploadSector(const PRTSectorData &sector_data);
  void evictSector(int sector_index);
  bool isSectorResident(int sector_index) const;

  BaseTexture *getCoefficientVoxel3D() const { return coefficientVoxel3D.getTex2D(); }
  TEXTUREID getCoefficientVoxel3DId() const { return coefficientVoxel3DTexId; }

  const IPoint3 &getSlotCount() const { return slotCount; }
  uint32_t getResidentCount() const { return (uint32_t)residentSectors.size(); }
  size_t getTotalVramBytes() const { return totalVramBytes; }

  void afterReset();

private:
  PRTGridDesc gridDesc;
  int cascadeCount = 3;
  IPoint3 slotCount = IPoint3(32, 16, 32);

  UniqueTex coefficientVoxel3D;
  TEXTUREID coefficientVoxel3DTexId = BAD_TEXTUREID;

  eastl::hash_map<int, SectorGPUData> residentSectors;
  size_t totalVramBytes = 0;
};

} // namespace daprtgi

