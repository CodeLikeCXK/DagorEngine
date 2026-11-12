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
  sh[1] = c1 * d.y;
  sh[2] = c1 * d.z;
  sh[3] = c1 * d.x;
  sh[4] = c2 * d.x * d.y;
  sh[5] = c2 * d.y * d.z;
  sh[6] = c3 * (3.0f * d.z * d.z - 1.0f);
  sh[7] = c2 * d.x * d.z;
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

static inline uint32_t encode_octahedral_normal(Point3 n)
{
  float invL1 = 1.0f / (fabsf(n.x) + fabsf(n.y) + fabsf(n.z) + 1e-6f);
  float nx = n.x * invL1;
  float ny = n.y * invL1;
  if (n.z < 0.0f)
  {
    float ox = (1.0f - fabsf(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
    float oy = (1.0f - fabsf(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
    nx = ox;
    ny = oy;
  }
  int ex = clamp((int)((nx * 0.5f + 0.5f) * 4095.0f + 0.5f), 0, 4095);
  int ey = clamp((int)((ny * 0.5f + 0.5f) * 4095.0f + 0.5f), 0, 4095);
  return (uint32_t)ex | ((uint32_t)ey << 12);
}

// Must match prt_oct_encode() in prtgi_common.hlsli.
static inline Point3 oct_decode(float u, float v)
{
  float ex = u * 2.0f - 1.0f;
  float ey = v * 2.0f - 1.0f;
  Point3 n(ex, ey, 1.0f - fabsf(ex) - fabsf(ey));
  float t = max(0.0f, -n.z);
  n.x += (n.x >= 0.0f) ? -t : t;
  n.y += (n.y >= 0.0f) ? -t : t;
  return normalize(n);
}

// Pack 6-axis Ambient Cube sky visibility (+X, -X, +Y, -Y, +Z, -Z) into uint64_t (unorm8 per face).
static inline uint64_t pack_sky_cube(float px, float nx, float py, float ny, float pz, float nz)
{
  auto q8 = [](float v) -> uint32_t { return (uint32_t)clamp((int)(saturate(v) * 255.0f + 0.5f), 0, 255); };
  float mean = (px + nx + py + ny + pz + nz) * (1.0f / 6.0f);
  uint32_t lo = q8(px) | (q8(nx) << 8) | (q8(py) << 16) | (q8(ny) << 24);
  uint32_t hi = q8(pz) | (q8(nz) << 8) | (q8(mean) << 16);
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

// Pack validity bit + relocation offset (in grid-spacing units, [-0.5, 0.5] -> 3 x unorm10).
static inline uint32_t pack_probe_validity(bool valid, const Point3 &offset_in_spacing)
{
  if (!valid)
    return 0u;
  uint32_t ox = (uint32_t)clamp((int)((offset_in_spacing.x + 0.5f) * 1023.0f + 0.5f), 0, 1023);
  uint32_t oy = (uint32_t)clamp((int)((offset_in_spacing.y + 0.5f) * 1023.0f + 0.5f), 0, 1023);
  uint32_t oz = (uint32_t)clamp((int)((offset_in_spacing.z + 0.5f) * 1023.0f + 0.5f), 0, 1023);
  return PRT_PROBE_VALID_BIT | ox | (oy << 10) | (oz << 20);
}

// Build a PRT_DEPTH_RES x PRT_DEPTH_RES octahedral depth-moment map (unorm16 mean, unorm16 mean^2)
// from the traced sample directions using a cosine lobe filter (DDGI p=6).
static void build_probe_depth_map(const eastl::vector<Point3> &sample_dirs, const float *hit_dists, int sample_count,
                                  float max_depth, uint32_t out_texels[PRT_DEPTH_TEXELS])
{
  float invMax = (max_depth > 1e-4f) ? (1.0f / max_depth) : 1.0f;
  for (int ty = 0; ty < PRT_DEPTH_RES; ++ty)
  {
    for (int tx = 0; tx < PRT_DEPTH_RES; ++tx)
    {
      float u = ((float)tx + 0.5f) / (float)PRT_DEPTH_RES;
      float v = ((float)ty + 0.5f) / (float)PRT_DEPTH_RES;
      Point3 texDir = oct_decode(u, v);

      float sumM1 = 0.0f;
      float sumM2 = 0.0f;
      float sumW = 0.0f;
      for (int s = 0; s < sample_count; ++s)
      {
        float c = dot(texDir, sample_dirs[s]);
        if (c > 0.0f)
        {
          float c2 = c * c;
          float w = c2 * c2 * c2; // cos^6 lobe
          float d = clamp(hit_dists[s] * invMax, 0.0f, 1.0f);
          sumM1 += d * w;
          sumM2 += (d * d) * w;
          sumW += w;
        }
      }
      float m1 = (sumW > 1e-5f) ? (sumM1 / sumW) : 1.0f;
      float m2 = (sumW > 1e-5f) ? (sumM2 / sumW) : 1.0f;
      uint32_t q1 = (uint32_t)clamp((int)(m1 * 65535.0f + 0.5f), 0, 65535);
      uint32_t q2 = (uint32_t)clamp((int)(m2 * 65535.0f + 0.5f), 0, 65535);
      out_texels[ty * PRT_DEPTH_RES + tx] = q1 | (q2 << 16);
    }
  }
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
      Point3 surfMin = minPt - Point3(32.0f, 32.0f, 32.0f);
      Point3 surfMax = maxPt + Point3(32.0f, 32.0f, 32.0f);
      out_asset.sectors[secIdx].surfelBoundsMin = surfMin;
      out_asset.sectors[secIdx].surfelBoundsSize = max(Point3(1, 1, 1), surfMax - surfMin);
    }
  }

  uint32_t totalProbes = out_asset.grid.totalProbes();
  out_asset.probeToSectorMap.resize(totalProbes);

  eastl::vector<Point3> sampleDirs;
  eastl::vector<float> sampleWeights;
  constexpr int SAMPLE_RAY_COUNT = 64;
  generateSampleDirections(sampleDirs, sampleWeights, SAMPLE_RAY_COUNT);
  const float maxDepthRange = spacing * PRT_DEPTH_RANGE;
  const uint64_t openSkyCube = pack_sky_cube(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);

  // Unoccluded cosine weights per axis. +Y and the horizontal axes use the upper hemisphere;
  // the -Y axis gets its own lower-hemisphere weight (see the sky cube accumulation below).
  float maxSkyPosX = 0.0f, maxSkyNegX = 0.0f, maxSkyPosY = 0.0f, maxSkyPosZ = 0.0f, maxSkyNegZ = 0.0f, maxSkyNegY = 0.0f;
  for (int s = 0; s < SAMPLE_RAY_COUNT; ++s)
  {
    const Point3 &d = sampleDirs[s];
    if (d.y > -0.05f)
    {
      if (d.x > 0.0f) maxSkyPosX += d.x; else maxSkyNegX += -d.x;
      if (d.y > 0.0f) maxSkyPosY += d.y;
      if (d.z > 0.0f) maxSkyPosZ += d.z; else maxSkyNegZ += -d.z;
    }
    if (d.y < -0.05f)
      maxSkyNegY += -d.y;
  }
  maxSkyPosX = max(maxSkyPosX, 1e-4f);
  maxSkyNegX = max(maxSkyNegX, 1e-4f);
  maxSkyPosY = max(maxSkyPosY, 1e-4f);
  maxSkyPosZ = max(maxSkyPosZ, 1e-4f);
  maxSkyNegZ = max(maxSkyNegZ, 1e-4f);
  maxSkyNegY = max(maxSkyNegY, 1e-4f);

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
    const Point3 gridPos = out_asset.grid.getProbeWorldPos(coord);
    Point3 probePos = gridPos;

    PRTProbeData pData;
    memset(&pData, 0, sizeof(pData));
    pData.factorStart = (int32_t)sector.factors.size();
    pData.factorCount = 0;
    pData.captureOffset = Point3(0, 0, 0);
    pData.validity = pack_probe_validity(true, Point3(0, 0, 0));
    pData.globalProbeIndex = pIdx;

    // --- Fast Probe Classification & Pruning ---
    constexpr int TRACE_FLAGS = dacoll::ETF_ALL | dacoll::ETF_RI_PHYS;
    real downDist = 20.0f;
    int floorPmid = -1;
    Point3 floorNorm(0, 1, 0);
    bool hitDown = dacoll::traceray_normalized(probePos, Point3(0, -1, 0), downDist, &floorPmid, &floorNorm, TRACE_FLAGS);

    real upDist = 40.0f;
    int ceilPmid = -1;
    Point3 ceilNorm(0, -1, 0);
    bool hitUp = dacoll::traceray_normalized(probePos, Point3(0, 1, 0), upDist, &ceilPmid, &ceilNorm, TRACE_FLAGS);

    // Case 2: High in empty sky (no floor within 20m, no ceiling within 40m, and no nearby geometry)
    if (!hitDown && !hitUp)
    {
      real horizDist = 12.0f;
      bool nearMesh = dacoll::traceray_normalized(probePos, Point3(1, 0, 0), horizDist, nullptr, nullptr, TRACE_FLAGS) ||
                      dacoll::traceray_normalized(probePos, Point3(-1, 0, 0), horizDist, nullptr, nullptr, TRACE_FLAGS) ||
                      dacoll::traceray_normalized(probePos, Point3(0, 0, 1), horizDist, nullptr, nullptr, TRACE_FLAGS) ||
                      dacoll::traceray_normalized(probePos, Point3(0, 0, -1), horizDist, nullptr, nullptr, TRACE_FLAGS) ||
                      dacoll::traceray_normalized(probePos, Point3(0, 1, 0), horizDist, nullptr, nullptr, TRACE_FLAGS) ||
                      dacoll::traceray_normalized(probePos, Point3(0, -1, 0), horizDist, nullptr, nullptr, TRACE_FLAGS);
      if (!nearMesh)
      {
        pData.validity = pack_probe_validity(true, Point3(0, 0, 0));
        pData.factorCount = 0;
        sector.probeIds.push_back(pIdx);
        sector.probeData.push_back(pData);
        sector.skyVisibility.push_back(openSkyCube);
        for (int ti = 0; ti < PRT_DEPTH_TEXELS; ++ti)
          sector.probeDepth.push_back(0xFFFFFFFFu);
        culledSkyProbes++;
        continue;
      }
    }

    // --- DDGI-style Probe Relocation & Backface Classification ---
    // Instead of immediately killing probes within 12cm of a wall (which created black holes on walls)
    // or trusting single-sided hits from inside terrain/boxes (which leaked sky indoors), we trace the
    // spherical directions, count backfaces, and iteratively push embedded/hugging probes into open space.
    const float minFrontDist = spacing * 0.22f;
    const float maxOffsetLen = spacing * 0.45f;

    bool rayHit[SAMPLE_RAY_COUNT];
    float rayDist[SAMPLE_RAY_COUNT];
    Point3 rayNorm[SAMPLE_RAY_COUNT];
    int backfaceCount = 0;

    for (int iter = 0; iter < 3; ++iter)
    {
      backfaceCount = 0;
      float closestBackDist = 1e9f;
      Point3 closestBackDir(0, 0, 0);
      Point3 closestBackNorm(0, 1, 0);
      float closestFrontDist = 1e9f;
      Point3 closestFrontDir(0, 0, 0);

      for (int s = 0; s < SAMPLE_RAY_COUNT; ++s)
      {
        Point3 dir = sampleDirs[s];
        real t = 100.0f;
        int pmid = -1;
        Point3 norm(0, 1, 0);
        bool h = dacoll::traceray_normalized(probePos, dir, t, &pmid, &norm, TRACE_FLAGS);
        rayHit[s] = h;
        rayDist[s] = h ? (float)t : maxDepthRange;
        rayNorm[s] = norm;

        if (h)
        {
          if (dot(dir, norm) > 0.0f)
          {
            backfaceCount++;
            if ((float)t < closestBackDist)
            {
              closestBackDist = (float)t;
              closestBackDir = dir;
              closestBackNorm = norm;
            }
          }
          else if ((float)t < closestFrontDist)
          {
            closestFrontDist = (float)t;
            closestFrontDir = dir;
          }
        }
      }

      if (iter == 2)
        break;

      Point3 delta(0, 0, 0);
      if (backfaceCount > SAMPLE_RAY_COUNT / 4 && closestBackDist < spacing * 0.75f)
      {
        // Inside geometry: step past the closest backface along the ray or normal.
        Point3 outDir = (dot(closestBackDir, closestBackNorm) > 0.2f) ? closestBackDir : closestBackNorm;
        delta = outDir * (closestBackDist + spacing * 0.12f);
      }
      else if (closestFrontDist < minFrontDist)
      {
        // Too close to a front face: push away so the self-shadow bias will not straddle the wall.
        delta = -closestFrontDir * (minFrontDist - closestFrontDist + 0.02f);
      }
      else
      {
        break;
      }

      Point3 newOffset = (probePos + delta) - gridPos;
      newOffset.x = clamp(newOffset.x, -maxOffsetLen, maxOffsetLen);
      newOffset.y = clamp(newOffset.y, -maxOffsetLen, maxOffsetLen);
      newOffset.z = clamp(newOffset.z, -maxOffsetLen, maxOffsetLen);
      probePos = gridPos + newOffset;
    }

    // Re-check vertical ground/ceiling at the relocated position to catch probes under single-sided terrain.
    real relDownDist = 20.0f;
    int relFloorPmid = -1;
    Point3 relFloorNorm(0, 1, 0);
    bool relHitDown = dacoll::traceray_normalized(probePos, Point3(0, -1, 0), relDownDist, &relFloorPmid, &relFloorNorm, TRACE_FLAGS);
    real relUpDist = 40.0f;
    int relCeilPmid = -1;
    Point3 relCeilNorm(0, -1, 0);
    bool relHitUp = dacoll::traceray_normalized(probePos, Point3(0, 1, 0), relUpDist, &relCeilPmid, &relCeilNorm, TRACE_FLAGS);

    bool underTerrain = (!relHitDown && relHitUp && relCeilNorm.y > 0.2f) ||
                        (relHitDown && relDownDist < 0.25f && relFloorNorm.y <= 0.0f) ||
                        (relHitUp && relUpDist < 0.25f && relCeilNorm.y >= 0.0f);

    if (backfaceCount > SAMPLE_RAY_COUNT / 4 || underTerrain)
    {
      pData.captureOffset = probePos - gridPos;
      pData.validity = 0u;
      sector.probeIds.push_back(pIdx);
      sector.probeData.push_back(pData);
      sector.skyVisibility.push_back(0ull);
      for (int ti = 0; ti < PRT_DEPTH_TEXELS; ++ti)
        sector.probeDepth.push_back(0u);
      culledSolidProbes++;
      continue;
    }

    pData.captureOffset = probePos - gridPos;
    pData.validity = pack_probe_validity(true, pData.captureOffset / spacing);

    // Case 3: Active gameplay probe near surfaces -> accumulate 6-axis sky visibility cube, depth moments, and surfels
    activeTracedProbes++;
    float litSkyPosX = 0.0f, litSkyNegX = 0.0f;
    float litSkyPosY = 0.0f, litSkyNegY = 0.0f;
    float litSkyPosZ = 0.0f, litSkyNegZ = 0.0f;
    float depthDists[SAMPLE_RAY_COUNT];

    for (int s = 0; s < SAMPLE_RAY_COUNT; ++s)
    {
      if (!rayHit[s])
      {
        const Point3 &dir = sampleDirs[s];
        // +Y and the four horizontal faces keep their upper-hemisphere normalization (exterior
        // walls should not darken from the ground). The -Y face is measured on its own hemisphere.
        if (dir.y > -0.05f)
        {
          litSkyPosX += max(0.0f,  dir.x);
          litSkyNegX += max(0.0f, -dir.x);
          litSkyPosY += max(0.0f,  dir.y);
          litSkyPosZ += max(0.0f,  dir.z);
          litSkyNegZ += max(0.0f, -dir.z);
        }
        if (dir.y < -0.05f)
          litSkyNegY += -dir.y;
      }
    }

    float skyPx = clamp(litSkyPosX / maxSkyPosX, 0.0f, 1.0f);
    float skyNx = clamp(litSkyNegX / maxSkyNegX, 0.0f, 1.0f);
    float skyPy = clamp(litSkyPosY / maxSkyPosY, 0.0f, 1.0f);
    float skyPz = clamp(litSkyPosZ / maxSkyPosZ, 0.0f, 1.0f);
    float skyNz = clamp(litSkyNegZ / maxSkyNegZ, 0.0f, 1.0f);
    // Downward sky visibility is measured, not synthesized. It used to be derived from the upper
    // hemisphere (0.5*+Y + 0.125*horizontal), which handed ~65% sky visibility to the *underside*
    // of every ceiling and lit interior ceilings with outdoor sky.
    float skyNy = clamp(litSkyNegY / maxSkyNegY, 0.0f, 1.0f);

    for (int s = 0; s < SAMPLE_RAY_COUNT; ++s)
    {
      Point3 dir = sampleDirs[s];
      bool hit = rayHit[s];
      float t = rayDist[s];
      Point3 norm = rayNorm[s];

      if (hit)
      {
        // Backface hits mean the ray started slightly inside a thin sheet: clamp depth to 0 so
        // Chebyshev rejects points on the far side of that sheet.
        bool isBackface = dot(dir, norm) > 0.0f;
        depthDists[s] = isBackface ? 0.0f : min(t, maxDepthRange);

        if (!isBackface && t > 0.03f)
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

          // Upper-hemisphere sky exposure for the surfel (balanced 9-ray dome + normal-biased ray).
          Point3 traceStart = hitPos + norm * 0.04f;
          static const Point3 upTestDirs[9] = {
            Point3( 0.0000f, 1.0000f,  0.0000f),
            Point3( 0.3536f, 0.8660f,  0.3536f),
            Point3(-0.3536f, 0.8660f,  0.3536f),
            Point3( 0.3536f, 0.8660f, -0.3536f),
            Point3(-0.3536f, 0.8660f, -0.3536f),
            Point3( 0.6325f, 0.4472f,  0.6325f),
            Point3(-0.6325f, 0.4472f,  0.6325f),
            Point3( 0.6325f, 0.4472f, -0.6325f),
            Point3(-0.6325f, 0.4472f, -0.6325f)
          };
          float skyW = 0.0f;
          float skyLit = 0.0f;
          for (int ui = 0; ui < 9; ++ui)
          {
            float c = max(0.15f, dot(norm, upTestDirs[ui]));
            skyW += c;
            real dist = 50.0f;
            if (!dacoll::traceray_normalized(traceStart, upTestDirs[ui], dist, nullptr, nullptr, TRACE_FLAGS))
              skyLit += c;
          }
          Point3 normUpDir = normalize(norm + Point3(0.0f, 0.65f, 0.0f));
          if (normUpDir.y > 0.1f)
          {
            const float cNorm = 0.8f;
            skyW += cNorm;
            real dist = 50.0f;
            if (!dacoll::traceray_normalized(traceStart, normUpDir, dist, nullptr, nullptr, TRACE_FLAGS))
              skyLit += cNorm;
          }
          float coneSky = skyLit / max(skyW, 1e-4f);
          // No "near probe" floor here: boosting nearby surfels to the probe's mean sky visibility
          // handed outdoor sky light to surfaces just behind a wall and was a direct indoor leak.
          uint32_t skyVisByte = (uint32_t)clamp((int)(coneSky * 255.0f + 0.5f), 0, 255);

          surfel.positionXY = (uint32_t)qx | ((uint32_t)qy << 16);
          surfel.positionZAlbedoRG = (uint32_t)qz | (140 << 16) | (140 << 24); // realistic 0.55 albedo
          surfel.normalAlbedoB = encode_octahedral_normal(norm) | (140 << 24);
          surfel.nearestProbe = (int32_t)((pIdx & 0xFFFFu) | (skyVisByte << 16));

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

          // Evaluate SH9 for the sample direction with distance falloff
          float sh[9];
          evaluateSH9Basis(dir, sh);
          float distAtten = 1.0f / (1.0f + 0.04f * t * t);
          float w = sampleWeights[s] * distAtten;

          BrickFactor factor;
          memset(&factor, 0, sizeof(factor));
          factor.brickIndex = brickIdx;
          factor.sh01 = (uint32_t)floatToHalf(sh[0] * w) | ((uint32_t)floatToHalf(sh[1] * w) << 16);
          factor.sh23 = (uint32_t)floatToHalf(sh[2] * w) | ((uint32_t)floatToHalf(sh[3] * w) << 16);
          factor.sh45 = (uint32_t)floatToHalf(sh[4] * w) | ((uint32_t)floatToHalf(sh[5] * w) << 16);
          factor.sh67 = (uint32_t)floatToHalf(sh[6] * w) | ((uint32_t)floatToHalf(sh[7] * w) << 16);
          factor.sh8 = (uint32_t)floatToHalf(sh[8] * w);

          sector.factors.push_back(factor);
          pData.factorCount++;
          totalFactors++;
        }
      }
      else
      {
        depthDists[s] = maxDepthRange;
      }
    }

    uint32_t depthTexels[PRT_DEPTH_TEXELS];
    build_probe_depth_map(sampleDirs, depthDists, SAMPLE_RAY_COUNT, maxDepthRange, depthTexels);

    sector.probeIds.push_back(pIdx);
    sector.probeData.push_back(pData);
    sector.skyVisibility.push_back(pack_sky_cube(skyPx, skyNx, skyPy, skyNy, skyPz, skyNz));
    for (int ti = 0; ti < PRT_DEPTH_TEXELS; ++ti)
      sector.probeDepth.push_back(depthTexels[ti]);
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
