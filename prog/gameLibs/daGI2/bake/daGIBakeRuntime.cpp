// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include <daGI2/daGIBake.h>
#include "daGIBakeCache.h"

#include <perfMon/dag_cpuFreq.h>
#include <perfMon/dag_statDrv.h>
#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_draw.h>
#include <drv/3d/dag_vertexIndexBuffer.h>
#include <drv/3d/dag_lock.h>
#include <drv/3d/dag_tex3d.h>
#include <drv/3d/dag_buffers.h>
#include <drv/3d/dag_rwResource.h>
#include <3d/dag_resPtr.h>
#include <3d/dag_lockSbuffer.h>
#include <3d/dag_resourceTags.h>
#include <shaders/dag_shaders.h>
#include <shaders/dag_shaderBlock.h>
#include <shaders/dag_computeShaders.h>
#include <shaders/dag_DynamicShaderHelper.h>
#include <render/spheres_consts.hlsli>
#include <render/voxelClip.h>
#include <daSDF/worldSDF.h>
#include <math/dag_mathUtils.h>
#include <math/dag_half.h>
#include <debug/dag_debug.h>
#include <debug/dag_debug3d.h>
#include <util/dag_finally.h>
#include <cmath>
#include <cstring>

extern void dagi_publish_irradiance_first_valid(int first_valid, float probe_size0);

namespace dagi_bake_runtime
{
namespace
{
static ShaderVariableInfo dagi_baked_indirectionVarId("dagi_baked_indirection", true);
static ShaderVariableInfo dagi_baked_activeVarId("dagi_baked_active", true);
static ShaderVariableInfo dagi_baked_grid_dimVarId("dagi_baked_grid_dim", true);
static ShaderVariableInfo dagi_baked_grid_originVarId("dagi_baked_grid_origin", true);
static ShaderVariableInfo dagi_baked_atlas_invVarId("dagi_baked_atlas_inv", true);

static ShaderVariableInfo dagi_bake_seed_paramsVarId("dagi_bake_seed_params", true);
static ShaderVariableInfo dagi_bake_trace_paramsVarId("dagi_bake_trace_params", true);
static ShaderVariableInfo dagi_bake_brick_coordsVarId("dagi_bake_brick_coords", true);
static ShaderVariableInfo dagi_bake_brick_flagsVarId("dagi_bake_brick_flags", true);
static ShaderVariableInfo dagi_bake_brick_probesVarId("dagi_bake_brick_probes", true);
static ShaderVariableInfo dagi_baked_debug_box_minVarId("dagi_baked_debug_box_min", true);
static ShaderVariableInfo dagi_baked_debug_box_dimVarId("dagi_baked_debug_box_dim", true);
static ShaderVariableInfo dagi_baked_debug_paramsVarId("dagi_baked_debug_params", true);

static ShaderVariableInfo dagi_irradiance_grid_sph0VarId("dagi_irradiance_grid_sph0", true);
static ShaderVariableInfo dagi_irradiance_grid_sph1VarId("dagi_irradiance_grid_sph1", true);
static ShaderVariableInfo dagi_irrad_grid_clipmap_sizeiVarId("dagi_irrad_grid_clipmap_sizei", true);
static ShaderVariableInfo dagi_irrad_grid_clipmap_lt_coord_0VarId("dagi_irrad_grid_clipmap_lt_coord_0", true);
static ShaderVariableInfo dagi_rad_grid_clipmap_sizeiVarId("dagi_rad_grid_clipmap_sizei", true);
static ShaderVariableInfo dagi_rad_grid_clipmap_lt_coord_0VarId("dagi_rad_grid_clipmap_lt_coord_0", true);
static ShaderVariableInfo dagi_radiance_gridVarId("dagi_radiance_grid", true);
static ShaderVariableInfo gi_froxels_sph0VarId("gi_froxels_sph0", true);
static ShaderVariableInfo gi_froxels_sph1VarId("gi_froxels_sph1", true);
static ShaderVariableInfo gi_froxels_sizeiVarId("gi_froxels_sizei", true);
static ShaderVariableInfo dagi_sky_visibility_sphVarId("dagi_sky_visibility_sph", true);
static ShaderVariableInfo dagi_sky_vis_clipmap_sizeiVarId("dagi_sky_vis_clipmap_sizei", true);

dagi_bake::Tier to_cache_tier(Tier t)
{
  if (t == Tier::Normal)
    return dagi_bake::Tier::Normal;
  if (t == Tier::High)
    return dagi_bake::Tier::High;
  return dagi_bake::Tier::None;
}

Tier from_cache_tier(dagi_bake::Tier t)
{
  if (t == dagi_bake::Tier::Normal)
    return Tier::Normal;
  if (t == dagi_bake::Tier::High)
    return Tier::High;
  return Tier::None;
}

dagi_bake::SourceMode to_cache_source(SourceMode m)
{
  switch (m)
  {
    case SourceMode::Baked: return dagi_bake::SourceMode::Baked;
    case SourceMode::Dynamic: return dagi_bake::SourceMode::Dynamic;
    default: return dagi_bake::SourceMode::Auto;
  }
}

uint32_t calc_atlas_side(uint32_t brick_count)
{
  uint32_t side = 1;
  while (side * side * side < max<uint32_t>(brick_count, 1u))
    ++side;
  return side;
}
} // namespace

const char *tier_name(Tier t) { return dagi_bake::tier_name(to_cache_tier(t)); }

bool parse_tier(const char *s, Tier &out)
{
  dagi_bake::Tier ct = dagi_bake::Tier::None;
  if (!dagi_bake::parse_tier(s, ct))
    return false;
  out = from_cache_tier(ct);
  return true;
}

const char *source_mode_name(SourceMode m) { return dagi_bake::source_mode_name(to_cache_source(m)); }

bool parse_source_mode(const char *s, SourceMode &out)
{
  dagi_bake::SourceMode cm = dagi_bake::SourceMode::Auto;
  if (!dagi_bake::parse_source_mode(s, cm))
    return false;
  switch (cm)
  {
    case dagi_bake::SourceMode::Baked: out = SourceMode::Baked; break;
    case dagi_bake::SourceMode::Dynamic: out = SourceMode::Dynamic; break;
    default: out = SourceMode::Auto; break;
  }
  return true;
}

eastl::string make_cache_path(const char *dir, const char *level_id, Tier tier)
{
  const std::string fname = dagi_bake::cache_file_name(level_id ? level_id : "level", to_cache_tier(tier));
  if (!dir || !*dir)
    return eastl::string(fname.c_str());
  eastl::string out(dir);
  if (out.back() != '/' && out.back() != '\\')
    out += '/';
  out += fname.c_str();
  return out;
}

static constexpr uint32_t CURRENT_BAKE_PRODUCER_VERSION = 2u;

bool probe_cache_valid(const char *path, uint64_t expected_content_hash, uint64_t expected_lighting_sig)
{
  if (!path || !*path)
    return false;
  dagi_bake::Header hdr;
  std::string err;
  if (!dagi_bake::read_header(path, hdr, &err))
    return false;
  if (hdr.producerVersion < CURRENT_BAKE_PRODUCER_VERSION)
    return false;
  if (expected_content_hash != 0 && hdr.contentHash != 0 && hdr.contentHash != expected_content_hash)
    return false;
  if (expected_lighting_sig != 0 && hdr.lightingSignature != 0 && hdr.lightingSignature != expected_lighting_sig)
    return false;
  return hdr.brickSize == dagi_bake::BRICK_PROBES_SIDE;
}

Selection select_mode(Tier configured, SourceMode source_mode, bool can_use_dynamic, bool normal_valid, bool high_valid,
  bool force_rebuild)
{
  const dagi_bake::Selection cs =
    dagi_bake::select(to_cache_tier(configured), to_cache_source(source_mode), can_use_dynamic, normal_valid, high_valid, force_rebuild);
  Selection s;
  s.loadTier = from_cache_tier(cs.loadTier);
  s.useDynamic = cs.useDynamic;
  s.rebuildFirst = cs.rebuildFirst;
  s.rebuildTier = from_cache_tier(cs.rebuildTier);
  s.promptRebuildHigh = cs.promptRebuildHigh;
  return s;
}

struct BakedGIVolume::Impl
{
  UniqueBuf fallbackIndirection;
  UniqueTex fallbackSph0;
  UniqueTex fallbackSph1;
  UniqueTex fallbackRadGrid;

  UniqueBuf indirectionBuf;
  UniqueTex atlasSph0;
  UniqueTex atlasSph1;
  eastl::unique_ptr<ComputeShaderElement> uploadAtlasCs;
  DynamicShaderHelper debugProbesShader;

  dagi_bake::Header loadedHeader;
  dagi_bake::VolumeData cpuVolume;
  std::vector<uint16_t> cpuIndirectionTable;
  BBox3 worldBounds;
  uint32_t atlasTexelsCount = 0;
  bool samplingEnabled = true;
  bool loaded = false;

  void initFallback()
  {
    if (!fallbackIndirection)
    {
      fallbackIndirection = dag::create_sbuffer(sizeof(uint32_t), 16, SBCF_BIND_SHADER_RES | SBCF_MISC_ALLOW_RAW, 0,
        "dagi_baked_indirection_fallback", RESTAG_DAGI2);
      if (fallbackIndirection)
      {
        uint32_t invalidWords[16] = {};
        for (int i = 12; i < 16; ++i)
          invalidWords[i] = 0xFFFFFFFFu;
        fallbackIndirection->updateData(0, sizeof(invalidWords), invalidWords, VBLOCK_WRITEONLY);
      }
    }
    if (!fallbackSph0)
    {
      fallbackSph0 = dag::create_voltex(2, 2, 2, TEXCF_UNORDERED | TEXFMT_A8R8G8B8, 1, "dagi_baked_sph0_fallback", RESTAG_DAGI2);
      if (fallbackSph0)
      {
        d3d::clear_rwtexf(fallbackSph0.getVolTex(), ResourceClearValue{}.asFloat, 0, 0);
        d3d::resource_barrier({fallbackSph0.getVolTex(), RB_RO_SRV | RB_STAGE_PIXEL | RB_STAGE_COMPUTE | RB_STAGE_VERTEX, 0, 0});
      }
    }
    if (!fallbackSph1)
    {
      fallbackSph1 = dag::create_voltex(2, 2, 2, TEXCF_UNORDERED | TEXFMT_A16B16G16R16F, 1, "dagi_baked_sph1_fallback", RESTAG_DAGI2);
      if (fallbackSph1)
      {
        d3d::clear_rwtexf(fallbackSph1.getVolTex(), ResourceClearValue{}.asFloat, 0, 0);
        d3d::resource_barrier({fallbackSph1.getVolTex(), RB_RO_SRV | RB_STAGE_PIXEL | RB_STAGE_COMPUTE | RB_STAGE_VERTEX, 0, 0});
      }
    }
    if (!fallbackRadGrid)
    {
      fallbackRadGrid =
        dag::create_voltex(2, 2, 2, TEXCF_UNORDERED | TEXFMT_R11G11B10F, 1, "dagi_baked_radgrid_fallback", RESTAG_DAGI2);
      if (fallbackRadGrid)
      {
        d3d::clear_rwtexf(fallbackRadGrid.getVolTex(), ResourceClearValue{}.asFloat, 0, 0);
        d3d::resource_barrier({fallbackRadGrid.getVolTex(), RB_RO_SRV | RB_STAGE_PIXEL | RB_STAGE_COMPUTE | RB_STAGE_VERTEX, 0, 0});
      }
    }

    if (!loaded)
    {
      ShaderGlobal::set_int(dagi_baked_activeVarId, 0);
      ShaderGlobal::set_int4(dagi_baked_grid_dimVarId, 0, 0, 0, 0);
      if (fallbackIndirection)
        ShaderGlobal::set_buffer(dagi_baked_indirectionVarId, fallbackIndirection);
    }
  }

  void bindStandInTexturesWhenDaGIOff()
  {
    initFallback();
    auto isUnbound = [](const ShaderVariableInfo &v) {
      return v.get_var_id() >= 0 && ShaderGlobal::get_tex(v.get_var_id()) == BAD_TEXTUREID;
    };
    if (fallbackRadGrid && isUnbound(dagi_radiance_gridVarId))
      ShaderGlobal::set_texture(dagi_radiance_gridVarId, fallbackRadGrid);
    if (fallbackRadGrid && isUnbound(gi_froxels_sph0VarId))
    {
      ShaderGlobal::set_texture(gi_froxels_sph0VarId, fallbackRadGrid);
      ShaderGlobal::set_int4(gi_froxels_sizeiVarId, 0, 0, 0, 0);
    }
    if (fallbackSph1 && isUnbound(gi_froxels_sph1VarId))
      ShaderGlobal::set_texture(gi_froxels_sph1VarId, fallbackSph1);
    if (fallbackSph1 && isUnbound(dagi_sky_visibility_sphVarId))
    {
      ShaderGlobal::set_texture(dagi_sky_visibility_sphVarId, fallbackSph1);
      ShaderGlobal::set_int4(dagi_sky_vis_clipmap_sizeiVarId, 0, 0, 0, 0);
    }
  }

  bool uploadVolumeToGpu()
  {
    initFallback();
    if (!uploadAtlasCs)
      uploadAtlasCs.reset(new_compute_shader("dagi_bake_upload_atlas_cs"));
    if (!uploadAtlasCs)
    {
      logerr("daGIBake: compute shader 'dagi_bake_upload_atlas_cs' not found");
      return false;
    }

    const uint32_t gx = cpuVolume.gridDim[0];
    const uint32_t gy = cpuVolume.gridDim[1];
    const uint32_t gz = cpuVolume.gridDim[2];
    const uint32_t totalGridBricks = gx * gy * gz;
    const uint32_t nBricks = uint32_t(cpuVolume.bricks.size());

    std::vector<uint16_t> table(totalGridBricks, dagi_bake::INVALID_BRICK_INDEX);
    for (uint32_t i = 0; i < nBricks; ++i)
    {
      const auto &b = cpuVolume.bricks[i];
      const uint32_t linear = b.x + gx * (b.y + gy * b.z);
      if (linear < totalGridBricks)
        table[linear] = uint16_t(i);
    }
    cpuIndirectionTable = table;

    const uint32_t atlasSide = calc_atlas_side(max<uint32_t>(nBricks, 1u));
    const uint32_t atlasTexels = atlasSide * 10u;
    const float invTexels = 1.f / float(atlasTexels);
    const float invProbeSize = 1.f / cpuVolume.probeSize;

    // 48-byte (12 dword) header in dagi_baked_indirection:
    // [0..3]  int4 grid_dim (gx, gy, gz, atlasSide)
    // [4..7]  float4 grid_origin (origin.xyz, 1/probeSize)
    // [8..11] float4 atlas_inv (1/atlasTexels, 1/atlasTexels, 1/atlasTexels, probeSize)
    // [12..]  packed uint16 brick indirection table
    const uint32_t headerDwords = 12u;
    const uint32_t indirectionDwords = max<uint32_t>((totalGridBricks + 1u) / 2u, 1u);
    std::vector<uint32_t> packedIndirection(headerDwords + indirectionDwords, 0xFFFFFFFFu);
    packedIndirection[0] = gx;
    packedIndirection[1] = gy;
    packedIndirection[2] = gz;
    packedIndirection[3] = atlasSide;
    packedIndirection[4] = bitwise_cast<uint32_t>(cpuVolume.origin[0]);
    packedIndirection[5] = bitwise_cast<uint32_t>(cpuVolume.origin[1]);
    packedIndirection[6] = bitwise_cast<uint32_t>(cpuVolume.origin[2]);
    packedIndirection[7] = bitwise_cast<uint32_t>(invProbeSize);
    packedIndirection[8] = bitwise_cast<uint32_t>(invTexels);
    packedIndirection[9] = bitwise_cast<uint32_t>(invTexels);
    packedIndirection[10] = bitwise_cast<uint32_t>(invTexels);
    packedIndirection[11] = bitwise_cast<uint32_t>(cpuVolume.probeSize);
    std::memcpy(packedIndirection.data() + headerDwords, table.data(), totalGridBricks * sizeof(uint16_t));

    indirectionBuf.close();
    indirectionBuf = dag::create_sbuffer(sizeof(uint32_t), headerDwords + indirectionDwords,
      SBCF_BIND_SHADER_RES | SBCF_MISC_ALLOW_RAW, 0, "dagi_baked_indirection", RESTAG_DAGI2);
    if (!indirectionBuf ||
        !indirectionBuf->updateData(0, uint32_t(packedIndirection.size() * sizeof(uint32_t)), packedIndirection.data(), VBLOCK_WRITEONLY))
      return false;

    atlasSph0.close();
    atlasSph1.close();
    atlasSph1 = dag::create_voltex(atlasTexels, atlasTexels, atlasTexels * 2u, TEXCF_UNORDERED | TEXFMT_A16B16G16R16F, 1,
      "dagi_baked_atlas_sph1", RESTAG_DAGI2);
    if (!atlasSph1)
      return false;
    atlasTexelsCount = atlasTexels;

    // Bind stand-in fallback textures BEFORE setting real baked indirection/atlas globals,
    // otherwise initFallback() (with loaded == false) overwrites dagi_baked_indirectionVarId.
    bindStandInTexturesWhenDaGIOff();
    ShaderGlobal::set_texture(dagi_irradiance_grid_sph0VarId, fallbackSph0);
    ShaderGlobal::set_texture(dagi_irradiance_grid_sph1VarId, atlasSph1);
    ShaderGlobal::set_buffer(dagi_baked_indirectionVarId, indirectionBuf);
    ShaderGlobal::set_int4(dagi_baked_grid_dimVarId, int(gx), int(gy), int(gz), int(atlasSide));
    ShaderGlobal::set_float4(dagi_baked_grid_originVarId, cpuVolume.origin[0], cpuVolume.origin[1], cpuVolume.origin[2],
      invProbeSize);
    ShaderGlobal::set_float4(dagi_baked_atlas_invVarId, invTexels, invTexels, invTexels, cpuVolume.probeSize);

    // Publish valid probeSize0 and empty clipmap sizes so deferred shaders read probeSize(0) without sampling dynamic cascades
    const IPoint3 invLt = VoxelClip::get_invalid_lt();
    ShaderGlobal::set_int4(dagi_irrad_grid_clipmap_sizeiVarId, 0, 0, 0, 0);
    ShaderGlobal::set_int4(dagi_irrad_grid_clipmap_lt_coord_0VarId, invLt.x, invLt.y, invLt.z,
      bitwise_cast<int32_t>(cpuVolume.probeSize));
    ShaderGlobal::set_int4(dagi_rad_grid_clipmap_sizeiVarId, 0, 0, 0, 0);
    ShaderGlobal::set_int4(dagi_rad_grid_clipmap_lt_coord_0VarId, invLt.x, invLt.y, invLt.z, bitwise_cast<int32_t>(1e9f));
    dagi_publish_irradiance_first_valid(0, cpuVolume.probeSize);

    if (nBricks > 0)
    {
      std::vector<uint32_t> coordDwords(nBricks * 4u, 0u);
      for (uint32_t i = 0; i < nBricks; ++i)
      {
        coordDwords[i * 4u + 0u] = cpuVolume.bricks[i].x;
        coordDwords[i * 4u + 1u] = cpuVolume.bricks[i].y;
        coordDwords[i * 4u + 2u] = cpuVolume.bricks[i].z;
        coordDwords[i * 4u + 3u] = i;
      }

      const size_t totalProbes = size_t(nBricks) * dagi_bake::BRICK_PROBES_COUNT;
      std::vector<uint32_t> probeDwords(totalProbes * 4u, 0u);
      for (size_t i = 0; i < totalProbes; ++i)
      {
        probeDwords[i * 4u + 0u] = cpuVolume.sph0[i];
        probeDwords[i * 4u + 1u] = uint32_t(cpuVolume.sph1[i] & 0xFFFFFFFFull);
        probeDwords[i * 4u + 2u] = uint32_t(cpuVolume.sph1[i] >> 32u);
        probeDwords[i * 4u + 3u] = 1u;
      }

      UniqueBuf coordStaging = dag::create_sbuffer(sizeof(uint32_t), uint32_t(coordDwords.size()),
        SBCF_BIND_SHADER_RES | SBCF_MISC_ALLOW_RAW, 0, "dagi_bake_upload_coords", RESTAG_DAGI2);
      UniqueBuf probeStaging = dag::create_sbuffer(sizeof(uint32_t), uint32_t(probeDwords.size()),
        SBCF_BIND_SHADER_RES | SBCF_MISC_ALLOW_RAW, 0, "dagi_bake_upload_probes", RESTAG_DAGI2);

      if (!coordStaging || !probeStaging)
        return false;
      coordStaging->updateData(0, uint32_t(coordDwords.size() * sizeof(uint32_t)), coordDwords.data(), VBLOCK_WRITEONLY);
      probeStaging->updateData(0, uint32_t(probeDwords.size() * sizeof(uint32_t)), probeDwords.data(), VBLOCK_WRITEONLY);

      ShaderGlobal::set_buffer(dagi_bake_brick_coordsVarId, coordStaging);
      ShaderGlobal::set_buffer(dagi_bake_brick_probesVarId, probeStaging);
      ShaderGlobal::set_int4(dagi_bake_trace_paramsVarId, int(nBricks), 0, 0, int(atlasSide));

      const int prevFrameBlk = ShaderGlobal::getBlock(ShaderGlobal::LAYER_FRAME);
      ShaderGlobal::setBlock(-1, ShaderGlobal::LAYER_FRAME);

      d3d::GpuAutoLock gpuLock;
      d3d::resource_barrier({atlasSph1.getVolTex(), RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
      uploadAtlasCs->dispatchThreads(nBricks * 1000u, 1, 1);

      const auto stageAll = RB_STAGE_COMPUTE | RB_STAGE_PIXEL | RB_STAGE_VERTEX;
      d3d::resource_barrier({atlasSph1.getVolTex(), RB_RO_SRV | RB_SOURCE_STAGE_COMPUTE | stageAll, 0, 0});

      ShaderGlobal::setBlock(prevFrameBlk, ShaderGlobal::LAYER_FRAME);
      ShaderGlobal::set_buffer(dagi_bake_brick_coordsVarId, BAD_D3DRESID);
      ShaderGlobal::set_buffer(dagi_bake_brick_probesVarId, BAD_D3DRESID);
    }

    ShaderGlobal::set_texture(dagi_irradiance_grid_sph0VarId, fallbackSph0);
    ShaderGlobal::set_texture(dagi_irradiance_grid_sph1VarId, atlasSph1);
    ShaderGlobal::set_buffer(dagi_baked_indirectionVarId, indirectionBuf);
    ShaderGlobal::set_int(dagi_baked_activeVarId, samplingEnabled ? 1 : 0);

    const Point3 bmin(cpuVolume.origin[0], cpuVolume.origin[1], cpuVolume.origin[2]);
    const float brickWorld = 8.f * cpuVolume.probeSize;
    const Point3 bmax = bmin + Point3(gx * brickWorld, gy * brickWorld, gz * brickWorld);
    worldBounds = BBox3(bmin, bmax);
    loaded = true;

    debug("[daGIBake] Loaded '%s' tier=%s bricks=%u/%u atlas=%ux%ux%u (%.2f MB) probeSize=%.2fm", loadedHeader.levelId.c_str(),
      dagi_bake::tier_name(loadedHeader.tier), nBricks, totalGridBricks, atlasTexels, atlasTexels, atlasTexels * 2u,
      double(uint64_t(atlasTexels) * atlasTexels * (atlasTexels * 2ull) * 8ull) / (1024.0 * 1024.0), cpuVolume.probeSize);
    return true;
  }
};

BakedGIVolume::BakedGIVolume() : impl(eastl::make_unique<Impl>()) {}
BakedGIVolume::~BakedGIVolume() { unload(); }

void BakedGIVolume::ensureFallbackBound()
{
  impl->initFallback();
  if (impl->loaded)
  {
    ShaderGlobal::set_texture(dagi_irradiance_grid_sph0VarId, impl->fallbackSph0);
    ShaderGlobal::set_texture(dagi_irradiance_grid_sph1VarId, impl->atlasSph1);
    ShaderGlobal::set_buffer(dagi_baked_indirectionVarId, impl->indirectionBuf);
    ShaderGlobal::set_int(dagi_baked_activeVarId, impl->samplingEnabled ? 1 : 0);
    impl->bindStandInTexturesWhenDaGIOff();
  }
  else
  {
    if (impl->fallbackSph0 && dagi_irradiance_grid_sph0VarId.get_var_id() >= 0 &&
        ShaderGlobal::get_tex(dagi_irradiance_grid_sph0VarId.get_var_id()) == BAD_TEXTUREID)
      ShaderGlobal::set_texture(dagi_irradiance_grid_sph0VarId, impl->fallbackSph0);
    if (impl->fallbackSph1 && dagi_irradiance_grid_sph1VarId.get_var_id() >= 0 &&
        ShaderGlobal::get_tex(dagi_irradiance_grid_sph1VarId.get_var_id()) == BAD_TEXTUREID)
      ShaderGlobal::set_texture(dagi_irradiance_grid_sph1VarId, impl->fallbackSph1);
    impl->bindStandInTexturesWhenDaGIOff();
  }
}

bool BakedGIVolume::loadFromFile(const char *path, uint64_t expected_content_hash, uint64_t expected_lighting_sig, bool verify_checksum)
{
  unload();
  std::string err;
  dagi_bake::Header hdr;
  dagi_bake::VolumeData vol;
  if (!dagi_bake::read_volume(path ? path : "", hdr, vol, verify_checksum, &err))
  {
    debug("[daGIBake] Cannot load '%s': %s", path ? path : "", err.c_str());
    ensureFallbackBound();
    return false;
  }
  if (hdr.producerVersion < CURRENT_BAKE_PRODUCER_VERSION)
  {
    debug("[daGIBake] Outdated producerVersion %u (expected >= %u) in '%s'", hdr.producerVersion, CURRENT_BAKE_PRODUCER_VERSION, path);
    ensureFallbackBound();
    return false;
  }
  if (expected_content_hash != 0 && hdr.contentHash != 0 && hdr.contentHash != expected_content_hash)
  {
    debug("[daGIBake] Content hash mismatch in '%s'", path);
    ensureFallbackBound();
    return false;
  }
  if (expected_lighting_sig != 0 && hdr.lightingSignature != 0 && hdr.lightingSignature != expected_lighting_sig)
  {
    debug("[daGIBake] Lighting signature mismatch in '%s'", path);
    ensureFallbackBound();
    return false;
  }

  impl->loadedHeader = std::move(hdr);
  impl->cpuVolume = std::move(vol);
  return impl->uploadVolumeToGpu();
}

void BakedGIVolume::unload()
{
  const bool wasLoaded = impl->loaded;
  impl->loaded = false;
  if (wasLoaded)
  {
    if (impl->fallbackSph0)
      ShaderGlobal::set_texture(dagi_irradiance_grid_sph0VarId, impl->fallbackSph0);
    if (impl->fallbackSph1)
      ShaderGlobal::set_texture(dagi_irradiance_grid_sph1VarId, impl->fallbackSph1);
  }
  impl->atlasSph0.close();
  impl->atlasSph1.close();
  impl->indirectionBuf.close();
  ShaderGlobal::set_int(dagi_baked_activeVarId, 0);
  ShaderGlobal::set_int4(dagi_baked_grid_dimVarId, 0, 0, 0, 0);
  if (impl->fallbackIndirection)
    ShaderGlobal::set_buffer(dagi_baked_indirectionVarId, impl->fallbackIndirection);
}

void BakedGIVolume::afterReset()
{
  if (impl->loaded)
    impl->uploadVolumeToGpu();
  else
    ensureFallbackBound();
}

bool BakedGIVolume::isLoaded() const { return impl->loaded; }
Tier BakedGIVolume::getLoadedTier() const { return impl->loaded ? from_cache_tier(impl->loadedHeader.tier) : Tier::None; }
uint32_t BakedGIVolume::getBrickCount() const { return impl->loaded ? uint32_t(impl->cpuVolume.bricks.size()) : 0u; }
uint32_t BakedGIVolume::getTotalGridBricks() const
{
  return impl->loaded ? impl->cpuVolume.gridDim[0] * impl->cpuVolume.gridDim[1] * impl->cpuVolume.gridDim[2] : 0u;
}
void BakedGIVolume::getGridDim(uint32_t &gx, uint32_t &gy, uint32_t &gz) const
{
  if (impl->loaded)
  {
    gx = impl->cpuVolume.gridDim[0];
    gy = impl->cpuVolume.gridDim[1];
    gz = impl->cpuVolume.gridDim[2];
  }
  else
    gx = gy = gz = 0u;
}
uint32_t BakedGIVolume::getAtlasTexels() const { return impl->loaded ? impl->atlasTexelsCount : 0u; }
float BakedGIVolume::getProbeSize() const { return impl->loaded ? impl->cpuVolume.probeSize : 0.f; }
const BBox3 &BakedGIVolume::getWorldBounds() const { return impl->worldBounds; }

void BakedGIVolume::setSamplingEnabled(bool enabled)
{
  impl->samplingEnabled = enabled;
  ShaderGlobal::set_int(dagi_baked_activeVarId, (impl->loaded && enabled) ? 1 : 0);
}

bool BakedGIVolume::isSamplingEnabled() const { return impl->samplingEnabled; }

bool BakedGIVolume::queryBrickAt(const Point3 &world_pos, int &bx, int &by, int &bz, uint16_t &brick_idx) const
{
  bx = by = bz = -1;
  brick_idx = dagi_bake::INVALID_BRICK_INDEX;
  if (!impl->loaded || impl->cpuVolume.probeSize <= 0.f)
    return false;
  const float brickWorld = 8.f * impl->cpuVolume.probeSize;
  const Point3 rel = (world_pos - Point3(impl->cpuVolume.origin[0], impl->cpuVolume.origin[1], impl->cpuVolume.origin[2])) / brickWorld;
  bx = int(floorf(rel.x));
  by = int(floorf(rel.y));
  bz = int(floorf(rel.z));
  const int gx = int(impl->cpuVolume.gridDim[0]);
  const int gy = int(impl->cpuVolume.gridDim[1]);
  const int gz = int(impl->cpuVolume.gridDim[2]);
  if (bx < 0 || by < 0 || bz < 0 || bx >= gx || by >= gy || bz >= gz)
    return false;
  const size_t linear = size_t(bx) + size_t(gx) * (size_t(by) + size_t(gy) * size_t(bz));
  if (linear < impl->cpuIndirectionTable.size())
    brick_idx = impl->cpuIndirectionTable[linear];
  return true;
}

void BakedGIVolume::drawDebugBricks(const Point3 &cam_pos, float radius) const
{
  if (!impl->loaded || impl->cpuVolume.bricks.empty())
    return;

  const float brickSize = 8.f * impl->cpuVolume.probeSize;
  const Point3 origin(impl->cpuVolume.origin[0], impl->cpuVolume.origin[1], impl->cpuVolume.origin[2]);
  const float maxDistSq = radius * radius;
  int camBx = -1, camBy = -1, camBz = -1;
  uint16_t camBrickIdx = dagi_bake::INVALID_BRICK_INDEX;
  queryBrickAt(cam_pos, camBx, camBy, camBz, camBrickIdx);

  // X-ray pass first so occluded bricks inside buildings remain visible through walls
  begin_draw_cached_debug_lines(false, false);
  set_cached_debug_lines_wtm(TMatrix::IDENT);
  uint32_t drawnXray = 0;
  for (const auto &b : impl->cpuVolume.bricks)
  {
    const Point3 bmin = origin + Point3(float(b.x), float(b.y), float(b.z)) * brickSize;
    const Point3 bcenter = bmin + Point3(0.5f, 0.5f, 0.5f) * brickSize;
    if (lengthSq(bcenter - cam_pos) > maxDistSq)
      continue;
    const bool isCamBrick = (int(b.x) == camBx && int(b.y) == camBy && int(b.z) == camBz);
    const E3DCOLOR col = isCamBrick ? E3DCOLOR(255, 220, 40, 100) : E3DCOLOR(40, 140, 210, 55);
    draw_cached_debug_box(BBox3(bmin, bmin + Point3(brickSize, brickSize, brickSize)), col);
    if (++drawnXray >= 256u)
      break;
  }
  end_draw_cached_debug_lines();

  begin_draw_cached_debug_lines(true, false);
  set_cached_debug_lines_wtm(TMatrix::IDENT);
  draw_cached_debug_box(impl->worldBounds, E3DCOLOR(80, 180, 255, 160));

  uint32_t drawn = 0;
  for (const auto &b : impl->cpuVolume.bricks)
  {
    const Point3 bmin = origin + Point3(float(b.x), float(b.y), float(b.z)) * brickSize;
    const Point3 bcenter = bmin + Point3(0.5f, 0.5f, 0.5f) * brickSize;
    if (lengthSq(bcenter - cam_pos) > maxDistSq)
      continue;
    const bool isCamBrick = (int(b.x) == camBx && int(b.y) == camBy && int(b.z) == camBz);
    const E3DCOLOR col = isCamBrick ? E3DCOLOR(255, 240, 40, 255) : E3DCOLOR(60, 235, 120, 190);
    draw_cached_debug_box(BBox3(bmin, bmin + Point3(brickSize, brickSize, brickSize)), col);
    if (++drawn >= 256u)
      break;
  }
  end_draw_cached_debug_lines();
}

void BakedGIVolume::drawDebugProbes(const Point3 &cam_pos, float radius, float sphere_scale)
{
  if (!impl->loaded || impl->cpuVolume.probeSize <= 0.f)
    return;
  if (!impl->debugProbesShader.shader)
    impl->debugProbesShader.init("dagi_baked_probes_debug", nullptr, 0, "dagi_baked_probes_debug", true);
  if (!impl->debugProbesShader.shader)
    return;

  const float probeSize = impl->cpuVolume.probeSize;
  const Point3 origin(impl->cpuVolume.origin[0], impl->cpuVolume.origin[1], impl->cpuVolume.origin[2]);
  const IPoint3 maxProbes(int(impl->cpuVolume.gridDim[0] * 8u), int(impl->cpuVolume.gridDim[1] * 8u),
    int(impl->cpuVolume.gridDim[2] * 8u));

  const int halfSpan = clamp(int(ceilf(radius / probeSize)), 4, 48);
  const IPoint3 centerProbe = ipoint3(floor((cam_pos - origin) / probeSize));
  const IPoint3 pmin = max(centerProbe - IPoint3(halfSpan, min(halfSpan, 16), halfSpan), IPoint3(0, 0, 0));
  const IPoint3 pmax = min(centerProbe + IPoint3(halfSpan, min(halfSpan, 16), halfSpan), maxProbes - IPoint3(1, 1, 1));
  if (pmin.x > pmax.x || pmin.y > pmax.y || pmin.z > pmax.z)
    return;

  IPoint3 span = pmax - pmin + IPoint3(1, 1, 1);
  int stride = 1;
  while ((uint64_t(span.x + stride - 1) / stride) * ((span.y + stride - 1) / stride) * ((span.z + stride - 1) / stride) > 16384ull)
    ++stride;
  const IPoint3 count((span.x + stride - 1) / stride, (span.y + stride - 1) / stride, (span.z + stride - 1) / stride);
  const uint32_t totalInstances = uint32_t(count.x) * uint32_t(count.y) * uint32_t(count.z);
  if (totalInstances == 0)
    return;

  ShaderGlobal::set_int4(dagi_baked_debug_box_minVarId, pmin.x, pmin.y, pmin.z, stride);
  ShaderGlobal::set_int4(dagi_baked_debug_box_dimVarId, count.x, count.y, count.z, 0);
  ShaderGlobal::set_float4(dagi_baked_debug_paramsVarId, clamp(sphere_scale, 0.02f, 0.45f), 0.f, 0.f, 0.f);

  impl->debugProbesShader.shader->setStates(0, true);
  d3d::setvsrc(0, 0, 0);
  d3d::draw_instanced(PRIM_TRILIST, 0, LOW_SPHERES_INDICES_TO_DRAW, totalInstances);
}

// ============================================================================
// Frame-Driven Non-Blocking GPU Baker
// ============================================================================

struct BakedGISession::Impl
{
  enum class Stage
  {
    Idle,
    TileRasterize,
    TileRelight,
    TileTraceAndExport,
    Finalize,
    Done,
    Failed
  };

  BakeParams params;
  Stage stage = Stage::Idle;
  int64_t startTimeRef = 0;
  double elapsedSec = 0.0;

  float probeSize = 2.0f;
  uint32_t numRays = 64;
  uint32_t maxTraceMeters = 64;
  uint32_t bounceRounds = 2;
  uint32_t sdfClips = 5;
  float sdfVoxel0 = 0.2f;

  uint32_t gridDim[3] = {0, 0, 0};
  uint32_t tileBricksXZ = 3;
  uint32_t tileBricksY = 3;
  uint32_t tilesX = 0, tilesY = 0, tilesZ = 0;
  uint32_t curTileIdx = 0;
  uint32_t rasterSubStep = 0;

  uint32_t relightStep = 0;
  uint32_t totalRelightSteps = 0;

  DaGISettings savedSettings;
  bool hasSavedSettings = false;

  dagi_bake::VolumeData outVol;
  std::vector<uint8_t> outProbeStates; // 0 = invalid inside wall, 1 = outdoor valid, 2 = indoor valid

  eastl::unique_ptr<ComputeShaderElement> seedRequestsCs;
  eastl::unique_ptr<ComputeShaderElement> classifyBricksCs;
  eastl::unique_ptr<ComputeShaderElement> traceBricksCs;

  UniqueBuf coordsBuf;
  UniqueBuf flagsBuf;
  UniqueBuf flagsReadback;
  UniqueBuf probesBuf;
  UniqueBuf probesReadback;

  static constexpr uint32_t MAX_TILE_CANDIDATE_BRICKS = 256;
  static constexpr uint32_t MAX_TRACE_BATCH_BRICKS = 64;

  bool initResources()
  {
    if (!seedRequestsCs)
      seedRequestsCs.reset(new_compute_shader("dagi_bake_seed_requests_cs"));
    if (!classifyBricksCs)
      classifyBricksCs.reset(new_compute_shader("dagi_bake_classify_bricks_cs"));
    if (!traceBricksCs)
      traceBricksCs.reset(new_compute_shader("dagi_bake_trace_bricks_cs"));
    if (!seedRequestsCs || !classifyBricksCs || !traceBricksCs)
    {
      logerr("daGIBake: missing bake compute shaders");
      return false;
    }

    coordsBuf = dag::create_sbuffer(sizeof(uint32_t), MAX_TILE_CANDIDATE_BRICKS * 4u, SBCF_BIND_SHADER_RES | SBCF_MISC_ALLOW_RAW, 0,
      "dagi_bake_coords", RESTAG_DAGI2);
    flagsBuf = dag::create_sbuffer(sizeof(uint32_t), MAX_TILE_CANDIDATE_BRICKS,
      SBCF_BIND_UNORDERED | SBCF_MISC_ALLOW_RAW | SBCF_BIND_SHADER_RES, 0, "dagi_bake_flags", RESTAG_DAGI2);
    flagsReadback = dag::create_sbuffer(sizeof(uint32_t), MAX_TILE_CANDIDATE_BRICKS, SBCF_USAGE_READ_BACK, 0,
      "dagi_bake_flags_rb", RESTAG_DAGI2);

    const uint32_t batchProbeDwords = MAX_TRACE_BATCH_BRICKS * dagi_bake::BRICK_PROBES_COUNT * 4u;
    probesBuf = dag::create_sbuffer(sizeof(uint32_t), batchProbeDwords,
      SBCF_BIND_UNORDERED | SBCF_MISC_ALLOW_RAW | SBCF_BIND_SHADER_RES, 0, "dagi_bake_probes", RESTAG_DAGI2);
    probesReadback =
      dag::create_sbuffer(sizeof(uint32_t), batchProbeDwords, SBCF_USAGE_READ_BACK, 0, "dagi_bake_probes_rb", RESTAG_DAGI2);

    return coordsBuf && flagsBuf && flagsReadback && probesBuf && probesReadback;
  }

  void releaseResources(DaGI *dagi)
  {
    coordsBuf.close();
    flagsBuf.close();
    flagsReadback.close();
    probesBuf.close();
    probesReadback.close();
    ShaderGlobal::set_buffer(dagi_bake_brick_coordsVarId, BAD_D3DRESID);
    ShaderGlobal::set_buffer(dagi_bake_brick_flagsVarId, BAD_D3DRESID);
    ShaderGlobal::set_buffer(dagi_bake_brick_probesVarId, BAD_D3DRESID);
    if (dagi && hasSavedSettings)
    {
      dagi->setSettings(savedSettings);
      hasSavedSettings = false;
    }
  }
};

BakedGISession::BakedGISession() : impl(eastl::make_unique<Impl>()) {}
BakedGISession::~BakedGISession() { cancel(); }

bool BakedGISession::start(const BakeParams &params, DaGI &dagi)
{
  cancel();
  if (params.worldBox.isempty())
    return false;

  impl->params = params;
  impl->startTimeRef = ref_time_ticks();
  impl->elapsedSec = 0.0;

  if (params.tier == Tier::High)
  {
    impl->probeSize = params.probeSizeOverride > 0.f ? params.probeSizeOverride : 0.5f;
    impl->numRays = 128;
    impl->maxTraceMeters = 96;
    impl->bounceRounds = 3;
    impl->sdfClips = 6;
    impl->sdfVoxel0 = 0.1f;
  }
  else
  {
    impl->probeSize = params.probeSizeOverride > 0.f ? params.probeSizeOverride : 1.0f;
    impl->numRays = 64;
    impl->maxTraceMeters = 64;
    impl->bounceRounds = 2;
    impl->sdfClips = 5;
    impl->sdfVoxel0 = 0.16f;
  }

  const float brickWorld = 8.f * impl->probeSize;
  Point3 bmin;
  bmin.x = floorf(params.worldBox[0].x / brickWorld) * brickWorld;
  bmin.y = floorf(params.worldBox[0].y / brickWorld) * brickWorld;
  bmin.z = floorf(params.worldBox[0].z / brickWorld) * brickWorld;

  Point3 bmax;
  bmax.x = ceilf(params.worldBox[1].x / brickWorld) * brickWorld;
  bmax.y = ceilf(params.worldBox[1].y / brickWorld) * brickWorld;
  bmax.z = ceilf(params.worldBox[1].z / brickWorld) * brickWorld;

  impl->gridDim[0] = max<uint32_t>(1u, uint32_t(roundf((bmax.x - bmin.x) / brickWorld)));
  impl->gridDim[1] = clamp<uint32_t>(uint32_t(roundf((bmax.y - bmin.y) / brickWorld)), 1u, 16u);
  impl->gridDim[2] = max<uint32_t>(1u, uint32_t(roundf((bmax.z - bmin.z) / brickWorld)));

  impl->tileBricksXZ = 3u;
  impl->tileBricksY = 3u;
  impl->tilesX = (impl->gridDim[0] + impl->tileBricksXZ - 1u) / impl->tileBricksXZ;
  impl->tilesY = (impl->gridDim[1] + impl->tileBricksY - 1u) / impl->tileBricksY;
  impl->tilesZ = (impl->gridDim[2] + impl->tileBricksXZ - 1u) / impl->tileBricksXZ;
  impl->curTileIdx = 0;
  impl->rasterSubStep = 0;

  impl->outVol = {};
  impl->outProbeStates.clear();
  impl->outVol.origin[0] = bmin.x;
  impl->outVol.origin[1] = bmin.y;
  impl->outVol.origin[2] = bmin.z;
  impl->outVol.probeSize = impl->probeSize;
  impl->outVol.gridDim[0] = impl->gridDim[0];
  impl->outVol.gridDim[1] = impl->gridDim[1];
  impl->outVol.gridDim[2] = impl->gridDim[2];

  if (!impl->initResources())
  {
    impl->stage = Impl::Stage::Failed;
    return false;
  }

  // Configure temporary high-capacity SurfaceCache + SDF for baking while disabling screen-space/temporal passes
  impl->savedSettings = dagi.getNextSettings();
  impl->hasSavedSettings = true;

  DaGISettings bakeSettings = impl->savedSettings;
  bakeSettings.sdf.texWidth = 128;
  bakeSettings.sdf.clips = uint8_t(impl->sdfClips);
  bakeSettings.sdf.voxel0Size = impl->sdfVoxel0;
  bakeSettings.sdf.yResScale = 1.0f;
  bakeSettings.sdf.temporalSpeed = 0.f;

  bakeSettings.albedoScene.clips = 3;
  bakeSettings.albedoScene.temporalSpeed = 0.f;

  bakeSettings.surfaceCache.entries = 1u << 20; // 1M entries (~16 MB table)
  bakeSettings.surfaceCache.cellSize = max(0.25f, impl->sdfVoxel0);
  bakeSettings.surfaceCache.relightThreads = 8192;

  // Minimal 8x8x8 radianceGrid satisfies DaGIImpl's (skyVisibility != radianceGrid) invariant and enables beforeFrameLit relights
  bakeSettings.skyVisibility.clipW = 0;
  bakeSettings.radianceGrid.w = 8;
  bakeSettings.radianceGrid.clips = 1;
  bakeSettings.radianceGrid.additionalIrradianceClips = 0;
  bakeSettings.radianceGrid.irradianceProbeDetail = 1.f;
  bakeSettings.radianceGrid.temporalSpeed = 0.f;

  bakeSettings.screenProbes.tileSize = 0;
  bakeSettings.volumetricGI.tileSize = 0;
  bakeSettings.mediaScene.clips = 0;

  dagi.setSettings(bakeSettings);
  dagi.invalidateAll();

  // Disable baked shader sampling while baking so Direct + SurfaceCache relighting sees clean state
  ShaderGlobal::set_int(dagi_baked_activeVarId, 0);
  ShaderGlobal::set_int4(dagi_baked_grid_dimVarId, 0, 0, 0, 0);

  impl->stage = Impl::Stage::TileRasterize;
  debug("[daGIBake] Started '%s' tier=%s grid=%ux%ux%u bricks (%ux%ux%u=%u tiles) probeSize=%.2fm", params.levelId.c_str(),
    tier_name(params.tier), impl->gridDim[0], impl->gridDim[1], impl->gridDim[2], impl->tilesX, impl->tilesY, impl->tilesZ,
    impl->tilesX * impl->tilesY * impl->tilesZ, impl->probeSize);
  return true;
}

bool BakedGISession::tick(DaGI &dagi, const RasterizeTileFn &rasterize_tile_cb, BakedGIVolume &out_volume)
{
  if (!isActive())
    return false;

  impl->elapsedSec = double(get_time_usec(impl->startTimeRef)) * 1e-6;
  const uint32_t totalTiles = impl->tilesX * impl->tilesY * impl->tilesZ;

  if (impl->curTileIdx >= totalTiles && impl->stage != Impl::Stage::Finalize)
    impl->stage = Impl::Stage::Finalize;

  const uint32_t txy = max(impl->tilesX * impl->tilesY, 1u);
  const uint32_t tx = impl->curTileIdx % max(impl->tilesX, 1u);
  const uint32_t ty = (impl->curTileIdx / max(impl->tilesX, 1u)) % max(impl->tilesY, 1u);
  const uint32_t tz = impl->curTileIdx / txy;
  const float brickWorld = 8.f * impl->probeSize;
  const uint32_t bx0 = tx * impl->tileBricksXZ;
  const uint32_t by0 = ty * impl->tileBricksY;
  const uint32_t bz0 = tz * impl->tileBricksXZ;
  const uint32_t bxEnd = min(bx0 + impl->tileBricksXZ, impl->gridDim[0]);
  const uint32_t byEnd = min(by0 + impl->tileBricksY, impl->gridDim[1]);
  const uint32_t bzEnd = min(bz0 + impl->tileBricksXZ, impl->gridDim[2]);

  const Point3 tileCenter(impl->outVol.origin[0] + 0.5f * float(bx0 + bxEnd) * brickWorld,
    impl->outVol.origin[1] + 0.5f * float(by0 + byEnd) * brickWorld,
    impl->outVol.origin[2] + 0.5f * float(bz0 + bzEnd) * brickWorld);

  if (impl->stage == Impl::Stage::TileRasterize)
  {
    d3d::GpuAutoLock gpuLock;
    // WorldSDF and AlbedoScene update 1 clip per updatePosition call by default; step across sdfClips frames
    // so every cascade from coarsest to finest is populated and seeded without stalling a single frame.
    rasterize_tile_cb(dagi, tileCenter);
    ++impl->rasterSubStep;
    if (impl->rasterSubStep < impl->sdfClips && !dagi.getWorldSDF().isValid())
      return true;
    if (impl->rasterSubStep < impl->sdfClips)
      return true;

    impl->rasterSubStep = 0;
    impl->relightStep = 0;
    const uint32_t activeClips = min<uint32_t>(dagi.getWorldSDF().getClipsCount(), 4u);
    constexpr uint32_t STRIDE = 8u;
    impl->totalRelightSteps = impl->bounceRounds * activeClips * STRIDE;
    impl->stage = Impl::Stage::TileRelight;
    return true;
  }

  if (impl->stage == Impl::Stage::TileRelight)
  {
    d3d::GpuAutoLock gpuLock;
    int sdfW = 128, sdfH = 64;
    dagi.getWorldSDF().getResolution(sdfW, sdfH);
    const uint32_t activeClips = min<uint32_t>(dagi.getWorldSDF().getClipsCount(), 4u);
    constexpr uint32_t STRIDE = 8u;
    const uint32_t totalClipVoxels = uint32_t(sdfW) * uint32_t(sdfH) * uint32_t(sdfW);
    const uint32_t threadsPerPhase = (totalClipVoxels + STRIDE - 1u) / STRIDE;

    // Run up to 16 relight sub-batches per frame (~8-15ms on 8cx Gen 3, well within WDDM TDR)
    constexpr uint32_t BATCHES_PER_TICK = 16u;
    for (uint32_t b = 0; b < BATCHES_PER_TICK && impl->relightStep < impl->totalRelightSteps; ++b, ++impl->relightStep)
    {
      const uint32_t clip = (impl->relightStep / STRIDE) % max(activeClips, 1u);
      const uint32_t phase = impl->relightStep % STRIDE;

      // Advance surface cache frame seed
      dagi.afterFrameRendered(DaGI::FrameData(0), false);

      ShaderGlobal::set_int4(dagi_bake_seed_paramsVarId, int(clip), int(STRIDE), int(phase), int(totalClipVoxels));
      impl->seedRequestsCs->dispatchThreads(threadsPerPhase, 1, 1);

      // Relights all queued surface cells (up to 8192) and clears request buffer
      dagi.beforeFrameLit(0.f);
    }

    if (impl->relightStep >= impl->totalRelightSteps)
      impl->stage = Impl::Stage::TileTraceAndExport;
    return true;
  }

  if (impl->stage == Impl::Stage::TileTraceAndExport)
  {
    d3d::GpuAutoLock gpuLock;
    ShaderGlobal::set_float4(dagi_baked_grid_originVarId, impl->outVol.origin[0], impl->outVol.origin[1], impl->outVol.origin[2],
      1.f / impl->probeSize);
    ShaderGlobal::set_float4(dagi_baked_atlas_invVarId, 0.f, 0.f, 0.f, impl->probeSize);

    std::vector<dagi_bake::BrickCoord> candidates;
    candidates.reserve((bxEnd - bx0) * (byEnd - by0) * (bzEnd - bz0));
    for (uint32_t bz = bz0; bz < bzEnd; ++bz)
      for (uint32_t by = by0; by < byEnd; ++by)
        for (uint32_t bx = bx0; bx < bxEnd; ++bx)
          candidates.push_back({uint16_t(bx), uint16_t(by), uint16_t(bz), 0u});

    std::vector<dagi_bake::BrickCoord> activeBricks;
    for (size_t base = 0; base < candidates.size(); base += Impl::MAX_TILE_CANDIDATE_BRICKS)
    {
      const uint32_t count = uint32_t(min<size_t>(candidates.size() - base, Impl::MAX_TILE_CANDIDATE_BRICKS));
      std::vector<uint32_t> coordWords(count * 4u, 0u);
      for (uint32_t i = 0; i < count; ++i)
      {
        coordWords[i * 4u + 0u] = candidates[base + i].x;
        coordWords[i * 4u + 1u] = candidates[base + i].y;
        coordWords[i * 4u + 2u] = candidates[base + i].z;
      }
      impl->coordsBuf->updateData(0, count * 4u * sizeof(uint32_t), coordWords.data(), VBLOCK_WRITEONLY);
      ShaderGlobal::set_buffer(dagi_bake_brick_coordsVarId, impl->coordsBuf);
      ShaderGlobal::set_buffer(dagi_bake_brick_flagsVarId, impl->flagsBuf);
      ShaderGlobal::set_int4(dagi_bake_trace_paramsVarId, int(count), int(impl->numRays), int(impl->maxTraceMeters), 0);

      d3d::resource_barrier({impl->flagsBuf.getBuf(), RB_RW_UAV | RB_STAGE_COMPUTE});
      impl->classifyBricksCs->dispatchThreads(count, 1, 1);
      d3d::resource_barrier({impl->flagsBuf.getBuf(), RB_RO_COPY_SOURCE | RB_SOURCE_STAGE_COMPUTE});
      impl->flagsBuf.getBuf()->copyTo(impl->flagsReadback.getBuf(), 0, 0, count * sizeof(uint32_t));

      if (auto lockedFlags = lock_sbuffer<const uint32_t>(impl->flagsReadback.getBuf(), 0, count, VBLOCK_READONLY))
      {
        for (uint32_t i = 0; i < count; ++i)
          if (lockedFlags[i] != 0u)
            activeBricks.push_back(candidates[base + i]);
      }
    }

    // Trace active bricks in bounded batches (<= 64 bricks = 32K probes per dispatch)
    for (size_t base = 0; base < activeBricks.size(); base += Impl::MAX_TRACE_BATCH_BRICKS)
    {
      const uint32_t batchBricks = uint32_t(min<size_t>(activeBricks.size() - base, Impl::MAX_TRACE_BATCH_BRICKS));
      std::vector<uint32_t> coordWords(batchBricks * 4u, 0u);
      for (uint32_t i = 0; i < batchBricks; ++i)
      {
        coordWords[i * 4u + 0u] = activeBricks[base + i].x;
        coordWords[i * 4u + 1u] = activeBricks[base + i].y;
        coordWords[i * 4u + 2u] = activeBricks[base + i].z;
      }
      impl->coordsBuf->updateData(0, batchBricks * 4u * sizeof(uint32_t), coordWords.data(), VBLOCK_WRITEONLY);
      ShaderGlobal::set_buffer(dagi_bake_brick_coordsVarId, impl->coordsBuf);
      ShaderGlobal::set_buffer(dagi_bake_brick_probesVarId, impl->probesBuf);
      ShaderGlobal::set_int4(dagi_bake_trace_paramsVarId, int(batchBricks), int(impl->numRays), int(impl->maxTraceMeters), 0);

      d3d::resource_barrier({impl->probesBuf.getBuf(), RB_RW_UAV | RB_STAGE_COMPUTE});
      // 1 group of 64 threads per probe -> batchBricks * 512 groups
      impl->traceBricksCs->dispatch(batchBricks * dagi_bake::BRICK_PROBES_COUNT, 1, 1);

      const uint32_t probeDwords = batchBricks * dagi_bake::BRICK_PROBES_COUNT * 4u;
      d3d::resource_barrier({impl->probesBuf.getBuf(), RB_RO_COPY_SOURCE | RB_SOURCE_STAGE_COMPUTE});
      impl->probesBuf.getBuf()->copyTo(impl->probesReadback.getBuf(), 0, 0, probeDwords * sizeof(uint32_t));

      if (auto lockedProbes = lock_sbuffer<const uint32_t>(impl->probesReadback.getBuf(), 0, probeDwords, VBLOCK_READONLY))
      {
        for (uint32_t b = 0; b < batchBricks; ++b)
        {
          if (impl->outVol.bricks.size() >= dagi_bake::INVALID_BRICK_INDEX)
            break;
          const dagi_bake::BrickCoord bc = activeBricks[base + b];
          impl->outVol.bricks.push_back(bc);
          for (uint32_t p = 0; p < dagi_bake::BRICK_PROBES_COUNT; ++p)
          {
            const uint32_t ofs = (b * dagi_bake::BRICK_PROBES_COUNT + p) * 4u;
            const uint32_t s0 = lockedProbes[ofs + 0u];
            const uint64_t s1 = uint64_t(lockedProbes[ofs + 1u]) | (uint64_t(lockedProbes[ofs + 2u]) << 32u);
            uint8_t state = uint8_t(lockedProbes[ofs + 3u] & 0xFFu);
            if (state == 1u && !impl->params.indoorInvTms.empty())
            {
              const uint32_t lx = p & 7u;
              const uint32_t ly = (p >> 3u) & 7u;
              const uint32_t lz = p >> 6u;
              const Point3 probeWorld(impl->outVol.origin[0] + (float(bc.x * 8u + lx) + 0.5f) * impl->probeSize,
                impl->outVol.origin[1] + (float(bc.y * 8u + ly) + 0.5f) * impl->probeSize,
                impl->outVol.origin[2] + (float(bc.z * 8u + lz) + 0.5f) * impl->probeSize);
              for (const TMatrix &invTm : impl->params.indoorInvTms)
              {
                const Point3 loc = invTm * probeWorld;
                if (fabsf(loc.x) < 0.5f && fabsf(loc.y) < 0.5f && fabsf(loc.z) < 0.5f)
                {
                  state = 2u;
                  break;
                }
              }
            }
            impl->outVol.sph0.push_back(s0);
            impl->outVol.sph1.push_back(s1);
            impl->outProbeStates.push_back(state);
          }
        }
      }
    }

    ++impl->curTileIdx;
    impl->rasterSubStep = 0;
    if (impl->curTileIdx % 4u == 0u || impl->curTileIdx == totalTiles)
    {
      debug("[daGIBake] '%s' (%s): tile %u/%u (%.0f%%) activeBricks=%u elapsed=%.1fs", impl->params.levelId.c_str(),
        tier_name(impl->params.tier), impl->curTileIdx, totalTiles, 100.0 * double(impl->curTileIdx) / max(totalTiles, 1u),
        uint32_t(impl->outVol.bricks.size()), impl->elapsedSec);
    }

    impl->stage = (impl->curTileIdx < totalTiles) ? Impl::Stage::TileRasterize : Impl::Stage::Finalize;
    return true;
  }

  if (impl->stage == Impl::Stage::Finalize)
  {
    impl->releaseResources(&dagi);
    dagi.invalidateAll();

    // Bakery-inspired 3-pass 26-neighbor hole-filling dilation with indoor/outdoor isolation barrier
    if (!impl->outVol.bricks.empty() && impl->outProbeStates.size() == impl->outVol.sph0.size())
    {
      const uint32_t gx = impl->outVol.gridDim[0];
      const uint32_t gy = impl->outVol.gridDim[1];
      const uint32_t gz = impl->outVol.gridDim[2];
      const size_t totalGridBricks = size_t(gx) * gy * gz;
      std::vector<uint16_t> brickGrid(totalGridBricks, dagi_bake::INVALID_BRICK_INDEX);
      for (uint32_t i = 0; i < uint32_t(impl->outVol.bricks.size()); ++i)
      {
        const auto &b = impl->outVol.bricks[i];
        const size_t linear = size_t(b.x) + size_t(gx) * (size_t(b.y) + size_t(gy) * size_t(b.z));
        if (linear < totalGridBricks)
          brickGrid[linear] = uint16_t(i);
      }

      auto lookupProbe = [&](int px, int py, int pz) -> int64_t {
        if (px < 0 || py < 0 || pz < 0 || px >= int(gx * 8u) || py >= int(gy * 8u) || pz >= int(gz * 8u))
          return -1;
        const size_t bLin = size_t(px >> 3) + size_t(gx) * (size_t(py >> 3) + size_t(gy) * size_t(pz >> 3));
        const uint16_t bIdx = brickGrid[bLin];
        if (bIdx == dagi_bake::INVALID_BRICK_INDEX)
          return -1;
        const uint32_t pLin = uint32_t(px & 7) + 8u * (uint32_t(py & 7) + 8u * uint32_t(pz & 7));
        return int64_t(bIdx) * 512ll + int64_t(pLin);
      };

      uint32_t totalFilled = 0u;
      for (int pass = 0; pass < 3; ++pass)
      {
        std::vector<uint32_t> nextSph0 = impl->outVol.sph0;
        std::vector<uint64_t> nextSph1 = impl->outVol.sph1;
        std::vector<uint8_t> nextStates = impl->outProbeStates;
        uint32_t passFilled = 0u;

        for (uint32_t bi = 0; bi < uint32_t(impl->outVol.bricks.size()); ++bi)
        {
          const auto &bc = impl->outVol.bricks[bi];
          for (uint32_t lz = 0; lz < 8u; ++lz)
          {
            for (uint32_t ly = 0; ly < 8u; ++ly)
            {
              for (uint32_t lx = 0; lx < 8u; ++lx)
              {
                const size_t idx = size_t(bi) * 512u + size_t(lx + 8u * (ly + 8u * lz));
                if (impl->outProbeStates[idx] != 0u)
                  continue;

                const int gx0 = int(bc.x * 8u + lx);
                const int gy0 = int(bc.y * 8u + ly);
                const int gz0 = int(bc.z * 8u + lz);

                bool hasIndoorNb = false;
                for (int dz = -1; dz <= 1 && !hasIndoorNb; ++dz)
                  for (int dy = -1; dy <= 1 && !hasIndoorNb; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                      if (dx == 0 && dy == 0 && dz == 0)
                        continue;
                      const int64_t nb = lookupProbe(gx0 + dx, gy0 + dy, gz0 + dz);
                      if (nb >= 0 && impl->outProbeStates[size_t(nb)] == 2u)
                      {
                        hasIndoorNb = true;
                        break;
                      }
                    }

                const uint8_t targetState = hasIndoorNb ? 2u : 1u;
                float accS0[4] = {};
                float accS1[4] = {};
                float wSum = 0.f;
                uint32_t count = 0u;

                for (int dz = -1; dz <= 1; ++dz)
                  for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                      if (dx == 0 && dy == 0 && dz == 0)
                        continue;
                      const int64_t nb = lookupProbe(gx0 + dx, gy0 + dy, gz0 + dz);
                      if (nb < 0 || impl->outProbeStates[size_t(nb)] != targetState)
                        continue;

                      const uint32_t ns0 = impl->outVol.sph0[size_t(nb)];
                      const uint64_t ns1 = impl->outVol.sph1[size_t(nb)];
                      const float nSum = max(half_to_float(uint16_t(ns1 & 0xFFFFull)), 0.f);
                      const float w = max(nSum, 1e-4f);
                      accS0[0] += (float(ns0 & 0xFFu) / 255.0f) * w;
                      accS0[1] += (float((ns0 >> 8u) & 0xFFu) / 255.0f) * w;
                      accS0[2] += (float((ns0 >> 16u) & 0xFFu) / 255.0f) * w;
                      accS0[3] += (float(ns0 >> 24u) / 255.0f) * w;
                      accS1[0] += nSum;
                      accS1[1] += half_to_float(uint16_t((ns1 >> 16u) & 0xFFFFull));
                      accS1[2] += half_to_float(uint16_t((ns1 >> 32u) & 0xFFFFull));
                      accS1[3] += half_to_float(uint16_t(ns1 >> 48u));
                      wSum += w;
                      ++count;
                    }

                if (count > 0u)
                {
                  const float invC = 1.0f / float(count);
                  const float invW = 1.0f / max(wSum, 1e-4f);
                  const uint32_t p0 = uint32_t(clamp(accS0[0] * invW, 0.f, 1.f) * 255.0f + 0.5f);
                  const uint32_t p1 = uint32_t(clamp(accS0[1] * invW, 0.f, 1.f) * 255.0f + 0.5f);
                  const uint32_t p2 = uint32_t(clamp(accS0[2] * invW, 0.f, 1.f) * 255.0f + 0.5f);
                  const uint32_t p3 = uint32_t(clamp(accS0[3] * invW, 0.f, 1.f) * 255.0f + 0.5f);
                  nextSph0[idx] = p0 | (p1 << 8u) | (p2 << 16u) | (p3 << 24u);
                  const uint64_t h0 = float_to_half(accS1[0] * invC);
                  const uint64_t h1 = float_to_half(accS1[1] * invC);
                  const uint64_t h2 = float_to_half(accS1[2] * invC);
                  const uint64_t h3 = float_to_half(accS1[3] * invC);
                  nextSph1[idx] = h0 | (h1 << 16u) | (h2 << 32u) | (h3 << 48u);
                  nextStates[idx] = targetState;
                  ++passFilled;
                }
              }
            }
          }
        }

        impl->outVol.sph0.swap(nextSph0);
        impl->outVol.sph1.swap(nextSph1);
        impl->outProbeStates.swap(nextStates);
        totalFilled += passFilled;
        if (passFilled == 0u)
          break;
      }
      debug("[daGIBake] Dilation filled %u invalid probes across '%s'", totalFilled, impl->params.levelId.c_str());
    }

    dagi_bake::Header hdr;
    hdr.producerVersion = CURRENT_BAKE_PRODUCER_VERSION;
    hdr.tier = to_cache_tier(impl->params.tier);
    hdr.levelId = impl->params.levelId.c_str();
    hdr.contentHash = impl->params.contentHash;
    hdr.lightingSignature = impl->params.lightingSignature;
    hdr.stateCount = 1;
    hdr.brickSize = dagi_bake::BRICK_PROBES_SIDE;
    hdr.brickCount = uint32_t(impl->outVol.bricks.size());

    std::string err;
    if (!dagi_bake::write_volume(impl->params.outputPath.c_str(), hdr, impl->outVol, &err))
    {
      logerr("daGIBake: failed to write '%s': %s", impl->params.outputPath.c_str(), err.c_str());
      impl->stage = Impl::Stage::Failed;
      return false;
    }

    impl->elapsedSec = double(get_time_usec(impl->startTimeRef)) * 1e-6;
    debug("[daGIBake] Completed '%s' -> '%s' in %.2fs (bricks=%u, probes=%u, size=%.2f MB)", impl->params.levelId.c_str(),
      impl->params.outputPath.c_str(), impl->elapsedSec, uint32_t(impl->outVol.bricks.size()),
      uint32_t(impl->outVol.bricks.size()) * dagi_bake::BRICK_PROBES_COUNT,
      double(uint64_t(impl->outVol.bricks.size()) * dagi_bake::BRICK_RECORD_BYTES) / (1024.0 * 1024.0));

    out_volume.unload();
    out_volume.impl->loadedHeader = std::move(hdr);
    out_volume.impl->cpuVolume = std::move(impl->outVol);
    if (!out_volume.impl->uploadVolumeToGpu())
    {
      impl->stage = Impl::Stage::Failed;
      return false;
    }

    impl->stage = Impl::Stage::Done;
    return false;
  }

  return false;
}

void BakedGISession::cancel()
{
  impl->releaseResources(nullptr);
  impl->stage = Impl::Stage::Idle;
}

bool BakedGISession::isActive() const
{
  return impl->stage == Impl::Stage::TileRasterize || impl->stage == Impl::Stage::TileRelight ||
         impl->stage == Impl::Stage::TileTraceAndExport || impl->stage == Impl::Stage::Finalize;
}

bool BakedGISession::succeeded() const { return impl->stage == Impl::Stage::Done; }

float BakedGISession::getProgress() const
{
  if (impl->stage == Impl::Stage::Done)
    return 1.f;
  const uint32_t totalTiles = max(impl->tilesX * impl->tilesY * impl->tilesZ, 1u);
  float sub = 0.f;
  if (impl->stage == Impl::Stage::TileRasterize && impl->sdfClips > 0)
    sub = 0.25f * float(impl->rasterSubStep) / float(impl->sdfClips);
  else if (impl->stage == Impl::Stage::TileRelight && impl->totalRelightSteps > 0)
    sub = 0.25f + 0.55f * float(impl->relightStep) / float(impl->totalRelightSteps);
  else if (impl->stage == Impl::Stage::TileTraceAndExport)
    sub = 0.85f;
  return clamp((float(impl->curTileIdx) + sub) / float(totalTiles), 0.f, 0.99f);
}

double BakedGISession::getElapsedSeconds() const { return impl->elapsedSec; }

const char *BakedGISession::getStageName() const
{
  switch (impl->stage)
  {
    case Impl::Stage::TileRasterize: return "SDF/Albedo Rasterize";
    case Impl::Stage::TileRelight: return "SurfaceCache Multi-Bounce";
    case Impl::Stage::TileTraceAndExport: return "Probe SH1 Trace & Pack";
    case Impl::Stage::Finalize: return "Write .dgibake & Upload Atlas";
    case Impl::Stage::Done: return "Done";
    case Impl::Stage::Failed: return "Failed";
    default: return "Idle";
  }
}

uint32_t BakedGISession::getCurrentTile() const { return impl->curTileIdx; }
uint32_t BakedGISession::getTotalTiles() const { return max(impl->tilesX * impl->tilesY * impl->tilesZ, 1u); }
uint32_t BakedGISession::getBakedBrickCount() const { return uint32_t(impl->outVol.bricks.size()); }
Tier BakedGISession::getTargetTier() const { return impl->params.tier; }
const char *BakedGISession::getOutputPath() const { return impl->params.outputPath.c_str(); }
} // namespace dagi_bake_runtime
