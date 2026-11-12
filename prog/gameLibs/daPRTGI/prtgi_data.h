//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <daPRTGI/daPRTGI.h>
#include <EASTL/vector.h>
#include <util/dag_stdint.h>
#include <ioSys/dag_fileIo.h>
#include <math/dag_Point3.h>
#include <math/integer/dag_IPoint3.h>
#include <math/dag_bounds3.h>

namespace daprtgi
{

// Packed surfel: position (unorm16 in bounds), octahedral normal (unorm12), sRGB albedo (unorm8)
#pragma pack(push, 1)
struct Surfel
{
  uint32_t positionXY;
  uint32_t positionZAlbedoRG;
  uint32_t normalAlbedoB;
  int32_t nearestProbe;
};

struct SurfelIndices
{
  uint32_t surfelStart;
  uint32_t surfelCount;
  uint32_t renderingLayerMask;
  uint32_t objectLayerMask;
};

// 9 SH coefficients packed as 16-bit half floats and brick index (Padded to 32 bytes for hardware-friendly StructuredBuffer stride)
struct BrickFactor
{
  uint32_t brickIndex; // 32-bit full brick index (supports millions of bricks per sector)
  uint32_t sh01;       // 16-bit half SH0, 16-bit half SH1
  uint32_t sh23;       // 16-bit half SH2, 16-bit half SH3
  uint32_t sh45;       // 16-bit half SH4, 16-bit half SH5
  uint32_t sh67;       // 16-bit half SH6, 16-bit half SH7
  uint32_t sh8;        // 16-bit half SH8, 16-bit pad
  uint32_t _pad[2];    // Pad to 32 bytes (multiple of 16)
};

struct PRTProbeData
{
  int32_t factorStart;
  int32_t factorCount;
  Point3 captureOffset;
  uint32_t validity; // packed intensity & validity
  uint32_t globalProbeIndex;
  uint32_t _pad;     // Pad to 32 bytes (multiple of 16)
};
#pragma pack(pop)

// Layout constants shared with prtgi_common.hlsli
static constexpr int PRT_DEPTH_RES = 6;
static constexpr int PRT_DEPTH_TEXELS = PRT_DEPTH_RES * PRT_DEPTH_RES; // 36
static constexpr float PRT_DEPTH_RANGE = 3.0f; // in grid-spacing units
static constexpr uint32_t PRT_PROBE_VALID_BIT = 0x80000000u;

// Published irradiance coefficients per probe: L0 + L1 (y, z, x) RGB triples.
// The L2 band (SH9 c4..c8) was stored but never sampled, so it is not published at all.
static constexpr int PRT_COEFF_SLICES = 4;

struct PRTGridDesc
{
  Point3 origin = Point3(0, 0, 0);
  IPoint3 minCoord = IPoint3(0, 0, 0);
  IPoint3 count = IPoint3(0, 0, 0);
  float spacing = 2.0f;

  inline uint32_t totalProbes() const
  {
    return (uint32_t)count.x * (uint32_t)count.y * (uint32_t)count.z;
  }

  inline uint32_t getProbeIndex(const IPoint3 &coord) const
  {
    IPoint3 local = coord - minCoord;
    return (uint32_t)(local.x * count.y * count.z + local.y * count.z + local.z);
  }

  inline IPoint3 getProbeCoord(uint32_t index) const
  {
    int yz = count.y * count.z;
    if (yz <= 0) return minCoord;
    int x = (int)index / yz;
    int rem = (int)index % yz;
    return IPoint3(x, rem / count.z, rem % count.z) + minCoord;
  }

  inline Point3 getProbeWorldPos(const IPoint3 &coord) const
  {
    return origin + Point3((float)coord.x, (float)coord.y, (float)coord.z) * spacing;
  }
};

struct PRTSectorData
{
  int sectorIndex = -1;
  IPoint3 sectorCoord = IPoint3(0, 0, 0);
  BBox3 bounds;

  eastl::vector<uint32_t> probeIds;
  eastl::vector<PRTProbeData> probeData;
  eastl::vector<BrickFactor> factors;
  eastl::vector<uint64_t> skyVisibility; // 6-axis ambient cube sky visibility per probe
  eastl::vector<uint32_t> probeDepth;    // PRT_DEPTH_TEXELS x (unorm16 mean, unorm16 mean^2) per probe

  eastl::vector<SurfelIndices> brickInfo;
  eastl::vector<Surfel> surfels;
  Point3 surfelBoundsMin = Point3(0, 0, 0);
  Point3 surfelBoundsSize = Point3(1, 1, 1);
};

struct PRTAsset
{
  static constexpr uint32_t ASSET_MAGIC = 0x50525447; // 'PRTG'
  // Bump whenever the baked content or its meaning changes, not just its layout:
  // 8: removed the "near probe" sky floor from surfel cone visibility (indoor leak source).
  // 9: -Y sky-cube face is measured on the lower hemisphere instead of synthesized from +Y.
  static constexpr uint32_t ASSET_VERSION = 9;

  PRTGridDesc grid;
  IPoint3 sectorGridCount = IPoint3(0, 0, 0);
  int sectorSizeInProbes = 32;
  uint64_t geometryHash = 0;

  eastl::vector<PRTSectorData> sectors;
  eastl::vector<int> probeToSectorMap;

  bool serialize(IGenSave &writer) const;
  bool deserialize(IGenLoad &reader);
  void clear();
};

} // namespace daprtgi
