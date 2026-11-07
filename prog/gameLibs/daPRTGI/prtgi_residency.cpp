//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_residency.h"
#include <drv/3d/dag_driver.h>
#include <debug/dag_debug.h>

namespace daprtgi
{

void SectorGPUData::release()
{
  destroy_d3dres(probeDataBuf);
  destroy_d3dres(factorsBuf);
  destroy_d3dres(skyVisBuf);
  destroy_d3dres(brickInfoBuf);
  destroy_d3dres(surfelsBuf);
  destroy_d3dres(brickRadianceBuf);
  destroy_d3dres(probeSHBuf);
  sectorIndex = -1;
  probeCount = 0;
  factorCount = 0;
  brickCount = 0;
  surfelCount = 0;
}

void PRTSectorResidency::init(const PRTGridDesc &grid, int cascade_count)
{
  release();
  gridDesc = grid;
  cascadeCount = cascade_count;

  // Horizontal axes are toroidal; Depth slices: [cascade * 9 * slots.y, (cascade + 1) * 9 * slots.y)
  slotCount = IPoint3(32, 16, 32);

  int volW = slotCount.x;
  int volH = slotCount.z;
  int volD = slotCount.y * 9 * cascadeCount;

  coefficientVoxel3D = dag::create_voltex(volW, volH, volD,
    TEXCF_UNORDERED | TEXFMT_A16B16G16R16F, 1, "prt_coefficient_voxel_3d");

  if (coefficientVoxel3D)
  {
    coefficientVoxel3DTexId = coefficientVoxel3D.getTexId();
    debug("PRTGI: Created 3D coefficient voxel texture (%dx%dx%d)", volW, volH, volD);
  }
  else
  {
    logerr("PRTGI: Failed to create 3D coefficient voxel texture!");
  }
}

void PRTSectorResidency::release()
{
  for (auto &it : residentSectors)
    it.second.release();
  residentSectors.clear();

  coefficientVoxel3D.close();
  coefficientVoxel3DTexId = BAD_TEXTUREID;
  totalVramBytes = 0;
}

void PRTSectorResidency::afterReset()
{
  // Recreate GPU 3D texture after graphics device reset
  init(gridDesc, cascadeCount);
  for (auto &it : residentSectors)
    it.second.release();
  residentSectors.clear();
}

bool PRTSectorResidency::isSectorResident(int sector_index) const
{
  return residentSectors.find(sector_index) != residentSectors.end();
}

void PRTSectorResidency::evictSector(int sector_index)
{
  auto it = residentSectors.find(sector_index);
  if (it != residentSectors.end())
  {
    it->second.release();
    residentSectors.erase(it);
  }
}

SectorGPUData *PRTSectorResidency::getOrUploadSector(const PRTSectorData &sector_data)
{
  auto it = residentSectors.find(sector_data.sectorIndex);
  if (it != residentSectors.end())
    return &it->second;

  SectorGPUData gpuData;
  gpuData.sectorIndex = sector_data.sectorIndex;
  gpuData.probeCount = (uint32_t)sector_data.probeIds.size();
  gpuData.factorCount = (uint32_t)sector_data.factors.size();
  gpuData.brickCount = (uint32_t)sector_data.brickInfo.size();
  gpuData.surfelCount = (uint32_t)sector_data.surfels.size();

  if (gpuData.probeCount > 0)
  {
    gpuData.probeDataBuf = d3d::create_sbuffer(sizeof(PRTProbeData), gpuData.probeCount,
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_probe_data");
    if (gpuData.probeDataBuf)
      gpuData.probeDataBuf->updateData(0, sizeof(PRTProbeData) * gpuData.probeCount, sector_data.probeData.data(), VBLOCK_WRITEONLY);

    gpuData.probeSHBuf = d3d::create_sbuffer(sizeof(Point4), gpuData.probeCount * 9,
      SBCF_BIND_UNORDERED | SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_probe_sh");
  }

  if (gpuData.factorCount > 0)
  {
    gpuData.factorsBuf = d3d::create_sbuffer(sizeof(BrickFactor), gpuData.factorCount,
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_factors");
    if (gpuData.factorsBuf)
      gpuData.factorsBuf->updateData(0, sizeof(BrickFactor) * gpuData.factorCount, sector_data.factors.data(), VBLOCK_WRITEONLY);
  }

  if (!sector_data.skyVisibility.empty())
  {
    gpuData.skyVisBuf = d3d::create_sbuffer(sizeof(uint32_t), (uint32_t)sector_data.skyVisibility.size(),
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_sky_vis");
    if (gpuData.skyVisBuf)
      gpuData.skyVisBuf->updateData(0, sizeof(uint32_t) * sector_data.skyVisibility.size(), sector_data.skyVisibility.data(), VBLOCK_WRITEONLY);
  }

  if (gpuData.brickCount > 0)
  {
    gpuData.brickInfoBuf = d3d::create_sbuffer(sizeof(SurfelIndices), gpuData.brickCount,
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_brick_info");
    if (gpuData.brickInfoBuf)
      gpuData.brickInfoBuf->updateData(0, sizeof(SurfelIndices) * gpuData.brickCount, sector_data.brickInfo.data(), VBLOCK_WRITEONLY);

    gpuData.brickRadianceBuf = d3d::create_sbuffer(sizeof(Point4), gpuData.brickCount,
      SBCF_BIND_UNORDERED | SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_brick_radiance");
  }

  if (gpuData.surfelCount > 0)
  {
    gpuData.surfelsBuf = d3d::create_sbuffer(sizeof(Surfel), gpuData.surfelCount,
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_surfels");
    if (gpuData.surfelsBuf)
      gpuData.surfelsBuf->updateData(0, sizeof(Surfel) * gpuData.surfelCount, sector_data.surfels.data(), VBLOCK_WRITEONLY);
  }

  size_t sectorBytes = (gpuData.probeCount * sizeof(PRTProbeData)) +
                       (gpuData.probeCount * 9 * sizeof(Point4)) +
                       (gpuData.factorCount * sizeof(BrickFactor)) +
                       (gpuData.brickCount * sizeof(SurfelIndices)) +
                       (gpuData.brickCount * sizeof(Point4)) +
                       (gpuData.surfelCount * sizeof(Surfel));
  totalVramBytes += sectorBytes;

  auto ins = residentSectors.insert(eastl::make_pair(sector_data.sectorIndex, gpuData));
  return &ins.first->second;
}

} // namespace daprtgi

