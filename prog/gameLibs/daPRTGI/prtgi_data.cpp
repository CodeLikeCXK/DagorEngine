//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_data.h"
#include <debug/dag_debug.h>

namespace daprtgi
{

void PRTAsset::clear()
{
  grid = PRTGridDesc();
  sectorGridCount = IPoint3(0, 0, 0);
  sectors.clear();
  probeToSectorMap.clear();
}

bool PRTAsset::serialize(IGenSave &writer) const
{
  uint32_t magic = ASSET_MAGIC;
  uint32_t version = ASSET_VERSION;
  writer.write(&magic, sizeof(magic));
  writer.write(&version, sizeof(version));
  writer.write(&geometryHash, sizeof(geometryHash));

  // Write Grid
  writer.write(&grid.origin, sizeof(grid.origin));
  writer.write(&grid.minCoord, sizeof(grid.minCoord));
  writer.write(&grid.count, sizeof(grid.count));
  writer.write(&grid.spacing, sizeof(grid.spacing));

  writer.write(&sectorGridCount, sizeof(sectorGridCount));
  writer.write(&sectorSizeInProbes, sizeof(sectorSizeInProbes));

  // Write probeToSectorMap
  uint32_t mapSize = (uint32_t)probeToSectorMap.size();
  writer.write(&mapSize, sizeof(mapSize));
  if (mapSize > 0)
    writer.write(probeToSectorMap.data(), mapSize * sizeof(int));

  // Write Sectors
  uint32_t sectorCount = (uint32_t)sectors.size();
  writer.write(&sectorCount, sizeof(sectorCount));

  for (const auto &sec : sectors)
  {
    writer.write(&sec.sectorIndex, sizeof(sec.sectorIndex));
    writer.write(&sec.sectorCoord, sizeof(sec.sectorCoord));
    writer.write(&sec.bounds, sizeof(sec.bounds));
    writer.write(&sec.surfelBoundsMin, sizeof(sec.surfelBoundsMin));
    writer.write(&sec.surfelBoundsSize, sizeof(sec.surfelBoundsSize));

    uint32_t probeCnt = (uint32_t)sec.probeIds.size();
    writer.write(&probeCnt, sizeof(probeCnt));
    if (probeCnt > 0)
    {
      writer.write(sec.probeIds.data(), probeCnt * sizeof(uint32_t));
      writer.write(sec.probeData.data(), probeCnt * sizeof(PRTProbeData));
    }

    uint32_t factorCnt = (uint32_t)sec.factors.size();
    writer.write(&factorCnt, sizeof(factorCnt));
    if (factorCnt > 0)
      writer.write(sec.factors.data(), factorCnt * sizeof(BrickFactor));

    uint32_t skyVisCnt = (uint32_t)sec.skyVisibility.size();
    writer.write(&skyVisCnt, sizeof(skyVisCnt));
    if (skyVisCnt > 0)
      writer.write(sec.skyVisibility.data(), skyVisCnt * sizeof(uint64_t));

    uint32_t depthCnt = (uint32_t)sec.probeDepth.size();
    writer.write(&depthCnt, sizeof(depthCnt));
    if (depthCnt > 0)
      writer.write(sec.probeDepth.data(), depthCnt * sizeof(uint32_t));

    uint32_t brickCnt = (uint32_t)sec.brickInfo.size();
    writer.write(&brickCnt, sizeof(brickCnt));
    if (brickCnt > 0)
      writer.write(sec.brickInfo.data(), brickCnt * sizeof(SurfelIndices));

    uint32_t surfelCnt = (uint32_t)sec.surfels.size();
    writer.write(&surfelCnt, sizeof(surfelCnt));
    if (surfelCnt > 0)
      writer.write(sec.surfels.data(), surfelCnt * sizeof(Surfel));
  }

  debug("PRTGI: Serialized %u sectors, %u probes to asset", sectorCount, grid.totalProbes());
  return true;
}

bool PRTAsset::deserialize(IGenLoad &reader)
{
  clear();

  uint32_t magic = 0;
  uint32_t version = 0;
  if (reader.tryRead(&magic, sizeof(magic)) != (int)sizeof(magic) || magic != ASSET_MAGIC)
  {
    logwarn("PRTGI: Invalid asset magic: 0x%X", magic);
    return false;
  }
  if (reader.tryRead(&version, sizeof(version)) != (int)sizeof(version) || version != ASSET_VERSION)
  {
    logwarn("PRTGI: Incompatible asset version: %u", version);
    return false;
  }
  reader.read(&geometryHash, sizeof(geometryHash));

  reader.read(&grid.origin, sizeof(grid.origin));
  reader.read(&grid.minCoord, sizeof(grid.minCoord));
  reader.read(&grid.count, sizeof(grid.count));
  reader.read(&grid.spacing, sizeof(grid.spacing));

  reader.read(&sectorGridCount, sizeof(sectorGridCount));
  reader.read(&sectorSizeInProbes, sizeof(sectorSizeInProbes));

  uint32_t mapSize = 0;
  reader.read(&mapSize, sizeof(mapSize));
  probeToSectorMap.resize(mapSize);
  if (mapSize > 0)
    reader.read(probeToSectorMap.data(), mapSize * sizeof(int));

  uint32_t sectorCount = 0;
  reader.read(&sectorCount, sizeof(sectorCount));
  sectors.resize(sectorCount);

  for (uint32_t i = 0; i < sectorCount; ++i)
  {
    auto &sec = sectors[i];
    reader.read(&sec.sectorIndex, sizeof(sec.sectorIndex));
    reader.read(&sec.sectorCoord, sizeof(sec.sectorCoord));
    reader.read(&sec.bounds, sizeof(sec.bounds));
    reader.read(&sec.surfelBoundsMin, sizeof(sec.surfelBoundsMin));
    reader.read(&sec.surfelBoundsSize, sizeof(sec.surfelBoundsSize));

    uint32_t probeCnt = 0;
    reader.read(&probeCnt, sizeof(probeCnt));
    sec.probeIds.resize(probeCnt);
    sec.probeData.resize(probeCnt);
    if (probeCnt > 0)
    {
      reader.read(sec.probeIds.data(), probeCnt * sizeof(uint32_t));
      reader.read(sec.probeData.data(), probeCnt * sizeof(PRTProbeData));
      for (uint32_t i = 0; i < probeCnt; ++i)
        sec.probeData[i].globalProbeIndex = sec.probeIds[i];
    }

    uint32_t factorCnt = 0;
    reader.read(&factorCnt, sizeof(factorCnt));
    sec.factors.resize(factorCnt);
    if (factorCnt > 0)
      reader.read(sec.factors.data(), factorCnt * sizeof(BrickFactor));

    uint32_t skyVisCnt = 0;
    reader.read(&skyVisCnt, sizeof(skyVisCnt));
    sec.skyVisibility.resize(skyVisCnt);
    if (skyVisCnt > 0)
      reader.read(sec.skyVisibility.data(), skyVisCnt * sizeof(uint64_t));

    uint32_t depthCnt = 0;
    reader.read(&depthCnt, sizeof(depthCnt));
    sec.probeDepth.resize(depthCnt);
    if (depthCnt > 0)
      reader.read(sec.probeDepth.data(), depthCnt * sizeof(uint32_t));

    uint32_t brickCnt = 0;
    reader.read(&brickCnt, sizeof(brickCnt));
    sec.brickInfo.resize(brickCnt);
    if (brickCnt > 0)
      reader.read(sec.brickInfo.data(), brickCnt * sizeof(SurfelIndices));

    uint32_t surfelCnt = 0;
    reader.read(&surfelCnt, sizeof(surfelCnt));
    sec.surfels.resize(surfelCnt);
    if (surfelCnt > 0)
      reader.read(sec.surfels.data(), surfelCnt * sizeof(Surfel));
  }

  debug("PRTGI: Loaded asset with %u sectors, %u probes", sectorCount, grid.totalProbes());
  return true;
}

} // namespace daprtgi

