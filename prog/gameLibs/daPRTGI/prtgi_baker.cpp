//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_baker.h"
#include <gamePhys/collision/collisionLib.h>
#include <gameRes/dag_collisionResource.h>
#include <math/dag_math3d.h>
#include <math/dag_mathUtils.h>
#include <math/dag_TMatrix.h>
#include <perfMon/dag_statDrv.h>
#include <perfMon/dag_daProfiler.h>
#include <util/dag_watchdog.h>
#include <osApiWrappers/dag_direct.h>
#include <debug/dag_debug.h>
#include <EASTL/hash_map.h>
#include <stdio.h>

namespace daprtgi
{

// SH9 Basis evaluation
static void evaluateSH9Basis(const Point3 &d, float sh[9])
{
  const float c0 = 0.28209479177387814f; // 0.5 * sqrt(1/pi)
  const float c1 = 0.4886025119029199f;  // 0.5 * sqrt(3/pi)
  const float c2 = 1.0925484305920792f;  // 0.5 * sqrt(15/pi)
  const float c3 = 0.31539156525252005f; // 0.25 * sqrt(5/pi)
  const float c4 = 0.5462742152960396f;  // 0.25 * sqrt(15/pi)

  sh[0] = c0;
  sh[1] = -c1 * d.y;
  sh[2] = c1 * d.z;
  sh[3] = -c1 * d.x;
  sh[4] = c2 * d.x * d.y;
  sh[5] = -c2 * d.y * d.z;
  sh[6] = c3 * (3.0f * d.z * d.z - 1.0f);
  sh[7] = -c2 * d.x * d.z;
  sh[8] = c4 * (d.x * d.x - d.y * d.y);
}

static uint16_t floatToHalf(float val)
{
  uint32_t f;
  memcpy(&f, &val, 4);
  uint32_t sign = (f >> 16) & 0x8000;
  int32_t exp = ((f >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = f & 0x7FFFFF;

  if (exp <= 0)
    return (uint16_t)sign;
  else if (exp >= 31)
    return (uint16_t)(sign | 0x7C00);

  return (uint16_t)(sign | (exp << 10) | (mantissa >> 13));
}

void PRTBaker::generateSampleDirections(eastl::vector<Point3> &out_dirs, eastl::vector<float> &out_weights, int count)
{
  out_dirs.resize(count);
  out_weights.resize(count);

  const float phi = (1.0f + sqrtf(5.0f)) * 0.5f; // Golden ratio
  float weight = 4.0f * (float)M_PI / (float)count;

  for (int i = 0; i < count; ++i)
  {
    float theta = 2.0f * (float)M_PI * (float)i / phi;
    float y = 1.0f - (2.0f * (float)i + 1.0f) / (float)count;
    float radius = sqrtf(max(0.0f, 1.0f - y * y));

    out_dirs[i] = Point3(radius * cosf(theta), y, radius * sinf(theta));
    out_weights[i] = weight;
  }
}

static BBox3 get_collres_bbox(const CollisionResource *collRes)
{
  BBox3 box;
  if (!collRes)
    return box;
  v_stu_bbox3(box, collRes->getDefaultInstance().getRootBBox());
  return box;
}

uint64_t PRTBaker::computeCurrentSceneGeometryHash()
{
  CollisionResource *collRes = dacoll::get_static_collision_resource();
  if (!collRes)
    return 1337;

  uint32_t triCount = collRes->getTrianglesCount(CollisionNode::TRACEABLE);
  BBox3 box = get_collres_bbox(collRes);
  uint64_t hash = 14695981039346656037ULL; // FNV-1a
  hash ^= triCount;
  hash *= 1099511628211ULL;
  
  uint32_t boxBitsX = *(uint32_t*)&box.lim[0].x;
  uint32_t boxBitsY = *(uint32_t*)&box.lim[0].y;
  uint32_t boxBitsZ = *(uint32_t*)&box.lim[0].z;
  hash ^= (uint64_t)boxBitsX | ((uint64_t)boxBitsY << 32);
  hash *= 1099511628211ULL;
  hash ^= (uint64_t)boxBitsZ;
  hash *= 1099511628211ULL;

  return hash;
}

bool PRTBaker::bakeCurrentScene(PRTAsset &out_asset, float spacing, int sector_size,
                               ProgressCallback progress_cb, const char *output_dir)
{
  debug("PRTGI: Starting scene bake (spacing=%.2f, sectorSize=%d)...", spacing, sector_size);
  if (progress_cb)
    progress_cb("Gathering scene geometry...", 0.05f);

  int64_t bakeStartTicks = profile_ref_ticks();

  // 1. Suspend watchdog trigger during bake to avoid false-positive "Freeze detected"
  intptr_t prevWatchdogTmt = watchdog_set_option(WATCHDOG_OPTION_TRIG_THRESHOLD, WATCHDOG_DISABLE);
  watchdog_kick();

  // 2. Temporarily mask out TAGS in da_profiler so traceray_normalized doesn't exhaust ChunkedStorage memory
  int savedProfilerMode = ::da_profiler::active_mode;
  ::da_profiler::active_mode &= ~::da_profiler::TAGS;

  CollisionResource *collRes = dacoll::get_static_collision_resource();
  BBox3 sceneBounds;
  if (collRes)
    sceneBounds = get_collres_bbox(collRes);
  else
    sceneBounds = BBox3(Point3(-100, -20, -100), Point3(100, 50, 100));

  // Snap bounds to spacing
  Point3 minPt = Point3(floorf(sceneBounds.lim[0].x / spacing) * spacing,
                        floorf(sceneBounds.lim[0].y / spacing) * spacing,
                        floorf(sceneBounds.lim[0].z / spacing) * spacing);
  Point3 maxPt = Point3(ceilf(sceneBounds.lim[1].x / spacing) * spacing,
                        ceilf(sceneBounds.lim[1].y / spacing) * spacing,
                        ceilf(sceneBounds.lim[1].z / spacing) * spacing);

  IPoint3 gridCount = IPoint3(
    max(1, (int)ceilf((maxPt.x - minPt.x) / spacing) + 1),
    max(1, (int)ceilf((maxPt.y - minPt.y) / spacing) + 1),
    max(1, (int)ceilf((maxPt.z - minPt.z) / spacing) + 1)
  );

  out_asset.clear();
  out_asset.grid.origin = minPt;
  out_asset.grid.minCoord = IPoint3(0, 0, 0);
  out_asset.grid.count = gridCount;
  out_asset.grid.spacing = spacing;
  out_asset.sectorSizeInProbes = sector_size;
  out_asset.geometryHash = computeCurrentSceneGeometryHash();

  int sectorCntX = max(1, (gridCount.x + sector_size - 1) / sector_size);
  int sectorCntZ = max(1, (gridCount.z + sector_size - 1) / sector_size);
  out_asset.sectorGridCount = IPoint3(sectorCntX, 1, sectorCntZ);

  int totalSectors = sectorCntX * sectorCntZ;
  out_asset.sectors.resize(totalSectors);
  for (int sz = 0; sz < sectorCntZ; ++sz)
  {
    for (int sx = 0; sx < sectorCntX; ++sx)
    {
      int secIdx = sz * sectorCntX + sx;
      out_asset.sectors[secIdx].sectorIndex = secIdx;
      out_asset.sectors[secIdx].sectorCoord = IPoint3(sx, 0, sz);
      
      Point3 secMin = minPt + Point3((float)(sx * sector_size) * spacing, 0.0f, (float)(sz * sector_size) * spacing);
      Point3 secMax = secMin + Point3((float)sector_size * spacing, (float)gridCount.y * spacing, (float)sector_size * spacing);
      out_asset.sectors[secIdx].bounds = BBox3(secMin, secMax);
      out_asset.sectors[secIdx].surfelBoundsMin = secMin;
      out_asset.sectors[secIdx].surfelBoundsSize = max(Point3(1, 1, 1), secMax - secMin);
    }
  }

  uint32_t totalProbes = out_asset.grid.totalProbes();
  out_asset.probeToSectorMap.resize(totalProbes);

  eastl::vector<Point3> sampleDirs;
  eastl::vector<float> sampleWeights;
  generateSampleDirections(sampleDirs, sampleWeights, 32);

  if (progress_cb)
    progress_cb("Tracing probes (with intelligent pruning)...", 0.15f);

  uint32_t activeTracedProbes = 0;
  uint32_t culledSkyProbes = 0;
  uint32_t culledSolidProbes = 0;
  uint32_t totalSurfels = 0;
  uint32_t totalBricks = 0;
  uint32_t totalFactors = 0;

  // Trace rays for each probe
  for (uint32_t pIdx = 0; pIdx < totalProbes; ++pIdx)
  {
    if ((pIdx % 200) == 0)
    {
      watchdog_kick();
      if (progress_cb && (pIdx % 2000) == 0)
      {
        float prog = 0.15f + 0.80f * ((float)pIdx / (float)totalProbes);
        char progMsg[128];
        snprintf(progMsg, sizeof(progMsg), "Baking probes (%u/%u, active: %u, culled: %u)...",
          pIdx, totalProbes, activeTracedProbes, culledSkyProbes + culledSolidProbes);
        progress_cb(progMsg, prog);
      }
    }

    IPoint3 coord = out_asset.grid.getProbeCoord(pIdx);
    int secX = clamp(coord.x / sector_size, 0, sectorCntX - 1);
    int secZ = clamp(coord.z / sector_size, 0, sectorCntZ - 1);
    int secIdx = secZ * sectorCntX + secX;
    out_asset.probeToSectorMap[pIdx] = secIdx;

    PRTSectorData &sector = out_asset.sectors[secIdx];
    Point3 probePos = out_asset.grid.getProbeWorldPos(coord);

    PRTProbeData pData;
    memset(&pData, 0, sizeof(pData));
    pData.factorStart = (int32_t)sector.factors.size();
    pData.factorCount = 0;
    pData.captureOffset = Point3(0, 0, 0);
    pData.validity = 0xFFFFFFFF; // fully valid

    // --- Fast Probe Classification & Pruning ---
    real downDist = 20.0f;
    int floorPmid = -1;
    Point3 floorNorm(0, 1, 0);
    bool hitDown = dacoll::traceray_normalized(probePos, Point3(0, -1, 0), downDist, &floorPmid, &floorNorm);

    real upDist = 15.0f;
    int ceilPmid = -1;
    Point3 ceilNorm(0, -1, 0);
    bool hitUp = dacoll::traceray_normalized(probePos, Point3(0, 1, 0), upDist, &ceilPmid, &ceilNorm);

    // Case 1: Underground or buried inside solid geometry
    if ((hitDown && downDist < 0.20f && floorNorm.y <= 0.0f) || (hitUp && upDist < 0.20f && ceilNorm.y >= 0.0f))
    {
      pData.validity = 0;
      sector.probeIds.push_back(pIdx);
      sector.probeData.push_back(pData);
      sector.skyVisibility.push_back(0);
      culledSolidProbes++;
      continue;
    }

    // Case 2: High in empty sky (no floor within 20m, no ceiling within 15m, and no nearby walls)
    if (!hitDown && !hitUp)
    {
      real horizDist = 12.0f;
      bool nearMesh = dacoll::traceray_normalized(probePos, Point3(1, 0, 0), horizDist, nullptr, nullptr) ||
                      dacoll::traceray_normalized(probePos, Point3(-1, 0, 0), horizDist, nullptr, nullptr) ||
                      dacoll::traceray_normalized(probePos, Point3(0, 0, 1), horizDist, nullptr, nullptr) ||
                      dacoll::traceray_normalized(probePos, Point3(0, 0, -1), horizDist, nullptr, nullptr);
      if (!nearMesh)
      {
        pData.validity = 0xFFFFFFFF;
        pData.factorCount = 0;
        sector.probeIds.push_back(pIdx);
        sector.probeData.push_back(pData);
        sector.skyVisibility.push_back(0xFFFFFFFF);
        culledSkyProbes++;
        continue;
      }
    }

    // Case 3: Active gameplay probe near surfaces -> Trace 32 rays
    activeTracedProbes++;
    uint32_t skyVisWord = 0;

    for (int s = 0; s < 32; ++s)
    {
      Point3 dir = sampleDirs[s];
      real t = 100.0f; // max trace dist
      int pmid = -1;
      Point3 norm(0, 1, 0);

      bool hit = dacoll::traceray_normalized(probePos, dir, t, &pmid, &norm);
      if (hit && t > 0.05f)
      {
        Point3 hitPos = probePos + dir * t;
        
        // Add surfel
        Surfel surfel;
        Point3 d = hitPos - sector.surfelBoundsMin;
        Point3 q = Point3(d.x / sector.surfelBoundsSize.x, d.y / sector.surfelBoundsSize.y, d.z / sector.surfelBoundsSize.z);
        q = Point3(clamp(q.x, 0.0f, 1.0f), clamp(q.y, 0.0f, 1.0f), clamp(q.z, 0.0f, 1.0f));
        uint16_t qx = (uint16_t)(q.x * 65535.0f);
        uint16_t qy = (uint16_t)(q.y * 65535.0f);
        uint16_t qz = (uint16_t)(q.z * 65535.0f);
        surfel.positionXY = (uint32_t)qx | ((uint32_t)qy << 16);
        surfel.positionZAlbedoRG = (uint32_t)qz | (200 << 16) | (200 << 24); // default 0.8 albedo
        surfel.normalAlbedoB = (2048) | (2048 << 12) | (200 << 24);
        surfel.nearestProbe = (int32_t)pIdx;

        uint32_t surfelIdx = (uint32_t)sector.surfels.size();
        sector.surfels.push_back(surfel);
        totalSurfels++;

        // Group 1:1 or small clusters into brick
        SurfelIndices brick;
        brick.surfelStart = surfelIdx;
        brick.surfelCount = 1;
        brick.renderingLayerMask = 1;
        brick.objectLayerMask = 1;
        uint32_t brickIdx = (uint32_t)sector.brickInfo.size();
        sector.brickInfo.push_back(brick);
        totalBricks++;

        // Evaluate SH9 for the sample direction
        float sh[9];
        evaluateSH9Basis(dir, sh);
        float w = sampleWeights[s];

        BrickFactor factor;
        memset(&factor, 0, sizeof(factor));
        factor.brickSh0 = (brickIdx & 0xFFFFu) | ((uint32_t)floatToHalf(sh[0] * w) << 16);
        factor.sh12 = (uint32_t)floatToHalf(sh[1] * w) | ((uint32_t)floatToHalf(sh[2] * w) << 16);
        factor.sh34 = (uint32_t)floatToHalf(sh[3] * w) | ((uint32_t)floatToHalf(sh[4] * w) << 16);
        factor.sh56 = (uint32_t)floatToHalf(sh[5] * w) | ((uint32_t)floatToHalf(sh[6] * w) << 16);
        factor.sh78 = (uint32_t)floatToHalf(sh[7] * w) | ((uint32_t)floatToHalf(sh[8] * w) << 16);

        sector.factors.push_back(factor);
        pData.factorCount++;
        totalFactors++;
      }
      else
      {
        skyVisWord |= (1u << s);
      }
    }

    sector.probeIds.push_back(pIdx);
    sector.probeData.push_back(pData);
    sector.skyVisibility.push_back(skyVisWord);
  }

  // Restore profiler and watchdog state
  ::da_profiler::active_mode = savedProfilerMode;
  watchdog_set_option(WATCHDOG_OPTION_TRIG_THRESHOLD, prevWatchdogTmt);
  watchdog_kick();

  float elapsedSec = profile_time_usec(bakeStartTicks) / 1000000.0f;

  if (progress_cb)
    progress_cb("Bake complete", 1.0f);

  // --- Output Detailed Baking Log File ---
  const char *targetDir = (output_dir && output_dir[0]) ? output_dir : "prtgi";
  dd_mkdir(targetDir);

  eastl::string logPath;
  logPath.sprintf("%s/prtgi_bake.log", targetDir);

  FILE *logFile = fopen(logPath.c_str(), "w");
  if (!logFile)
    logFile = fopen("prtgi/prtgi_bake.log", "w");
  if (!logFile)
    logFile = fopen("prtgi_bake.log", "w");

  if (logFile)
  {
    fprintf(logFile, "=========================================================\n");
    fprintf(logFile, "  Dagor Engine 6.5 - daPRTGI Probe Baking Log\n");
    fprintf(logFile, "=========================================================\n");
    fprintf(logFile, "Grid Origin:          (%.2f, %.2f, %.2f)\n", minPt.x, minPt.y, minPt.z);
    fprintf(logFile, "Grid Dimensions:      (%.2f, %.2f, %.2f) meters\n", maxPt.x - minPt.x, maxPt.y - minPt.y, maxPt.z - minPt.z);
    fprintf(logFile, "Grid Probe Spacing:   %.2f m\n", spacing);
    fprintf(logFile, "Grid Probe Counts:    (%d, %d, %d)\n", gridCount.x, gridCount.y, gridCount.z);
    fprintf(logFile, "Total Grid Probes:    %u\n", totalProbes);
    fprintf(logFile, "Sector Grid Count:    (%d, %d) -> Total Sectors: %d (Sector Size: %d)\n",
      sectorCntX, sectorCntZ, totalSectors, sector_size);
    fprintf(logFile, "Scene Geometry Hash:  0x%llX\n", (unsigned long long)out_asset.geometryHash);
    fprintf(logFile, "---------------------------------------------------------\n");
    fprintf(logFile, "[Classification & Pruning Results]\n");
    fprintf(logFile, "  - Active Surface Probes:  %u (%.1f%% of grid)\n",
      activeTracedProbes, (float)activeTracedProbes * 100.0f / (totalProbes ? totalProbes : 1));
    fprintf(logFile, "  - Culled Empty Sky Probes:%u (%.1f%% of grid)\n",
      culledSkyProbes, (float)culledSkyProbes * 100.0f / (totalProbes ? totalProbes : 1));
    fprintf(logFile, "  - Culled Solid Probes:    %u (%.1f%% of grid)\n",
      culledSolidProbes, (float)culledSolidProbes * 100.0f / (totalProbes ? totalProbes : 1));
    fprintf(logFile, "---------------------------------------------------------\n");
    fprintf(logFile, "[Radiance Transfer & Form Factors]\n");
    fprintf(logFile, "  - Total Surfels:          %u\n", totalSurfels);
    fprintf(logFile, "  - Total Bricks:           %u\n", totalBricks);
    fprintf(logFile, "  - Total Form Factors:     %u\n", totalFactors);
    fprintf(logFile, "  - Total Baking Time:      %.2f seconds\n", elapsedSec);
    fprintf(logFile, "  - Baking Status:          SUCCESSFUL\n");
    fprintf(logFile, "=========================================================\n");
    fclose(logFile);
  }

  debug("PRTGI: Bake finished in %.2f sec. Generated %u sectors, %u probes (Active: %u, Sky: %u, Solid: %u). Log: prtgi_bake.log",
    elapsedSec, totalSectors, totalProbes, activeTracedProbes, culledSkyProbes, culledSolidProbes);

  return true;
}

} // namespace daprtgi

