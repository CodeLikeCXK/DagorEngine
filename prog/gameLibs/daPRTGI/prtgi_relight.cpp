//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_relight.h"
#include <shaders/dag_shaders.h>
#include <math/integer/dag_IPoint4.h>
#include <perfMon/dag_statDrv.h>
#include <debug/dag_debug.h>

namespace daprtgi
{

void PRTRelightPass::init()
{
  brickRelightCS = new_compute_shader("prt_brick_relight_cs", true);
  probeRelightCS = new_compute_shader("prt_probe_relight_cs", true);
  publishCS = new_compute_shader("prt_publish_cs", true);

  prtGridOriginVarId = get_shader_variable_id("_prtGridOrigin", true);
  prtGridMinVarId = get_shader_variable_id("_prtGridMin", true);
  prtGridCountVarId = get_shader_variable_id("_prtGridCount", true);
  prtSlotCountVarId = get_shader_variable_id("_prtSlotCount", true);
  prtGridSpacingVarId = get_shader_variable_id("_prtGridSpacing", true);
  prtCascadeCountVarId = get_shader_variable_id("_prtCascadeCount", true);
  prtVolumeEnabledVarId = get_shader_variable_id("_prtVolumeEnabled", true);
  prtCoefficientVoxel3DVarId = get_shader_variable_id("_coefficientVoxel3D", true);

  debug("PRTGI Relight Init: brickRelightCS=%p, probeRelightCS=%p, volEnabledId=%d, voxel3DId=%d",
    brickRelightCS, probeRelightCS, prtVolumeEnabledVarId, prtCoefficientVoxel3DVarId);
}

void PRTRelightPass::close()
{
  del_it(brickRelightCS);
  del_it(probeRelightCS);
  del_it(publishCS);
}

void PRTRelightPass::unbindShaderResources()
{
  static const char *const bufferVars[] = {"_prtProbeInfo", "_prtProbeDepth", "_prtProbeData", "_factors",
    "_prtSkyVisibility", "_brickInfo", "_surfels", "_brickRadiance", "_prtCommittedSH"};
  for (const char *name : bufferVars)
  {
    const int varId = get_shader_variable_id(name, true);
    if (varId >= 0)
      ShaderGlobal::set_buffer_unsafe(varId, nullptr);
  }

  static const char *const textureVars[] = {"_coefficientVoxel3D", "_coefficientVoxel3DRW"};
  for (const char *name : textureVars)
  {
    const int varId = get_shader_variable_id(name, true);
    if (varId >= 0)
      ShaderGlobal::set_texture(varId, BAD_TEXTUREID);
  }

  if (prtVolumeEnabledVarId >= 0)
    ShaderGlobal::set_int(prtVolumeEnabledVarId, 0);
}

void PRTRelightPass::relightSectors(const eastl::vector<int> &selected_sectors, PRTAsset &asset,
                                   PRTSectorResidency &residency, const Point3 &camera_pos,
                                   float sky_intensity)
{
  G_UNUSED(camera_pos);
  if (selected_sectors.empty())
    return;

  TIME_D3D_PROFILE(prtgi_relight);

  static int prtSkyIntensityVarId = get_shader_variable_id("_prtEnvironmentIntensity", true);
  if (prtSkyIntensityVarId >= 0)
    ShaderGlobal::set_float(prtSkyIntensityVarId, sky_intensity);

  if (prtGridMinVarId >= 0)
    ShaderGlobal::set_int4(prtGridMinVarId, IPoint4(asset.grid.minCoord.x, asset.grid.minCoord.y, asset.grid.minCoord.z, 0));
  if (prtGridCountVarId >= 0)
    ShaderGlobal::set_int4(prtGridCountVarId, IPoint4(asset.grid.count.x, asset.grid.count.y, asset.grid.count.z, 0));
  if (prtSlotCountVarId >= 0)
    ShaderGlobal::set_int4(prtSlotCountVarId, IPoint4(residency.getSlotCount().x, residency.getSlotCount().y, residency.getSlotCount().z, 0));

  static int coefficientVoxel3DRWVarId = get_shader_variable_id("_coefficientVoxel3DRW", true);
  if (coefficientVoxel3DRWVarId >= 0 && residency.getCoefficientVoxel3D())
    ShaderGlobal::set_texture(coefficientVoxel3DRWVarId, residency.getCoefficientVoxel3DId());

  static int brickRadianceVarId = get_shader_variable_id("_brickRadiance", true);

  for (int secIdx : selected_sectors)
  {
    if (secIdx < 0 || secIdx >= (int)asset.sectors.size())
      continue;

    const PRTSectorData &sector = asset.sectors[secIdx];
    SectorGPUData *gpuData = residency.getOrUploadSector(sector);
    if (!gpuData)
      continue;

    // 1. Dispatch BrickRelight CS
    if (brickRelightCS && gpuData->brickCount > 0)
    {
      static int brickCountVarId = get_shader_variable_id("_brickCount", true);
      static int surfelBoundsMinVarId = get_shader_variable_id("_surfelBoundsMin", true);
      static int surfelBoundsSizeVarId = get_shader_variable_id("_surfelBoundsSize", true);

      if (brickCountVarId >= 0) ShaderGlobal::set_int(brickCountVarId, (int)gpuData->brickCount);
      if (surfelBoundsMinVarId >= 0) ShaderGlobal::set_float4(surfelBoundsMinVarId, sector.surfelBoundsMin.x, sector.surfelBoundsMin.y, sector.surfelBoundsMin.z, 0);
      if (surfelBoundsSizeVarId >= 0) ShaderGlobal::set_float4(surfelBoundsSizeVarId, sector.surfelBoundsSize.x, sector.surfelBoundsSize.y, sector.surfelBoundsSize.z, 0);

      static int brickInfoVarId = get_shader_variable_id("_brickInfo", true);
      static int surfelsVarId = get_shader_variable_id("_surfels", true);

      if (brickInfoVarId >= 0) ShaderGlobal::set_buffer_unsafe(brickInfoVarId, gpuData->brickInfoBuf);
      if (surfelsVarId >= 0) ShaderGlobal::set_buffer_unsafe(surfelsVarId, gpuData->surfelsBuf);
      if (brickRadianceVarId >= 0) ShaderGlobal::set_buffer_unsafe(brickRadianceVarId, gpuData->brickRadianceBuf);

      uint32_t gx = min<uint32_t>(gpuData->brickCount, 65535u);
      uint32_t gy = (gpuData->brickCount + 65534u) / 65535u;
      brickRelightCS->dispatch(gx, gy, 1);
      if (brickRadianceVarId >= 0) ShaderGlobal::set_buffer_unsafe(brickRadianceVarId, nullptr);
    }

    // 2. Dispatch ProbeRelight CS
    if (probeRelightCS && gpuData->probeCount > 0)
    {
      static int prtProbeCountVarId = get_shader_variable_id("_prtProbeCount", true);
      if (prtProbeCountVarId >= 0) ShaderGlobal::set_int(prtProbeCountVarId, (int)gpuData->probeCount);

      static int prtProbeDataVarId = get_shader_variable_id("_prtProbeData", true);
      static int factorsVarId = get_shader_variable_id("_factors", true);
      static int prtSkyVisibilityVarId = get_shader_variable_id("_prtSkyVisibility", true);
      static int prtCommittedSHVarId = get_shader_variable_id("_prtCommittedSH", true);

      if (prtProbeDataVarId >= 0) ShaderGlobal::set_buffer_unsafe(prtProbeDataVarId, gpuData->probeDataBuf);
      if (factorsVarId >= 0) ShaderGlobal::set_buffer_unsafe(factorsVarId, gpuData->factorsBuf);
      if (prtSkyVisibilityVarId >= 0) ShaderGlobal::set_buffer_unsafe(prtSkyVisibilityVarId, gpuData->skyVisBuf);
      if (brickRadianceVarId >= 0) ShaderGlobal::set_buffer_unsafe(brickRadianceVarId, gpuData->brickRadianceBuf);
      if (prtCommittedSHVarId >= 0) ShaderGlobal::set_buffer_unsafe(prtCommittedSHVarId, gpuData->probeSHBuf);

      probeRelightCS->dispatchThreads(gpuData->probeCount, 1, 1);
      if (prtCommittedSHVarId >= 0) ShaderGlobal::set_buffer_unsafe(prtCommittedSHVarId, nullptr);
      if (brickRadianceVarId >= 0) ShaderGlobal::set_buffer_unsafe(brickRadianceVarId, nullptr);
    }
  }

  // Unbind UAV to prevent D3D11 runtime hazard resolution and pipeline flush
  if (coefficientVoxel3DRWVarId >= 0)
    ShaderGlobal::set_texture(coefficientVoxel3DRWVarId, BAD_TEXTUREID);
}

void PRTRelightPass::updateGlobalShaderConstants(const PRTGridDesc &grid, const PRTSectorResidency &residency,
                                                const Point3 &camera_pos, int cascade_count)
{
  G_UNUSED(camera_pos);
  if (prtGridOriginVarId >= 0)
    ShaderGlobal::set_float4(prtGridOriginVarId, grid.origin.x, grid.origin.y, grid.origin.z, 0);

  if (prtGridMinVarId >= 0)
    ShaderGlobal::set_int4(prtGridMinVarId, IPoint4(grid.minCoord.x, grid.minCoord.y, grid.minCoord.z, 0));

  if (prtGridCountVarId >= 0)
    ShaderGlobal::set_int4(prtGridCountVarId, IPoint4(grid.count.x, grid.count.y, grid.count.z, 0));

  if (prtSlotCountVarId >= 0)
    ShaderGlobal::set_int4(prtSlotCountVarId, IPoint4(residency.getSlotCount().x, residency.getSlotCount().y, residency.getSlotCount().z, 0));

  if (prtGridSpacingVarId >= 0)
    ShaderGlobal::set_float(prtGridSpacingVarId, grid.spacing);

  if (prtCascadeCountVarId >= 0)
    ShaderGlobal::set_int(prtCascadeCountVarId, cascade_count);

  if (prtVolumeEnabledVarId >= 0)
    ShaderGlobal::set_int(prtVolumeEnabledVarId, residency.getCoefficientVoxel3D() ? 1 : 0);

  if (prtCoefficientVoxel3DVarId >= 0)
    ShaderGlobal::set_texture(prtCoefficientVoxel3DVarId, residency.getCoefficientVoxel3DId());

  static int prtProbeInfoVarId = get_shader_variable_id("_prtProbeInfo", true);
  if (prtProbeInfoVarId >= 0)
    ShaderGlobal::set_buffer_unsafe(prtProbeInfoVarId, residency.getProbeInfoBuf());

  static int prtProbeDepthVarId = get_shader_variable_id("_prtProbeDepth", true);
  if (prtProbeDepthVarId >= 0)
    ShaderGlobal::set_buffer_unsafe(prtProbeDepthVarId, residency.getProbeDepthBuf());
}

} // namespace daprtgi
