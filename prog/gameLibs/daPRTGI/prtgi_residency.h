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

  uint64_t lastUseTick = 0;
  size_t vramBytes = 0;

  void release();
};

class PRTSectorResidency
{
public:
  PRTSectorResidency() = default;
  ~PRTSectorResidency() { release(); }

  // use_depth_occlusion=false skips the per-probe depth-moment buffer entirely (Low/Baked tier):
  // occlusion then comes from the baked per-probe ambient cube, saving ~60 MB at this scene size.
  void init(const PRTGridDesc &grid, int cascade_count = 3, bool use_depth_occlusion = true);
  void release();

  SectorGPUData *getOrUploadSector(const PRTSectorData &sector_data);
  void evictSector(int sector_index);
  bool isSectorResident(int sector_index) const;

  // The publication resource is a VOLTEX, not a 2D texture. getTex2D()
  // returns nullptr for a volume texture and incorrectly disabled PRTGI in
  // every consumer via _prtVolumeEnabled.
  BaseTexture *getCoefficientVoxel3D() const { return coefficientVoxel3D.getBaseTex(); }
  TEXTUREID getCoefficientVoxel3DId() const { return coefficientVoxel3DTexId; }
  // .xy = 6-axis sky cube, .z = validity bit + relocation offset. One fetch instead of two.
  Sbuffer *getProbeInfoBuf() const { return probeInfoBuf; }
  Sbuffer *getProbeDepthBuf() const { return probeDepthBuf; }
  void publishSkyMasks(const PRTAsset &asset);

  const IPoint3 &getSlotCount() const { return slotCount; }
  uint32_t getResidentCount() const { return (uint32_t)residentSectors.size(); }
  // Working set of per-sector input buffers kept on the GPU. The published GI itself lives in the
  // coefficient volume, so these are only needed while a sector is being relit.
  void setResidentLimit(int limit) { residentLimit = (limit > 1) ? limit : 1; }
  size_t getTotalVramBytes() const { return totalVramBytes; }

  void afterReset();

private:
  PRTGridDesc gridDesc;
  int cascadeCount = 3;
  bool depthOcclusion = true;
  IPoint3 slotCount = IPoint3(32, 16, 32);

  UniqueTex coefficientVoxel3D;
  TEXTUREID coefficientVoxel3DTexId = BAD_TEXTUREID;
  Sbuffer *probeInfoBuf = nullptr;
  Sbuffer *probeDepthBuf = nullptr;

  eastl::hash_map<int, SectorGPUData> residentSectors;
  size_t totalVramBytes = 0;
  int residentLimit = 3;
  uint64_t useTickCounter = 0;
};

} // namespace daprtgi
