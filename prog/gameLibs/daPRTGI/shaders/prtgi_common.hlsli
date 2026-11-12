#ifndef PRTGI_COMMON_HLSLI
#define PRTGI_COMMON_HLSLI

// Published irradiance bands per probe (L0 + L1 y/z/x). Defined before first use because
// HLSL macros must exist at the point of expansion. Must match prtgi_data.h.
#define PRT_COEFF_SLICES 4

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

float prt_surfel_sky_vis(Surfel surfel)
{
  return float((surfel.nearestProbe >> 16) & 0xFFu) / 255.0f;
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
  uint brickIndex;
  uint sh01;
  uint sh23;
  uint sh45;
  uint sh67;
  uint sh8;
  uint _pad0;
  uint _pad1;
};

uint prt_factor_brick(BrickFactor factor)
{
  return factor.brickIndex;
}

void prt_factor_sh(BrickFactor factor, out float sh[9])
{
  sh[0] = f16tof32(factor.sh01);
  sh[1] = f16tof32(factor.sh01 >> 16);
  sh[2] = f16tof32(factor.sh23);
  sh[3] = f16tof32(factor.sh23 >> 16);
  sh[4] = f16tof32(factor.sh45);
  sh[5] = f16tof32(factor.sh45 >> 16);
  sh[6] = f16tof32(factor.sh67);
  sh[7] = f16tof32(factor.sh67 >> 16);
  sh[8] = f16tof32(factor.sh8);
}

struct PRTProbeData
{
  int factorStart;
  int factorCount;
  float3 captureOffset;
  uint validity;
  uint globalProbeIndex;
  uint _pad0;
};

int prt_wrap(int coordinate, int count)
{
  return coordinate - count * int(floor(float(coordinate) / float(count)));
}

int3 prt_publication_slot(int3 node, uint cascade, int4 slotCount, int4 windowMin)
{
  return int3(prt_wrap(node.x, slotCount.x), node.y - windowMin.y, prt_wrap(node.z, slotCount.z));
}

// Only a single cascade and the L0+L1 bands are published (see PRT_COEFF_SLICES).
// The bands are packed adjacently along X: a 3D texture is stored slice-by-slice in Z, so
// putting the coefficient on Z would scatter one probe's 4 taps across 4 separate slices.
int3 prt_texture_coordinate(int3 slot, uint cascade, uint coefficient, int4 slotCount)
{
  return int3(slot.x * PRT_COEFF_SLICES + int(coefficient), slot.z, slot.y);
}

// ---------------------------------------------------------------------------
// Probe layout constants. Must match prtgi_data.h (PRT_DEPTH_RES, PRT_DEPTH_RANGE).
// ---------------------------------------------------------------------------
#define PRT_DEPTH_RES 6
#define PRT_DEPTH_TEXELS 36
// Depth moments are normalized by (grid spacing * PRT_DEPTH_RANGE).
#define PRT_DEPTH_RANGE 3.0f
#define PRT_PROBE_VALID_BIT 0x80000000u

// Probe validity word: bit31 = valid, bits 0..29 = relocation offset (3 x unorm10)
// in grid-spacing units, mapped from [-0.5, 0.5].
bool prt_probe_valid(uint bits)
{
  return (bits & PRT_PROBE_VALID_BIT) != 0u;
}

float3 prt_probe_offset(uint bits)
{
  return float3(bits & 0x3FFu, (bits >> 10) & 0x3FFu, (bits >> 20) & 0x3FFu) * (1.0f / 1023.0f) - 0.5f;
}

// Sky visibility is stored as a 6-axis Ambient Cube (+X, -X, +Y, -Y, +Z, -Z) in unorm8:
// packed.x = visPosX | (visNegX << 8) | (visPosY << 16) | (visNegY << 24)
// packed.y = visPosZ | (visNegZ << 8) | (meanSky << 16)
// Each cardinal axis is normalized by the unoccluded sky weight in its hemisphere,
// so opposite hemispheres (+X vs -X) and the ground hemisphere do not darken exterior walls.
void prt_sky_cube_unpack(uint2 packed, out float3 posVis, out float3 negVis)
{
  posVis = float3(packed.x & 0xFFu, (packed.x >> 16) & 0xFFu, packed.y & 0xFFu) * (1.0f / 255.0f);
  negVis = float3((packed.x >> 8) & 0xFFu, (packed.x >> 24) & 0xFFu, (packed.y >> 8) & 0xFFu) * (1.0f / 255.0f);
}

float4 prt_sky_sh_unpack(uint2 packed)
{
  float3 posVis, negVis;
  prt_sky_cube_unpack(packed, posVis, negVis);
  float l0 = dot(posVis + negVis, float3(1.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f));
  float3 l1 = (posVis - negVis) * 0.5f;
  return float4(l0, l1);
}

float prt_sky_vis_sh(uint2 packed, float3 normal)
{
  float3 posVis, negVis;
  prt_sky_cube_unpack(packed, posVis, negVis);
  float3 n2 = normal * normal;
  float3 vis = float3(
    normal.x >= 0.0f ? posVis.x : negVis.x,
    normal.y >= 0.0f ? posVis.y : negVis.y,
    normal.z >= 0.0f ? posVis.z : negVis.z);
  return saturate(dot(n2, vis));
}

// Octahedral mapping; must match prtgi_baker.cpp oct_decode().
float2 prt_oct_encode(float3 n)
{
  n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
  float2 e = n.xy;
  if (n.z < 0.0f)
    e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
  return e * 0.5f + 0.5f;
}

// Octahedral border wrap for bilinear taps outside [0, RES-1].
int2 prt_oct_wrap(int2 t)
{
  const int R = PRT_DEPTH_RES;
  if (t.x < 0)       { t.x = 0;     t.y = R - 1 - t.y; }
  else if (t.x >= R) { t.x = R - 1; t.y = R - 1 - t.y; }
  if (t.y < 0)       { t.y = 0;     t.x = R - 1 - t.x; }
  else if (t.y >= R) { t.y = R - 1; t.x = R - 1 - t.x; }
  return clamp(t, 0, R - 1);
}

// Linear index uses slot.xyz directly. Do not use prt_texture_coordinate, which swaps Y/Z.
uint prt_sky_mask_index(int3 slot, int4 slotCount)
{
  return (uint)slot.x + (uint)slotCount.x * ((uint)slot.z + (uint)slotCount.z * (uint)slot.y);
}

#endif

