#ifndef PRTGI_COMMON_HLSLI
#define PRTGI_COMMON_HLSLI

struct Surfel
{
  uint positionXY;
  uint positionZAlbedoRG;
  uint normalAlbedoB;
  int nearestProbe;
};

float3 prt_surfel_position(Surfel surfel, float3 boundsMin, float3 boundsSize)
{
  float3 q = float3(surfel.positionXY & 0xFFFFu, surfel.positionXY >> 16, surfel.positionZAlbedoRG & 0xFFFFu) / 65535.0;
  return boundsMin + q * boundsSize;
}

float3 prt_surfel_normal(Surfel surfel)
{
  float2 e = float2(surfel.normalAlbedoB & 0xFFFu, (surfel.normalAlbedoB >> 12) & 0xFFFu) / 4095.0 * 2.0 - 1.0;
  float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
  float t = saturate(-n.z);
  n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
  return normalize(n);
}

float3 prt_surfel_albedo(Surfel surfel)
{
  float3 srgb = float3((surfel.positionZAlbedoRG >> 16) & 0xFFu, surfel.positionZAlbedoRG >> 24, surfel.normalAlbedoB >> 24) / 255.0;
  return srgb * srgb; // Approximate sRGB to linear
}

struct SurfelIndices
{
  uint surfelStart;
  uint surfelCount;
  uint renderingLayerMask;
  uint objectLayerMask;
};

struct BrickFactor
{
  uint brickSh0;
  uint sh12;
  uint sh34;
  uint sh56;
  uint sh78;
  uint _pad0;
  uint _pad1;
  uint _pad2;
};

uint prt_factor_brick(BrickFactor factor)
{
  return factor.brickSh0 & 0xFFFFu;
}

void prt_factor_sh(BrickFactor factor, out float sh[9])
{
  sh[0] = f16tof32(factor.brickSh0 >> 16);
  sh[1] = f16tof32(factor.sh12); sh[2] = f16tof32(factor.sh12 >> 16);
  sh[3] = f16tof32(factor.sh34); sh[4] = f16tof32(factor.sh34 >> 16);
  sh[5] = f16tof32(factor.sh56); sh[6] = f16tof32(factor.sh56 >> 16);
  sh[7] = f16tof32(factor.sh78); sh[8] = f16tof32(factor.sh78 >> 16);
}

struct PRTProbeData
{
  int factorStart;
  int factorCount;
  float3 captureOffset;
  uint validity;
  uint _pad0;
  uint _pad1;
};

int prt_wrap(int coordinate, int count)
{
  return coordinate - count * int(floor(float(coordinate) / float(count)));
}

int3 prt_publication_slot(int3 node, uint cascade, int4 slotCount, int4 windowMin)
{
  return int3(prt_wrap(node.x, slotCount.x), node.y - windowMin.y, prt_wrap(node.z, slotCount.z));
}

int3 prt_texture_coordinate(int3 slot, uint cascade, uint coefficient, int4 slotCount)
{
  return int3(slot.x, slot.z, slot.y + int(cascade * 9u + coefficient) * slotCount.y);
}

#endif

