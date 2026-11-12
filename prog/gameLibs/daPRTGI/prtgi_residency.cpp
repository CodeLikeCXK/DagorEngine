//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_residency.h"
#include <drv/3d/dag_driver.h>
#include <debug/dag_debug.h>
#include <math.h>

namespace daprtgi
{

static int prt_wrap_coord(int coordinate, int count)
{
  if (count <= 0)
    return 0;
  // Match prt_wrap(): coordinate - count * floor(coordinate / count).
  return coordinate - count * (int)floorf((float)coordinate / (float)count);
}

void SectorGPUData::release()
{
  // destroy_d3dres only calls destroy(), the pointer stays dangling. del_d3dres also nulls it,
  // so a second release() (close + destructor) cannot destroy an already freed resource again.
  del_d3dres(probeDataBuf);
  del_d3dres(factorsBuf);
  del_d3dres(skyVisBuf);
  del_d3dres(brickInfoBuf);
  del_d3dres(surfelsBuf);
  del_d3dres(brickRadianceBuf);
  del_d3dres(probeSHBuf);
  sectorIndex = -1;
  probeCount = 0;
  factorCount = 0;
  brickCount = 0;
  surfelCount = 0;
}

void PRTSectorResidency::init(const PRTGridDesc &grid, int cascade_count, bool use_depth_occlusion)
{
  release();
  gridDesc = grid;
  cascadeCount = cascade_count;
  depthOcclusion = use_depth_occlusion;

  // Ensure slotCount completely encompasses all horizontal and vertical probe layers
  // in the scene grid without spatial aliasing or toroidal overwriting between indoor and outdoor probes
  int slotX = max(32, gridDesc.count.x);
  int slotY = max(32, gridDesc.count.y + 4);
  int slotZ = max(32, gridDesc.count.z);
  slotCount = IPoint3(slotX, slotY, slotZ);

  // Only L0+L1 are published and only one cascade is used, so the volume is ~7x smaller than the
  // old 9-band x cascadeCount layout. Bands are packed adjacently along X for cache locality.
  int volW = slotCount.x * PRT_COEFF_SLICES;
  int volH = slotCount.z;
  int volD = slotCount.y;

  coefficientVoxel3D = dag::create_voltex(volW, volH, volD,
    TEXCF_UNORDERED | TEXFMT_A16B16G16R16F | TEXCF_CLEAR_ON_CREATE, 1, "prt_coefficient_voxel_3d");

  if (coefficientVoxel3D)
  {
    coefficientVoxel3DTexId = coefficientVoxel3D.getTexId();
    debug("PRTGI: Created 3D coefficient voxel texture (%dx%dx%d)", volW, volH, volD);
  }
  else
  {
    logerr("PRTGI: Failed to create 3D coefficient voxel texture!");
  }

  const uint32_t maskCount = (uint32_t)slotCount.x * (uint32_t)slotCount.y * (uint32_t)slotCount.z;
  // One interleaved per-probe record: { skyCubeLo, skyCubeHi, validityAndOffset, 0 }.
  // The sampler used to issue two independent random reads per probe corner; now one.
  probeInfoBuf = d3d::create_sbuffer(sizeof(Point4), maskCount,
    SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_probe_info");
  if (probeInfoBuf)
  {
    eastl::vector<Point4> zeros(maskCount, Point4(0, 0, 0, 0));
    probeInfoBuf->updateData(0, sizeof(Point4) * maskCount, zeros.data(), VBLOCK_WRITEONLY);
    totalVramBytes += sizeof(Point4) * maskCount;
    debug("PRTGI: Created probe info buffer (%u slots)", maskCount);
  }
  else
  {
    logerr("PRTGI: Failed to create probe info buffer!");
  }

  // Both tiers need per-point occlusion: an ambient cube alone cannot reject the probes on the
  // far side of a thin ceiling, which is exactly where the interior leak showed up. Low uses the
  // same buffer with a single-tap lookup, so it stays cheap.
  if (depthOcclusion)
  {
    const uint32_t depthCount = maskCount * (uint32_t)PRT_DEPTH_TEXELS;
    probeDepthBuf = d3d::create_sbuffer(sizeof(uint32_t), depthCount,
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_probe_depth");
    if (probeDepthBuf)
    {
      eastl::vector<uint32_t> ones(depthCount, 0xFFFFFFFFu);
      probeDepthBuf->updateData(0, sizeof(uint32_t) * depthCount, ones.data(), VBLOCK_WRITEONLY);
      totalVramBytes += sizeof(uint32_t) * depthCount;
      debug("PRTGI: Created probe depth buffer (%u entries)", depthCount);
    }
    else
    {
      logerr("PRTGI: Failed to create probe depth buffer!");
    }
  }
}

void PRTSectorResidency::release()
{
  for (auto &it : residentSectors)
    it.second.release();
  residentSectors.clear();

  coefficientVoxel3D.close();
  coefficientVoxel3DTexId = BAD_TEXTUREID;
  del_d3dres(probeInfoBuf);
  del_d3dres(probeDepthBuf);
  totalVramBytes = 0;
}

void PRTSectorResidency::publishSkyMasks(const PRTAsset &asset)
{
  if (!probeInfoBuf)
    return;

  const uint32_t maskCount = (uint32_t)slotCount.x * (uint32_t)slotCount.y * (uint32_t)slotCount.z;
  if (maskCount == 0)
    return;

  // 4 uints per probe: { skyCubeLo, skyCubeHi, validityAndOffset, 0 }
  eastl::vector<uint32_t> info((size_t)maskCount * 4, 0);
  // Only allocate the staging copy when the depth buffer actually exists (High tier).
  eastl::vector<uint32_t> depths;
  if (probeDepthBuf)
    depths.assign((size_t)maskCount * PRT_DEPTH_TEXELS, 0xFFFFFFFFu);
  uint32_t written = 0;

  for (const PRTSectorData &sector : asset.sectors)
  {
    const uint32_t probeCount = (uint32_t)sector.probeIds.size();
    const uint32_t visCount = (uint32_t)sector.skyVisibility.size();
    const uint32_t dataCount = (uint32_t)sector.probeData.size();
    const uint32_t depthCount = (uint32_t)sector.probeDepth.size();
    for (uint32_t i = 0; i < probeCount; ++i)
    {
      const IPoint3 coord = asset.grid.getProbeCoord(sector.probeIds[i]);
      const int sx = prt_wrap_coord(coord.x, slotCount.x);
      const int sy = coord.y;
      const int sz = prt_wrap_coord(coord.z, slotCount.z);
      if (sy < 0 || sy >= slotCount.y)
        continue;

      uint64_t mask = (i < visCount) ? sector.skyVisibility[i] : 0ull;
      const uint32_t valBits = (i < dataCount) ? sector.probeData[i].validity : 0u;
      if ((valBits & PRT_PROBE_VALID_BIT) == 0u)
        mask = 0ull;

      const uint32_t index = (uint32_t)sx + (uint32_t)slotCount.x * ((uint32_t)sz + (uint32_t)slotCount.z * (uint32_t)sy);
      if (index < maskCount)
      {
        uint32_t *rec = &info[(size_t)index * 4];
        rec[0] = (uint32_t)(mask & 0xFFFFFFFFull);
        rec[1] = (uint32_t)(mask >> 32);
        rec[2] = valBits;
        if (!depths.empty() && (size_t)(i + 1) * PRT_DEPTH_TEXELS <= depthCount)
        {
          memcpy(&depths[(size_t)index * PRT_DEPTH_TEXELS],
            &sector.probeDepth[(size_t)i * PRT_DEPTH_TEXELS],
            sizeof(uint32_t) * PRT_DEPTH_TEXELS);
        }
        ++written;
      }
    }
  }

  probeInfoBuf->updateData(0, sizeof(uint32_t) * info.size(), info.data(), VBLOCK_WRITEONLY);
  if (probeDepthBuf)
    probeDepthBuf->updateData(0, sizeof(uint32_t) * depths.size(), depths.data(), VBLOCK_WRITEONLY);
  debug("PRTGI: Published %u sky masks & depth maps into %u slots", written, maskCount);
}

void PRTSectorResidency::afterReset()
{
  // Recreate GPU 3D texture after graphics device reset
  init(gridDesc, cascadeCount, depthOcclusion);
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
  {
    it->second.lastUseTick = ++useTickCounter;
    return &it->second;
  }

  // Nothing ever evicted sectors before, so all 16 stayed resident (~600 MB of GPU buffers at
  // this scene size). Keep only a small working set; the published coefficients live in the
  // volume texture, so the per-sector inputs can be re-uploaded on the rare re-visit.
  while (!residentSectors.empty() && (int)residentSectors.size() >= residentLimit)
  {
    auto lru = residentSectors.begin();
    for (auto i = residentSectors.begin(); i != residentSectors.end(); ++i)
      if (i->second.lastUseTick < lru->second.lastUseTick)
        lru = i;
    totalVramBytes -= lru->second.vramBytes;
    lru->second.release();
    residentSectors.erase(lru);
  }

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

    gpuData.probeSHBuf = d3d::create_sbuffer(sizeof(Point4), gpuData.probeCount * PRT_COEFF_SLICES,
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
    gpuData.skyVisBuf = d3d::create_sbuffer(sizeof(uint64_t), (uint32_t)sector_data.skyVisibility.size(),
      SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED, 0, "prt_sky_vis");
    if (gpuData.skyVisBuf)
      gpuData.skyVisBuf->updateData(0, sizeof(uint64_t) * sector_data.skyVisibility.size(), sector_data.skyVisibility.data(), VBLOCK_WRITEONLY);
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
                       (gpuData.probeCount * PRT_COEFF_SLICES * sizeof(Point4)) +
                       (gpuData.factorCount * sizeof(BrickFactor)) +
                       (gpuData.brickCount * sizeof(SurfelIndices)) +
                       (gpuData.brickCount * sizeof(Point4)) +
                       (gpuData.surfelCount * sizeof(Surfel));
  gpuData.vramBytes = sectorBytes;
  totalVramBytes += sectorBytes;

  gpuData.lastUseTick = ++useTickCounter;
  auto ins = residentSectors.insert(eastl::make_pair(sector_data.sectorIndex, gpuData));
  return &ins.first->second;
}

} // namespace daprtgi
