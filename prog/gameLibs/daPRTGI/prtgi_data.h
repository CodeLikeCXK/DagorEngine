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
  uint32_t brickSh0; // lower 16-bit: brick index, upper 16-bit: half SH0
  uint32_t sh12;     // 16-bit half SH1, 16-bit half SH2
  uint32_t sh34;     // 16-bit half SH3, 16-bit half SH4
  uint32_t sh56;     // 16-bit half SH5, 16-bit half SH6
  uint32_t sh78;     // 16-bit half SH7, 16-bit half SH8
  uint32_t _pad[3];  // Pad to 32 bytes (multiple of 16)
};

struct PRTProbeData
{
  int32_t factorStart;
  int32_t factorCount;
  Point3 captureOffset;
  uint32_t validity; // packed intensity & validity
  uint32_t _pad[2];  // Pad to 32 bytes (multiple of 16)
};
#pragma pack(pop)

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
  eastl::vector<uint32_t> skyVisibility; // 32 directions bitmask per probe

  eastl::vector<SurfelIndices> brickInfo;
  eastl::vector<Surfel> surfels;
  Point3 surfelBoundsMin = Point3(0, 0, 0);
  Point3 surfelBoundsSize = Point3(1, 1, 1);
};

struct PRTAsset
{
  static constexpr uint32_t ASSET_MAGIC = 0x50525447; // 'PRTG'
  static constexpr uint32_t ASSET_VERSION = 2;

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

