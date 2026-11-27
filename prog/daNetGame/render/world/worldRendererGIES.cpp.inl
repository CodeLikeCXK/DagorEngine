// Copyright (C) Gaijin Games KFT.  All rights reserved.

#define INSIDE_RENDERER 1 // fixme: move to jam

#include <daECS/core/componentTypes.h>
#include <daECS/core/coreEvents.h>
#include <daECS/core/entityManager.h>
#include <daECS/core/entitySystem.h>
#include <ecs/phys/physEvents.h>

#include <memory/dag_framemem.h>
#include <gameRes/dag_collisionResource.h>
#include <gamePhys/collision/collisionLib.h>
#include <rendInst/visibility.h>
#include <util/dag_convar.h>
#include "private_worldRenderer.h"
#include <ioSys/dag_dataBlock.h>
#include <osApiWrappers/dag_direct.h>
#include <render/voxelization_target.h>
#include <daGI2/daGI2.h>
#include <daSDF/worldSDF.h>
#include <daGI2/lruCollisionVoxelization.h>
#include <shaders/dag_shaderBlock.h>
#include <startup/dag_globalSettings.h>
#include <perfMon/dag_statDrv.h>
#include <render/deferredRenderer.h>
#include <math/dag_TMatrix4.h>
#include <math/dag_cube_matrix.h>
#include <util/dag_hash.h>
#include <render/lruCollision.h>
#include <rendInst/rendInstCollision.h>
#include "global_vars.h"
#include <scene/dag_tiledScene.h>
#include <math/dag_math3d.h>
#include <util/dag_console.h>
#include <util/dag_stlqsort.h>
#include <gamePhys/phys/rendinstDestr.h>
#include <debug/dag_debug3d.h>
#include <math/dag_vecMathCompatibility.h>
#include <rendInst/rendInstExtra.h>
#include <rendInst/rendInstGenRender.h>
#include "main/level.h"
#include "main/gameLoad.h"

#include "frameGraphNodes/frameGraphNodes.h"
#include "render/world/gbufferConsts.h"
#include <drv/3d/dag_rwResource.h>
#include <drv/3d/dag_viewScissor.h>
#include <drv/3d/dag_renderTarget.h>
#include <drv/3d/dag_buffers.h>
#include <drv/3d/dag_shaderConstants.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <GIWindows/GIWindows.h>
#include <render/world/bvh.h>
#include <render/renderEvent.h>
#include <ecs/render/updateStageRender.h>
#include <gui/dag_stdGuiRender.h>
#include <gui/dag_imgui.h>
#include <imgui/imgui.h>
#include <cstdio>


CONSOLE_FLOAT_VAL_MINMAX("render", world_sdf_from_collision_rasterize_below, 3, -1, 16);
CONSOLE_FLOAT_VAL_MINMAX("render", world_sdf_rasterize_supersample, 1, 1, 4);
CONSOLE_BOOL_VAL("render", rasterize_sdf_prims, true);
CONSOLE_BOOL_VAL("render", sdf_rasterize_collision, true);
CONSOLE_BOOL_VAL("render", debug_rasterize_lru_collision, false);

enum GiQualitySetting
{
  GI_ONLY_AO = 0,
  GI_COLORED = 1,
  GI_SCREEN_PROBES = 2
};

CONSOLE_INT_VAL("render", gi_quality, GI_COLORED, GI_ONLY_AO, GI_SCREEN_PROBES);
CONSOLE_FLOAT_VAL_MINMAX("render", gi_algorithm_quality, 1, 0, 1);
CONSOLE_BOOL_VAL("render", gi_update_pos, true);
CONSOLE_BOOL_VAL("render", gi_update_scene, true);
CONSOLE_BOOL_VAL("render", gi_enabled, true);
CONSOLE_BOOL_VAL("render", gi_show_resticted_update_box, false);
CONSOLE_INT_VAL("render", gi_surface_cache_entries_k, 384, 0, 16384); // in 1024 entries
CONSOLE_BOOL_VAL("render", gi_baked_debug_hud, false);
CONSOLE_INT_VAL("render", gi_baked_debug_vis, 0, 0, 3); // 0=Off, 1=Probes, 2=Bricks, 3=Probes+Bricks
CONSOLE_FLOAT_VAL_MINMAX("render", gi_baked_debug_radius, 32.f, 4.f, 192.f);
CONSOLE_FLOAT_VAL_MINMAX("render", gi_baked_debug_sphere_scale, 0.08f, 0.02f, 0.40f);

static ShaderVariableInfo ssr_alternate_reflectionsVarId("ssr_alternate_reflections", true);
static ShaderVariableInfo gi_enable_lights_at_radiance_traceVarId("gi_enable_lights_at_radiance_trace", true);

extern ShaderBlockIdHolder rendinstVoxelizeSceneBlockId;
extern float cascade0Dist;

struct GiDatablockCache
{
  uint8_t sdfClips = 0; // 0 - autodetect
  float sdfVoxel0 = 0.15f;
  uint8_t albedoClips = 3;
  uint8_t radianceGridClipW = 28;
  uint8_t radianceGridClips = 4;
  uint16_t surfaceCacheFeedGroups = DaGISettings::SurfaceCache{}.feedGroups;
  uint16_t surfaceCacheRelightThreads = DaGISettings::SurfaceCache{}.relightThreads;
  uint16_t surfaceCacheVoxelRemeanFrames = DaGISettings::SurfaceCache{}.voxelRemeanFrames;
};

static GiDatablockCache giDatablockCache;

static void update_gi_datablock_cache()
{
  const DataBlock *graphicsGI = dgs_get_settings()->getBlockByNameEx("graphics")->getBlockByNameEx("gi");
  giDatablockCache.sdfClips = graphicsGI->getInt("sdfClips", giDatablockCache.sdfClips);
  giDatablockCache.sdfVoxel0 = clamp(graphicsGI->getReal("sdfVoxel0", giDatablockCache.sdfVoxel0), 0.065f, 0.45f);
  giDatablockCache.albedoClips = graphicsGI->getInt("albedoClips", giDatablockCache.albedoClips);
  giDatablockCache.radianceGridClipW = graphicsGI->getInt("radianceGridClipW", giDatablockCache.radianceGridClipW);
  giDatablockCache.radianceGridClips = graphicsGI->getInt("radianceGridClips", giDatablockCache.radianceGridClips);
  gi_surface_cache_entries_k.set(graphicsGI->getInt("surfaceCacheEntriesK", gi_surface_cache_entries_k));
  giDatablockCache.surfaceCacheFeedGroups = graphicsGI->getInt("surfaceCacheFeedGroups", giDatablockCache.surfaceCacheFeedGroups);
  giDatablockCache.surfaceCacheRelightThreads =
    graphicsGI->getInt("surfaceCacheRelightThreads", giDatablockCache.surfaceCacheRelightThreads);
  giDatablockCache.surfaceCacheVoxelRemeanFrames =
    graphicsGI->getInt("surfaceCacheVoxelRemeanFrames", giDatablockCache.surfaceCacheVoxelRemeanFrames);
}

static bool gi_bake_console_handler(const char *argv[], int argc)
{
  int found = 0;
  CONSOLE_CHECK_NAME("gi", "bake", 1, 2)
  {
    auto *wr = static_cast<WorldRenderer *>(get_world_renderer());
    if (!wr)
      return found;
    dagi_bake_runtime::Tier t = dagi_bake_runtime::Tier::Normal;
    if (argc > 1 && !dagi_bake_runtime::parse_tier(argv[1], t))
    {
      console::print_d("Usage: gi.bake [normal|high]");
      return found;
    }
    wr->triggerBakedGIRebuild(argc > 1 ? t : dagi_bake_runtime::Tier::None);
  }
  CONSOLE_CHECK_NAME("gi", "bake_status", 1, 1)
  {
    auto *wr = static_cast<WorldRenderer *>(get_world_renderer());
    if (wr)
      wr->logBakedGIStatus();
  }
  CONSOLE_CHECK_NAME("gi", "bake_delete", 1, 2)
  {
    auto *wr = static_cast<WorldRenderer *>(get_world_renderer());
    if (wr)
      wr->deleteBakedGICache(argc > 1 ? argv[1] : "all");
  }
  CONSOLE_CHECK_NAME("gi", "debug_hud", 1, 2)
  {
    const bool next = (argc > 1) ? (atoi(argv[1]) != 0) : !gi_baked_debug_hud.get();
    gi_baked_debug_hud.set(next);
    console::print_d("[daGIBake] debug HUD: %s", next ? "ON" : "OFF");
  }
  CONSOLE_CHECK_NAME("gi", "debug_vis", 1, 3)
  {
    const int mode = (argc > 1) ? clamp(atoi(argv[1]), 0, 3) : ((gi_baked_debug_vis.get() + 1) % 4);
    gi_baked_debug_vis.set(mode);
    if (argc > 2)
      gi_baked_debug_radius.set(clamp(float(atof(argv[2])), 4.f, 192.f));
    static const char *names[4] = {"OFF", "3D SH1 Probe Spheres", "Active Brick Wireframes", "Probes + Bricks"};
    console::print_d("[daGIBake] 3D debug vis: %d (%s), radius=%.1fm", mode, names[mode], gi_baked_debug_radius.get());
  }
  CONSOLE_CHECK_NAME("gi", "baked_ab", 1, 2)
  {
    auto *wr = static_cast<WorldRenderer *>(get_world_renderer());
    if (wr)
      wr->setBakedGISamplingEnabled(argc > 1 ? (atoi(argv[1]) != 0 ? 1 : 0) : -1);
  }
  return found;
}
REGISTER_CONSOLE_HANDLER(gi_bake_console_handler);

static void set_voxelization_transform(int max_res, IPoint3 res, const BBox3 &box)
{
  const Point3 voxelize_box_sz = max(Point3(1e-6f, 1e-6f, 1e-6f), box.width());

  // use fixed aspect ratio of 1. for oversampling and HW clipping
  const Point3 voxelize_aspect_ratio = point3(max(res, IPoint3(1, 1, 1))) / max_res;

  const Point3 mult = 2. * div(voxelize_aspect_ratio, voxelize_box_sz);
  const Point3 add = -mul(box[0], mult) - voxelize_aspect_ratio;
  ShaderGlobal::set_float4(voxelize_world_to_rasterize_space_mulVarId, P3D(mult), 0);
  ShaderGlobal::set_float4(voxelize_world_to_rasterize_space_addVarId, P3D(add), 0);
}

void WorldRenderer::processGIInvalidationRequests()
{
  if (enviProbeState != EnviProbeState::Ready)
    return;

  BBox3 boxes[MAX_GI_INVALIDATE_BOXES];
  uint32_t count = 0;
  bool fullInvalidate = false;
  {
    OSSpinlockScopedLock lock(pendingGiInvalidateLock);
    count = pendingGiInvalidateCount;
    pendingGiInvalidateCount = 0;
    for (uint32_t i = 0; i < count; ++i)
      boxes[i] = pendingGiInvalidateBoxes[i];
    fullInvalidate = pendingFullGiInvalidation;
    pendingFullGiInvalidation = false;
  }
  if (fullInvalidate)
    doInvalidateGI();
  else if (daGI2)
    for (uint32_t i = 0; i < count; ++i)
      daGI2->invalidateBox(boxes[i]);
}

ECS_TAG(render)
ECS_REQUIRE(ecs::Tag hero)
ECS_NO_ORDER
ECS_ON_EVENT(EventOnEntityTeleported)
static void wr_handle_on_hero_teleport_es(const ecs::Event &)
{
  if (auto *wr = get_world_renderer())
    static_cast<WorldRenderer *>(wr)->onHeroTeleportation();
}

void WorldRenderer::processHeroTeleportation()
{
  if (!hasPendingHeroTeleportation)
    return;

  const float cameraMove = length(currentFrameCamera.cameraWorldPos - prevFrameCamera.cameraWorldPos);
  const float teleportationDistanceThreshold =
    dgs_get_settings()->getBlockByNameEx("graphics")->getReal("teleportationDistanceThreshold", 3.0);

  if (cameraMove <= teleportationDistanceThreshold)
  {
    debug("WR: hero teleportation: skipped render invalidation because movement is too short (%f <= %f)", cameraMove,
      teleportationDistanceThreshold);
    hasPendingHeroTeleportation = false;
    return;
  }

  extern void invalidate_ssr_history(int frames);
  giUpdatePosFrameCounter = 0;
  invalidateAllGI();
  invalidate_ssr_history(3);

  hasPendingHeroTeleportation = false;
}

void WorldRenderer::doInvalidateGI()
{
  if (daGI2)
    daGI2->invalidateAll();
}

void WorldRenderer::invalidateRiCollision()
{
  if (lruCollision)
    lruCollision->clearRiInfo();
}

void WorldRenderer::invalidateAllGI()
{
  OSSpinlockScopedLock lock(pendingGiInvalidateLock);
  pendingFullGiInvalidation = true;
}
void WorldRenderer::invalidateGI(const BBox3 &world_box)
{
  OSSpinlockScopedLock lock(pendingGiInvalidateLock);
  if (pendingGiInvalidateCount < MAX_GI_INVALIDATE_BOXES)
  {
    pendingGiInvalidateBoxes[pendingGiInvalidateCount++] = world_box;
    return;
  }
  auto growth = [&](int i) {
    BBox3 merged = pendingGiInvalidateBoxes[i];
    merged += world_box;
    return merged.volume() - pendingGiInvalidateBoxes[i].volume();
  };
  int best = 0;
  float bestGrowth = growth(0);
  for (int i = 1; i < MAX_GI_INVALIDATE_BOXES; ++i)
  {
    const float g = growth(i);
    if (g < bestGrowth)
    {
      bestGrowth = g;
      best = i;
    }
  }
  pendingGiInvalidateBoxes[best] += world_box;
}

void WorldRenderer::drawGIDebug(const Frustum &camera_frustum)
{
  G_UNUSED(camera_frustum);
  if ((gi_baked_debug_vis.get() & 1) != 0 && bakedGIVolume.isLoaded())
    bakedGIVolume.drawDebugProbes(currentFrameCamera.viewItm.getcol(3), gi_baked_debug_radius.get(), gi_baked_debug_sphere_scale.get());
  if (daGI2)
  {
    daGI2->debugRenderTrans();
    mediaVolumes.debugRender();
  }
}

static const int MAX_GI_FRAMES_TO_INVALIDATE_POS = 32;
static ShaderVariableInfo use_hw_rt_giVarId("use_hw_rt_gi", true);
static ShaderVariableInfo dagi_force_opaque_traceVarId("dagi_force_opaque_trace", true);

eastl::string WorldRenderer::resolveBakeLevelId() const
{
  char nameBuf[DAGOR_MAX_PATH] = {};
  const char *levelBin = originalLevelBlk.getStr("levelBin", nullptr);
  if (!levelBin || !*levelBin)
  {
    const char *blkPath = g_entity_mgr ? g_entity_mgr->getOr(get_current_level_eid(), ECS_HASH("level__blk"), "") : "";
    DataBlock lblk;
    if (blkPath && *blkPath && dblk::load(lblk, blkPath, dblk::ReadFlag::ROBUST))
    {
      const char *lb = lblk.getStr("levelBin", "");
      if (lb && *lb)
        dd_get_fname_without_path_and_ext(nameBuf, sizeof(nameBuf), lb);
    }
  }
  else
  {
    dd_get_fname_without_path_and_ext(nameBuf, sizeof(nameBuf), levelBin);
  }

  if (!nameBuf[0])
  {
    const char *scn = sceneload::get_current_game().sceneName.c_str();
    if (scn && *scn)
    {
      dd_get_fname_without_path_and_ext(nameBuf, sizeof(nameBuf), scn);
      const size_t len = strlen(nameBuf);
      if (len > 5 && strcmp(nameBuf + len - 5, "_demo") == 0)
        nameBuf[len - 5] = '\0';
    }
  }
  return nameBuf[0] ? eastl::string(nameBuf) : eastl::string("level");
}

eastl::string WorldRenderer::resolveBakeCacheDir() const
{
  const DataBlock *graphicsBlk = ::dgs_get_settings()->getBlockByNameEx("graphics");
  const char *dir = graphicsBlk->getStr("giBakeDir", "baked_gi");
  return eastl::string(dir ? dir : "baked_gi");
}

BBox3 WorldRenderer::computeBakeWorldBox() const
{
  const DataBlock *graphicsGI = ::dgs_get_settings()->getBlockByNameEx("graphics")->getBlockByNameEx("gi");
  const float halfExtentXZ = clamp(graphicsGI->getReal("bakeHalfExtentXZ", 120.f), 32.f, 2048.f);
  const float halfExtentY = clamp(graphicsGI->getReal("bakeHalfExtentY", 16.f), 8.f, 256.f);

  Point3 center = currentFrameCamera.viewItm.getcol(3);
  BBox3 rawBox;
  if (staticSceneCollisionResource && !staticSceneCollisionResource->boundingBox.isempty())
    rawBox = staticSceneCollisionResource->boundingBox;
  else if (!binSceneBbox.isempty())
    rawBox = binSceneBbox;
  else
    rawBox = getWorldBBox3();

  if (!rawBox.isempty())
  {
    const Point3 rawW = rawBox.width();
    if (rawW.x <= halfExtentXZ * 2.f && rawW.z <= halfExtentXZ * 2.f)
      center = rawBox.center();
  }

  BBox3 clampedBox(center - Point3(halfExtentXZ, halfExtentY * 0.5f, halfExtentXZ),
    center + Point3(halfExtentXZ, halfExtentY * 1.5f, halfExtentXZ));
  if (!rawBox.isempty())
  {
    clampedBox[0].x = max(clampedBox[0].x, rawBox[0].x);
    clampedBox[0].y = max(clampedBox[0].y, rawBox[0].y - 2.f);
    clampedBox[0].z = max(clampedBox[0].z, rawBox[0].z);
    clampedBox[1].x = min(clampedBox[1].x, rawBox[1].x);
    clampedBox[1].y = min(clampedBox[1].y, rawBox[1].y + 8.f);
    clampedBox[1].z = min(clampedBox[1].z, rawBox[1].z);
    if (clampedBox.isempty() || clampedBox.width().x < 8.f || clampedBox.width().z < 8.f)
      clampedBox = BBox3(center - Point3(halfExtentXZ, 16.f, halfExtentXZ), center + Point3(halfExtentXZ, 48.f, halfExtentXZ));
  }
  return clampedBox;
}

void WorldRenderer::triggerBakedGIRebuild(dagi_bake_runtime::Tier tier)
{
  bakedGIForceRebuild = true;
  bakedGIExplicitTier = tier;
  setGIQualityFromSettings();
}

void WorldRenderer::logBakedGIStatus() const
{
  const eastl::string levelId = resolveBakeLevelId();
  const eastl::string dir = resolveBakeCacheDir();
  const eastl::string nPath = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::Normal);
  const eastl::string hPath = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::High);
  console::print_d("[daGIBake] level='%s' pipeline=%s bakedLoaded=%d tier=%s bricks=%u probe=%.2fm baking=%d(%.0f%%) promptHigh=%d\n"
                   "  normal='%s' (valid=%d)\n  high='%s' (valid=%d)",
    levelId.c_str(), isThinGBuffer() ? "thin" : "full", int(bakedGIVolume.isLoaded()),
    dagi_bake_runtime::tier_name(bakedGIVolume.getLoadedTier()), bakedGIVolume.getBrickCount(), bakedGIVolume.getProbeSize(),
    int(bakedGISession.isActive()), bakedGISession.getProgress() * 100.f, int(bakedGIPromptRebuildHigh), nPath.c_str(),
    int(dagi_bake_runtime::probe_cache_valid(nPath.c_str(), 0, 0)), hPath.c_str(),
    int(dagi_bake_runtime::probe_cache_valid(hPath.c_str(), 0, 0)));
}

void WorldRenderer::deleteBakedGICache(const char *which)
{
  const eastl::string levelId = resolveBakeLevelId();
  const eastl::string dir = resolveBakeCacheDir();
  if (!which || strcmp(which, "normal") == 0 || strcmp(which, "all") == 0)
  {
    const eastl::string p = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::Normal);
    std::remove(p.c_str());
    debug("[daGIBake] Deleted '%s'", p.c_str());
  }
  if (!which || strcmp(which, "high") == 0 || strcmp(which, "all") == 0)
  {
    const eastl::string p = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::High);
    std::remove(p.c_str());
    debug("[daGIBake] Deleted '%s'", p.c_str());
  }
  bakedGISession.cancel();
  bakedGIVolume.unload();
  setGIQualityFromSettings();
}

void WorldRenderer::setGIQualityFromSettings()
{
  bakedGISession.cancel();
  bakedGIPendingStart = false;
  bakedGIPromptRebuildHigh = false;
  bakedGIVolume.ensureFallbackBound();

  update_gi_datablock_cache();

  const DataBlock *graphicsBlk = ::dgs_get_settings()->getBlockByNameEx("graphics");
  const DataBlock *graphicsGI = graphicsBlk->getBlockByNameEx("gi");

  dagi_bake_runtime::Tier configuredTier = dagi_bake_runtime::Tier::Normal;
  const char *tierStr = graphicsBlk->getStr("giBakeTier", graphicsGI->getStr("bakeTier", "normal"));
  if (!dagi_bake_runtime::parse_tier(tierStr, configuredTier))
  {
    logwarn("Invalid graphics/giBakeTier '%s', defaulting to 'normal'", tierStr ? tierStr : "");
    configuredTier = dagi_bake_runtime::Tier::Normal;
  }

  dagi_bake_runtime::SourceMode sourceMode = dagi_bake_runtime::SourceMode::Auto;
  const char *srcStr = graphicsBlk->getStr("giSource", graphicsGI->getStr("source", "auto"));
  if (!dagi_bake_runtime::parse_source_mode(srcStr, sourceMode))
  {
    logwarn("Invalid graphics/giSource '%s', defaulting to 'auto'", srcStr ? srcStr : "");
    sourceMode = dagi_bake_runtime::SourceMode::Auto;
  }

  const bool forceRebuildCfg = graphicsBlk->getBool("giForceRebuild", false) || bakedGIForceRebuild;
  if (bakedGIForceRebuild && bakedGIExplicitTier != dagi_bake_runtime::Tier::None)
    configuredTier = bakedGIExplicitTier;
  bakedGIForceRebuild = false;
  bakedGIExplicitTier = dagi_bake_runtime::Tier::None;

  GiQuality quality = getGiQuality();
  if (quality.algorithm == GiAlgorithm::OFF)
  {
    bakedGIVolume.unload();
    bakedGIEnabled = false;
    closeGI();
    gi_enabled.set(false);
    return;
  }

  const eastl::string levelId = resolveBakeLevelId();
  const eastl::string cacheDir = resolveBakeCacheDir();
  const eastl::string normalPath = dagi_bake_runtime::make_cache_path(cacheDir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::Normal);
  const eastl::string highPath = dagi_bake_runtime::make_cache_path(cacheDir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::High);

  const bool normalValid = dagi_bake_runtime::probe_cache_valid(normalPath.c_str(), 0, 0);
  const bool highValid = dagi_bake_runtime::probe_cache_valid(highPath.c_str(), 0, 0);
  const bool canUseDynamic = !isThinGBuffer() && !is_only_low_gi_supported();

  // Mode dispatch across 3 paths:
  //   Path 1: Valid .dgibake cache on disk -> load BakedGIVolume and call closeGI() (zero runtime GI cost).
  //   Path 2: Cache missing in Baked mode or Thin GBuffer -> schedule frame-driven GPU bake (BakedGISession).
  //   Path 3: Dynamic mode or deleted cache in Full Deferred Auto mode -> run live dynamic daGI2.
  const dagi_bake_runtime::Selection sel =
    dagi_bake_runtime::select_mode(configuredTier, sourceMode, canUseDynamic, normalValid, highValid, forceRebuildCfg);
  bakedGIPromptRebuildHigh = sel.promptRebuildHigh;
  if (sel.promptRebuildHigh)
    debug("[daGIBake] Option A: high tier cache '%s' missing; using normal tier (high tier can be rebuilt via gi.bake high)",
      highPath.c_str());

  // Path 1: Static Baked GI cache already valid on disk -> load into GPU 3D brick atlas & turn off live daGI2!
  if (!sel.useDynamic && !sel.rebuildFirst)
  {
    const eastl::string &loadPath = (sel.loadTier == dagi_bake_runtime::Tier::High) ? highPath : normalPath;
    if (bakedGIVolume.loadFromFile(loadPath.c_str(), 0, 0, false))
    {
      bakedGIEnabled = true;
      closeGI(); // zero live daGI2 GPU/CPU overhead during gameplay!
      bakedGIVolume.ensureFallbackBound();

      if (isThinGBuffer())
      {
        gi_quality.setMinMax(GI_ONLY_AO, GI_ONLY_AO);
        gi_quality.set(GI_ONLY_AO);
      }
      else
      {
        gi_quality.setMinMax(GI_ONLY_AO, GI_SCREEN_PROBES);
        gi_quality.set(GI_COLORED);
      }
      ShaderGlobal::set_int(gi_qualityVarId, gi_quality);
      ShaderGlobal::set_int(use_hw_rt_giVarId, 0);
      gi_enabled.set(true);
      requestFgRecreation("baked gi loaded");
      return;
    }
    logwarn("[daGIBake] Failed to load '%s'; falling back", loadPath.c_str());
  }

  // Path 2: Runtime bake required (Thin GBuffer missing cache, giSource=baked missing cache, or explicit rebuild)
  if (!sel.useDynamic && sel.rebuildFirst)
  {
    bakedGIVolume.unload();
    bakedGIEnabled = false;
    bakedGIPendingStart = true;
    bakedGITargetTier = sel.rebuildTier;
  }
  else
  {
    // Path 3: Dynamic daGI2 mode (Full Deferred when giSource=dynamic or when .dgibake was deleted in giSource=auto)
    bakedGIVolume.unload();
    bakedGIEnabled = false;
  }

  if (isThinGBuffer() || is_only_low_gi_supported())
    gi_quality.setMinMax(GI_ONLY_AO, GI_ONLY_AO);
  else
    gi_quality.setMinMax(GI_ONLY_AO, GI_SCREEN_PROBES);

  switch (quality.algorithm)
  {
    case GiAlgorithm::OFF:
      closeGI();
      gi_enabled.set(false);
      return;
    case GiAlgorithm::LOW: gi_quality.set(GI_ONLY_AO); break;
    case GiAlgorithm::MEDIUM: gi_quality.set(GI_COLORED); break;
    case GiAlgorithm::HIGH: gi_quality.set(GI_SCREEN_PROBES); break;
    default:
      logwarn("Invalid GI quality");
      gi_quality.set(GI_ONLY_AO);
      break;
  }
  if (is_rtgi_enabled() && !bakedGIPendingStart)
  {
    gi_quality.set(GI_SCREEN_PROBES);
    int rtgiMode = dgs_get_settings()->getBlockByNameEx("graphics")->getBool("giForceInlineTrace", true) ? 1 : 2;
    ShaderGlobal::set_int(use_hw_rt_giVarId, rtgiMode);
  }
  else
  {
    ShaderGlobal::set_int(use_hw_rt_giVarId, 0);
    if (is_ptgi_enabled() && !bakedGIPendingStart)
      gi_quality.set(GI_COLORED);
  }
  gi_algorithm_quality.set(quality.algorithmQuality);

  ShaderGlobal::set_int(dagi_force_opaque_traceVarId,
    dgs_get_settings()->getBlockByNameEx("graphics")->getBool("giForceOpaqueTrace", false) ? 1 : 0);

  initGI();

  gi_enabled.set(true);
  giUpdatePosFrameCounter = 0;
}

bool WorldRenderer::giNeedsReprojection()
{
  return hasFeature(FeatureRenderFlags::DEFERRED_LIGHT) && daGI2 && !bakedGIEnabled && gi_quality == GI_SCREEN_PROBES &&
         daGI2->getNextSettings().screenProbes.tileSize > 8;
}

uint32_t WorldRenderer::getGIHistoryFrames() const { return bakedGIEnabled ? 1024u : (daGI2 ? daGI2->getHistoryFrames() : 0); }

void WorldRenderer::setGILightsToShader(bool allow_frustum_lights)
{
  if (!lights.initialized())
    return;

  if (giGlobalLightsPrepared)
    lights.setOutOfFrustumLightsToShader(*giLights);
  else if (allow_frustum_lights)
    lights.setInsideOfFrustumLightsToShader();
  else
    lights.setEmptyOutOfFrustumLights(*giLights);
}

uint32_t get_gi_history_frames() { return static_cast<WorldRenderer *>(get_world_renderer())->getGIHistoryFrames(); }

WorldRenderer::GIConvergenceState WorldRenderer::logGIConvergence()
{
  const GIConvergenceState state{daGI2 && daGI2->requiresUpdate(), getGIHistoryFrames(),
    (uint32_t)::dgs_get_settings()->getBlockByNameEx("debug")->getInt("giConvergedHistoryFrames", 600)};
  const bool gi = !daGI2 || (!state.requiresUpdate && state.historyFrames >= state.historyFramesNeed);
  if (gi && !giConvergenceLogged)
    debug("[convergence] gi");
  giConvergenceLogged = gi;
  return state;
}

void WorldRenderer::overrideGISettings(const DaGISettings &settings)
{
  if (!giSettingsOverride)
    giSettingsOverride = eastl::make_unique<DaGISettings>();
  *giSettingsOverride = settings;
}

void WorldRenderer::resetGISettingsOverride() { giSettingsOverride.reset(); }

DaGISettings WorldRenderer::getGISettings() const
{
  if (giSettingsOverride)
    return *giSettingsOverride;

  auto s = daGI2->getNextSettings();
  constexpr float ALBEDO_MEDIA_MUL = 0.0625f;
  const bool temporality = gi_quality == GI_SCREEN_PROBES || gi_algorithm_quality > 0.0001;

  const float temporalSpeed = temporality ? lerp<float>(0.05, 1, gi_algorithm_quality) : 0;
  s.mediaScene.temporalSpeed = temporalSpeed * ALBEDO_MEDIA_MUL;
  s.albedoScene.temporalSpeed = temporalSpeed * ALBEDO_MEDIA_MUL;
  s.sdf.clips = clamp(giDatablockCache.sdfClips > 0 ? giDatablockCache.sdfClips : (bareMinimumPreset ? 5 : 6), 4, 6);
  s.sdf.voxel0Size = giDatablockCache.sdfVoxel0;
  s.surfaceCache.entries = uint32_t(gi_surface_cache_entries_k.get()) << 10;
  s.surfaceCache.feedGroups = giDatablockCache.surfaceCacheFeedGroups;
  s.surfaceCache.relightThreads = giDatablockCache.surfaceCacheRelightThreads;
  s.surfaceCache.voxelRemeanFrames = giDatablockCache.surfaceCacheVoxelRemeanFrames;
  if (bakedGIPendingStart || bakedGISession.isActive())
  {
    // During runtime baking (including on Thin GBuffer where gi_quality is GI_ONLY_AO), allocate full AlbedoScene + SurfaceCache + RadianceGrid!
    s.skyVisibility.clipW = 0;
    s.albedoScene.clips = giDatablockCache.albedoClips;
    s.radianceGrid.w = giDatablockCache.radianceGridClipW;
    s.radianceGrid.clips = giDatablockCache.radianceGridClips;
    s.radianceGrid.irradianceProbeDetail = 2.f;
    s.radianceGrid.additionalIrradianceClips = 2;
    s.screenProbes.tileSize = 0;
    s.volumetricGI.tileSize = 0;
    s.radianceGrid.temporalSpeed = 1.0f;
  }
  else if (gi_quality == GI_ONLY_AO)
  {
    s.skyVisibility.clipW = bareMinimumPreset ? 64 : 56;
    s.skyVisibility.clips = bareMinimumPreset ? 5 : 6;
    s.albedoScene.clips = 0;
    s.radianceGrid.w = 0;
    s.screenProbes.tileSize = 0;
    s.volumetricGI.tileSize = 0;
    s.surfaceCache.entries = 0;
    s.skyVisibility.framesToUpdateTemporally = temporality ? lerp(96, 32, gi_algorithm_quality) : 0;
    s.mediaScene.temporalSpeed = gi_algorithm_quality * ALBEDO_MEDIA_MUL;
  }
  else if (gi_quality == GI_COLORED)
  {
    s.skyVisibility.clipW = 0;
    s.albedoScene.clips = giDatablockCache.albedoClips;
    s.radianceGrid.w = giDatablockCache.radianceGridClipW;
    s.radianceGrid.clips = giDatablockCache.radianceGridClips;
    s.radianceGrid.irradianceProbeDetail = 2.f;
    s.radianceGrid.additionalIrradianceClips = 2;
    s.screenProbes.tileSize = 0;
    s.volumetricGI.tileSize = 64;
    s.radianceGrid.temporalSpeed = temporality ? 1.0f / lerp(96.0f, 32.0f, gi_algorithm_quality) : 0.0f;
  }
  else if (gi_quality == GI_SCREEN_PROBES)
  {
    s.skyVisibility.clipW = 0;
    s.radianceGrid.w = giDatablockCache.radianceGridClipW;
    s.radianceGrid.clips = giDatablockCache.radianceGridClips;
    s.radianceGrid.irradianceProbeDetail = 1.f;
    s.radianceGrid.additionalIrradianceClips = is_rtr_enabled() ? 2 : 0;
    s.albedoScene.clips = giDatablockCache.albedoClips;

    s.screenProbes.temporality = giDynamicQuality;

    int w, h;
    getPostFxInternalResolution(w, h);
    float minTileSize = cvt_ex(h, 1080, 1080 * 2, 16, 32);
    float tileSizeF = cvt(gi_algorithm_quality, 0.25, 0.95f, minTileSize, 8);
    s.screenProbes.tileSize = static_cast<int>(round(tileSizeF)) & ~1;

    float radianceOctResF = cvt(gi_algorithm_quality, 0.f, 0.5f, 4, 8);
    s.screenProbes.radianceOctRes = static_cast<int>(round(radianceOctResF));

    s.screenProbes.angleFiltering = gi_algorithm_quality > 0.975f;
    s.volumetricGI.tileSize = (int(lerp(64.f, 48.f, gi_algorithm_quality)) + 1) & ~1;
    s.volumetricGI.slices = int(lerp(16.f, 24.f, gi_algorithm_quality));
  }

  return s;
}

void WorldRenderer::giUpdateSettings()
{
  if (bakedGIEnabled && bakedGIVolume.isLoaded())
  {
    if (!gi_enabled.get())
    {
      bakedGIVolume.unload();
      bakedGIEnabled = false;
      ShaderGlobal::set_int(gi_qualityVarId, -1);
      requestFgRecreation("baked gi disabled");
      return;
    }
    bakedGIVolume.ensureFallbackBound();
    const int targetQuality = isThinGBuffer() ? GI_ONLY_AO : GI_COLORED;
    ShaderGlobal::set_int(gi_qualityVarId, targetQuality);
    gi_quality.pullValueChange();
    gi_algorithm_quality.pullValueChange();
    return;
  }

  if (bakedGISession.isActive())
  {
    ShaderGlobal::set_int(gi_qualityVarId, isThinGBuffer() ? GI_ONLY_AO : int(gi_quality));
    return;
  }

  if (daGI2 && !gi_enabled.get())
    closeGI();

  if (!daGI2 && gi_enabled.get())
    initGI();

  ShaderGlobal::set_int(gi_qualityVarId, daGI2 ? gi_quality : -1);

  if (!daGI2)
    return;

  if (gi_algorithm_quality >= 0.75f)
    giDynamicQuality = cvt(gi_algorithm_quality, 0.75f, 1.0f, 0.55f, 1.0f);
  else
    giDynamicQuality = cvt(gi_algorithm_quality, 0.25f, 0.5f, 0.1f, 0.55f);

  const bool giNeededReprojection = giNeedsReprojection();
  daGI2->setSettings(getGISettings());

  const bool qualityChanged = gi_quality.pullValueChange();
  const bool algorithmQualityChanged = gi_algorithm_quality.pullValueChange();
  if (qualityChanged || algorithmQualityChanged || giNeededReprojection != giNeedsReprojection())
    requestFgRecreation("gi shvars");
}

void WorldRenderer::tickBakedGISession()
{
  if (bakedGIPendingStart && daGI2)
  {
    const Point3 camPos = currentFrameCamera.viewItm.getcol(3);
    const bool skyReady = (enviProbeState == EnviProbeState::Ready);
    const bool shadowsReady = ignoreStaticShadowsForGI || !hasFeature(FeatureRenderFlags::STATIC_SHADOWS) ||
                              shadowsManager.validStaticShadows(camPos, 100.f) || renderedFrames > 30;
    const BBox3 bakeBox = computeBakeWorldBox();
    if (skyReady && shadowsReady && !bakeBox.isempty())
    {
      bakedGIPendingStart = false;
      const DataBlock *graphicsGI = ::dgs_get_settings()->getBlockByNameEx("graphics")->getBlockByNameEx("gi");
      dagi_bake_runtime::BakeParams bp;
      bp.tier = bakedGITargetTier;
      bp.levelId = resolveBakeLevelId();
      bp.outputPath = dagi_bake_runtime::make_cache_path(resolveBakeCacheDir().c_str(), bp.levelId.c_str(), bp.tier);
      bp.worldBox = bakeBox;
      bp.probeSizeOverride =
        graphicsGI->getReal(bp.tier == dagi_bake_runtime::Tier::High ? "bakeProbeSizeHigh" : "bakeProbeSizeNormal", 0.f);
      // Collect artist-placed indoor envi_probe_box OBBs (unit [-0.5, 0.5]^3 local space) inside the bake bounds
      // so interior probes are guaranteed isolated from outdoor probes during CPU hole-filling dilation.
      if (const scene::TiledScene *probeBoxes = getEnviProbeBoxesScene())
      {
        bbox3f queryBox;
        queryBox.bmin = v_ldu(&bakeBox[0].x);
        queryBox.bmax = v_ldu(&bakeBox[1].x);
        probeBoxes->boxCull<false, false>(queryBox, 0, 0, [&](scene::node_index, mat44f_cref m) {
          mat44f invMat;
          v_mat44_inverse43(invMat, m);
          TMatrix invTm;
          v_mat_43cu_from_mat44(invTm.m[0], invMat);
          bp.indoorInvTms.push_back(invTm);
        });
      }
      if (!staticSceneCollisionResource)
        staticSceneCollisionResource = dacoll::get_static_collision_resource();
      bakedGISession.start(bp, *daGI2);
    }
  }

  if (!bakedGISession.isActive() || !daGI2)
    return;

  int w, h, maxW, maxH;
  getRenderingResolution(w, h);
  getMaxPossibleRenderingResolution(maxW, maxH);

  auto rasterizeTileCb = [&](DaGI &dagi, const Point3 &tileCenter) {
    TMatrix tileItm = TMatrix::IDENT;
    tileItm.setcol(3, tileCenter);
    const DaGISettings s = dagi.getNextSettings();
    if (lights.initialized())
    {
      alignas(16) TMatrix4 globtm = dagi_lights_globtm(s.sdf, tileCenter);
      lights.cullOutOfFrustumLights(*giLights, (mat44f_cref)globtm, SpotLightMaskType::SPOT_LIGHT_MASK_GI,
        OmniLightMaskType::OMNI_LIGHT_MASK_GI);
      giGlobalLightsPrepared = true;
    }
    if (!ShaderGlobal::is_var_assumed(gi_enable_lights_at_radiance_traceVarId))
      ShaderGlobal::set_int(gi_enable_lights_at_radiance_traceVarId, lights.initialized() ? 1 : 0);

    dagi.beforeRender(w, h, maxW, maxH, tileItm, currentFrameCamera.jitterProjTm, 0.1f, 500.f, DaGI::RadianceUpdate::On);
    updateGIPosInternal(tileCenter, tileItm, tileCenter.y - 64.f, tileCenter.y + 64.f);
  };

  if (giGlobalLightsPrepared && lights.initialized())
    lights.setOutOfFrustumLightsToShader(*giLights);

  // Preserve world_view_pos across baking tile rasterization so FrameGraph debug nodes (which bind
  // world_view_pos) do not flag spurious global shader variable mutations during gameplay frames.
  const Color4 savedWorldViewPos = ShaderGlobal::get_float4(world_view_posVarId);
  const bool stillRunning = bakedGISession.tick(*daGI2, rasterizeTileCb, bakedGIVolume);
  ShaderGlobal::set_float4(world_view_posVarId, savedWorldViewPos);

  if (giGlobalLightsPrepared && lights.initialized())
    lights.setInsideOfFrustumLightsToShader();

  if (!stillRunning && bakedGISession.succeeded() && bakedGIVolume.isLoaded())
  {
    bakedGIEnabled = true;
    closeGI(); // free temporary WorldSDF / SurfaceCache / LRUCollision immediately!
    bakedGIVolume.ensureFallbackBound();
    const int targetQuality = isThinGBuffer() ? GI_ONLY_AO : GI_COLORED;
    gi_quality.set(targetQuality);
    ShaderGlobal::set_int(gi_qualityVarId, targetQuality);
    requestFgRecreation("baked gi session completed");
  }
}

void WorldRenderer::giBeforeRender()
{
  giGlobalLightsPrepared = false;

  if (bakedGIPendingStart || bakedGISession.isActive())
  {
    tickBakedGISession();
    if (bakedGISession.isActive() || bakedGIEnabled)
      return;
  }

  if (bakedGIEnabled && bakedGIVolume.isLoaded())
    return;

  if (!daGI2)
    return;
  TIME_D3D_PROFILE(gi2_before_render)

  int w, h, maxW, maxH;
  getRenderingResolution(w, h);
  getMaxPossibleRenderingResolution(maxW, maxH);

  const DaGISettings s = daGI2->getNextSettings();

  const bool useGiGlobalLights = s.surfaceCache.entries > 0;
  if (useGiGlobalLights && lights.initialized())
  {
    TIME_PROFILE(cull_global_lights)
    alignas(16) TMatrix4 globtm = dagi_lights_globtm(s.sdf, currentFrameCamera.viewItm.getcol(3));
    lights.cullOutOfFrustumLights(*giLights, (mat44f_cref)globtm, SpotLightMaskType::SPOT_LIGHT_MASK_GI,
      OmniLightMaskType::OMNI_LIGHT_MASK_GI);
    giGlobalLightsPrepared = true;
  }
  if (!ShaderGlobal::is_var_assumed(gi_enable_lights_at_radiance_traceVarId))
    ShaderGlobal::set_int(gi_enable_lights_at_radiance_traceVarId, useGiGlobalLights ? 1 : 0);

  ShaderGlobal::set_int(ssr_alternate_reflectionsVarId, ssrWantsAlternateReflections && gi_quality > 0 ? 1 : 0);
  const float giUpdateDist = 100.f;
  DaGI::RadianceUpdate ru = DaGI::RadianceUpdate::On;
  if (!ignoreStaticShadowsForGI && ++giUpdatePosFrameCounter <= MAX_GI_FRAMES_TO_INVALIDATE_POS)
  {
    const Point3 &cameraPos = currentFrameCamera.viewItm.getcol(3);
    if (!shadowsManager.validStaticShadows(cameraPos, giUpdateDist) ||
        (giUpdatePosFrameCounter == 1 && !shadowsManager.fullyUpdatedStaticShadows(cameraPos, giUpdateDist)))
      ru = DaGI::RadianceUpdate::Off;
  }

  daGI2->beforeRender(w, h, maxW, maxH, currentFrameCamera.viewItm, currentFrameCamera.jitterProjTm,
    currentFrameCamera.noJitterPersp.zn, currentFrameCamera.noJitterPersp.zf, ru);
}

void WorldRenderer::renderGiCollision(const TMatrix &view_itm, const Frustum &)
{
  if (!debug_rasterize_lru_collision || !voxelizeCollision || !voxelizeCollision->renderCollisionElem)
    return;
  DA_PROFILE_GPU;
  rendinst::riex_collidable_t handles;
  BBox3 sceneBox(view_itm.getcol(3) - Point3(30, 30, 30), view_itm.getcol(3) + Point3(30, 30, 30));
  rendinst::gatherRIGenExtraCollidableMin(handles, v_ldu_bbox3(sceneBox), 0);
  if (staticSceneCollisionResource && (staticSceneCollisionResource->boundingBox & sceneBox))
    handles.push_back(rendinst::RIEX_HANDLE_NULL);
  ShaderGlobal::set_int(get_shader_variable_id("rasterize_collision_type", true), 1);
  if (!handles.empty())
    voxelizeCollision->rasterize(*lruCollision, dag::Span<uint64_t>(handles.data(), handles.size()), nullptr, nullptr,
      voxelizeCollision->renderCollisionElem, 1, false);
}

bool WorldRenderer::requiresGIUpdate() const
{
  if (bakedGIEnabled || bakedGISession.isActive())
    return false;
  if (!daGI2)
    return false;
  return daGI2->requiresUpdate();
}

void WorldRenderer::updateGIPos(const Point3 &pos, const TMatrix &view_itm, float hmin, float hmax)
{
  if (bakedGIEnabled || bakedGISession.isActive())
    return;
  updateGIPosInternal(pos, view_itm, hmin, hmax);
}

void WorldRenderer::updateGIPosInternal(const Point3 &pos, const TMatrix &view_itm, float hmin, float hmax)
{
  if (!daGI2)
    return;

  if (giWindows)
    giWindows->updatePos(pos);

  if (!gi_update_pos.get())
    return daGI2->updateConstants();
  int oldBlock = ShaderGlobal::getBlock(ShaderGlobal::LAYER_FRAME);
  auto baseVoxelizeRI = [&](const BBox3 &box_, const Point3 &voxelSize) {
    BBox3 box = box_;
    ShaderGlobal::setBlock(globalFrameBlockId, ShaderGlobal::LAYER_FRAME);
    const IPoint3 res(ceil(div(box.width(), voxelSize)));
    getMinMaxZ(box[0].y, box[1].y);
    int maxRes = max(max(res.x, res.y), res.z);
    G_ASSERT(maxRes > 0);
    SCOPE_RENDER_TARGET;
    set_voxelization_target_and_override(maxRes, maxRes);
    ShaderGlobal::set_float4(voxelize_box0VarId, box[0].x, box[0].y, box[0].z, 0);
    ShaderGlobal::set_float4(voxelize_box1VarId, 1. / box.width().x, 1 / box.width().y, 1 / box.width().z, 0);
    bool ret = false;
    bbox3f boxCull = v_ldu_bbox3(box);
    bbox3f actualBox;
    v_bbox3_init_empty(actualBox);
    rendinst::prepareRIGenExtraVisibilityBox(boxCull, 3, voxelSize.x * 0.5, 71, *rendinst_voxelize_visibility, &actualBox);
    if (v_bbox3_test_box_intersect_safe(actualBox, boxCull))
    {
      box[0].y = floorf(v_extract_y(actualBox.bmin) / voxelSize.y) * voxelSize.y;
      box[1].y = ceilf(v_extract_y(actualBox.bmax) / voxelSize.y) * voxelSize.y;
      const int resY = int(ceilf((box[1].y - box[0].y) / voxelSize.y));

      set_voxelization_transform(maxRes, IPoint3(res.x, resY, res.z), box);

      SCENE_LAYER_GUARD(rendinstVoxelizeSceneBlockId);
      rendinst::render::renderRIGen(rendinst::RenderPass::VoxelizeAlbedo, rendinst_voxelize_visibility, view_itm,
        rendinst::LayerFlag::Opaque, rendinst::OptimizeDepthPass::No, 3);
      ret = true;
    }

    set_voxelization_transform(maxRes, res, box_);
    RenderAlbedoVoxelization ecsRenderEvent(box_, false);
    g_entity_mgr->broadcastEventImmediate(ecsRenderEvent);
    const bool ecsRenderedAnything = ecsRenderEvent.get<1>();
    ret |= ecsRenderedAnything;

    reset_voxelization_override();
    ShaderElement::invalidate_cached_state_block();

    return ret;
  };

  Point3 giPos = pos;
  G_UNUSED(hmax);
  giPos.y = min(giPos.y, hmax + giHmapOffsetFromUp);
  giPos.y = max(giPos.y, hmin + giHmapOffsetFromLow);

  if (giGlobalLightsPrepared)
    lights.setOutOfFrustumLightsToShader(*giLights);

  TIME_D3D_PROFILE(gi2_update_pos)
  {
    int rw, rh;
    getRenderingResolution(rw, rh);
    bvh_bind_resources(rw);
  }
  mediaVolumes.updateTypes();
  if (mediaVolumes.bakePending(globalFrameBlockId))
    daGI2->invalidateInitialMedia();
  daGI2->updatePosition(
    [&](int, const BBox3 &box, float voxelSize, uintptr_t &) {
      BBox3 sceneBox = box;
      sceneBox[0] -= Point3(voxelSize, voxelSize, voxelSize);
      sceneBox[1] += Point3(voxelSize, voxelSize, voxelSize);

      rendinst::riex_collidable_t handles;
      if (sdf_rasterize_collision)
      {
        TIME_PROFILE(sdf_from_collision)
        rendinst::gatherRIGenExtraCollidableMin(handles, v_ldu_bbox3(sceneBox), voxelSize * 0.5f);
      }
      if (staticSceneCollisionResource && (staticSceneCollisionResource->boundingBox & sceneBox))
        handles.push_back(rendinst::RIEX_HANDLE_NULL);
      {
        TIME_PROFILE(sort_instances)
        stlsort::sort(handles.begin(), handles.end(),
          [](auto a, auto b) { return rendinst::handle_to_ri_type(a) < rendinst::handle_to_ri_type(b); });
      }
      IPoint3 res = ipoint3(floor(sceneBox.width() / voxelSize + Point3(0.5, 0.5, 0.5)));
      const int maxRes = max(max(res.x, res.y), max(res.z, 1));

      SCOPE_RENDER_TARGET;
      set_voxelization_transform(maxRes, res, sceneBox);
      set_voxelization_target_and_override(maxRes * world_sdf_rasterize_supersample, maxRes * world_sdf_rasterize_supersample);

      if (!handles.empty())
      {
        if (voxelSize <= world_sdf_from_collision_rasterize_below)
        {
          bool prims = rasterize_sdf_prims.get() && !d3d::get_driver_desc().caps.hasBarycentrics;
          voxelizeCollision->rasterizeSDF(*lruCollision, dag::Span<uint64_t>(handles.data(), handles.size()), prims);
        }
        else
        {
          voxelizeCollision->voxelizeSDFCompute(*lruCollision, dag::Span<uint64_t>(handles.data(), handles.size()));
        }
      }

      RenderSdfVoxelization ecsRenderEvent(sceneBox, false);
      g_entity_mgr->broadcastEventImmediate(ecsRenderEvent);
      const bool ecsRenderedAnything = ecsRenderEvent.get<1>();

      reset_voxelization_override();
      ShaderElement::invalidate_cached_state_block();

      bool intersectLevel = !handles.empty() || ecsRenderedAnything;
      return intersectLevel ? UpdateGiQualityStatus::RENDERED : UpdateGiQualityStatus::NOTHING;
    },
    [&](const BBox3 &box, float voxelSize, uintptr_t &) {
      SCOPE_VIEW_PROJ_MATRIX;
      const bool intersectLevel = baseVoxelizeRI(box, Point3(voxelSize, voxelSize, voxelSize));
      return intersectLevel ? UpdateGiQualityStatus::RENDERED : UpdateGiQualityStatus::NOTHING;
    },
    [&](const BBox3 &region, float voxelSize) { mediaVolumes.setRegionInstances(region, voxelSize); });
  bvh_unbind_resources();
  mediaVolumes.debugPrepare(view_itm.getcol(3));

  if (giGlobalLightsPrepared)
    lights.setInsideOfFrustumLightsToShader();

  ShaderGlobal::setBlock(oldBlock, ShaderGlobal::LAYER_FRAME);
}

void WorldRenderer::initGIWindows(eastl::unique_ptr<scene::TiledScene> &&windows)
{
  giWindows.reset();
  giWindows = eastl::make_unique<GIWindows>();
  giWindows->init(eastl::move(windows));
}

void WorldRenderer::initGI()
{
  requestFgRecreation("initGI");

  closeGI();
  if (getGiQuality().algorithm == GiAlgorithm::OFF)
  {
    createGiNodes();
    return;
  }

  staticSceneCollisionResource = dacoll::get_static_collision_resource();
  if (staticSceneCollisionResource)
  {
    const uint32_t faceCnt = (uint32_t)staticSceneCollisionResource->getTrianglesCount(CollisionNode::TRACEABLE);
    if (faceCnt > LRURendinstCollision::MAX_VOXELIZATION_TRIS)
      logerr("static scene collision has %u faces, over the %u GI voxelization limit; SDF gets no static scene", faceCnt,
        LRURendinstCollision::MAX_VOXELIZATION_TRIS);
    if (!faceCnt || faceCnt > LRURendinstCollision::MAX_VOXELIZATION_TRIS)
      staticSceneCollisionResource = nullptr;
  }

  daGI2 = create_dagi();
  init_voxelization();
  voxelizeCollision = new LRUCollisionVoxelization;
  voxelizeCollision->init();

  lruCollision.reset();
  lruCollision = LRURendinstCollision::acquire();

  rendinst_voxelize_visibility = rendinst::createRIGenVisibility(midmem);
  rendinst::setRIGenVisibilityMinLod(rendinst_voxelize_visibility, 0, 2);

  createGiNodes();

  ShaderGlobal::set_sampler(specular_tex_samplerstateVarId, d3d::request_sampler({}));
}

void WorldRenderer::resetGI()
{
  bakedGIVolume.afterReset();
  if (lruCollision)
    lruCollision->reset();
  if (daGI2)
    daGI2->afterReset();
  mediaVolumes.afterReset();
  if (giWindows)
    giWindows->afterReset();
  giUpdatePosFrameCounter = 0;
}

void WorldRenderer::closeGI()
{
  staticSceneCollisionResource = nullptr;
  lruCollision.reset();
  rendinst::destroyRIGenVisibility(rendinst_voxelize_visibility);
  rendinst_voxelize_visibility = nullptr;

  if (daGI2)
    close_voxelization();
  del_it(daGI2);
  del_it(voxelizeCollision);
  createGiNodes();

  mediaVolumes.clear();

  requestFgRecreation("close GI");
}

const scene::TiledScene *WorldRenderer::getWallsScene() const { return nullptr; }

const scene::TiledScene *WorldRenderer::getWindowsScene() const { return giWindows ? giWindows->windows.get() : nullptr; }

void WorldRenderer::createGiNodes()
{
  if (!daGI2 || bakedGIEnabled || bakedGISession.isActive() || bakedGIPendingStart)
  {
    giRenderFGNode = {};
    giCalcFGNode = {};
    giFeedbackFGNode = {};
    giScreenDebugFGNode = {};
    return;
  }

  giCalcFGNode = makeGiCalcNode();
  giFeedbackFGNode = makeGiFeedbackNode();
#if DAGOR_DBGLEVEL > 0
  giScreenDebugFGNode = makeGiScreenDebugNode();
  giScreenDebugDepthFGNode = makeGiScreenDebugDepthNode();
#endif
}

DaGI *WorldRenderer::getGI() { return daGI2; }

void WorldRenderer::setBakedGISamplingEnabled(int mode)
{
  const bool next = (mode < 0) ? !bakedGIVolume.isSamplingEnabled() : (mode != 0);
  bakedGIVolume.setSamplingEnabled(next);
  console::print_d("[daGIBake] Baked GI shader sampling: %s (A/B compare)", next ? "ON (Baked SH1)" : "OFF (Sky Ambient)");
}

void WorldRenderer::renderBakedGIDebugBricks() const
{
  if ((gi_baked_debug_vis.get() & 2) != 0 && bakedGIVolume.isLoaded())
    bakedGIVolume.drawDebugBricks(currentFrameCamera.viewItm.getcol(3), gi_baked_debug_radius.get());
}

void WorldRenderer::renderBakedGIDebugHUD() const
{
  const bool cfgHud = ::dgs_get_settings()->getBlockByNameEx("graphics")->getBool("giDebugOverlay", false);
  const bool isBaking = isBakedGIBaking();
  if (!isBaking && !gi_baked_debug_hud.get() && !cfgHud)
    return;

  StdGuiRender::ScopeStarterOptional strt;
  StdGuiRender::set_ablend(true);
  StdGuiRender::reset_textures();

  const float x0 = 18.f;
  const float y0 = 18.f;
  const float boxW = 540.f;
  const float boxH = isBaking ? 142.f : 128.f;

  StdGuiRender::solid_frame(x0, y0, x0 + boxW, y0 + boxH, 1.5f, E3DCOLOR(14, 18, 26, 215),
    isBaking ? E3DCOLOR(255, 190, 50, 240) : E3DCOLOR(70, 210, 140, 220));

  StdGuiRender::set_font(0);
  float cy = y0 + 22.f;
  char line[256];

  auto drawLine = [&](E3DCOLOR col, const char *txt) {
    StdGuiRender::set_color(col);
    StdGuiRender::goto_xy(x0 + 12.f, cy);
    StdGuiRender::draw_str(txt);
    cy += 19.f;
  };

  snprintf(line, sizeof(line), "[daGI2 Static GI]  Pipeline: %s  |  API: %s",
    isThinGBuffer() ? "THIN GBUFFER (compatPC)" : "FULL DEFERRED", d3d::get_driver_name());
  drawLine(E3DCOLOR(130, 220, 255, 255), line);

  if (isBaking)
  {
    const float prog = clamp(bakedGISession.getProgress(), 0.f, 1.f);
    snprintf(line, sizeof(line), "STATUS: BAKING (%s)  %.1f%%  [%.1fs]",
      dagi_bake_runtime::tier_name(bakedGISession.isActive() ? bakedGISession.getTargetTier() : bakedGITargetTier),
      prog * 100.f, bakedGISession.getElapsedSeconds());
    drawLine(E3DCOLOR(255, 215, 70, 255), line);

    snprintf(line, sizeof(line), "Stage: %s  |  Tile %u / %u  |  Active Bricks: %u",
      bakedGISession.getStageName(), bakedGISession.getCurrentTile() + 1u, bakedGISession.getTotalTiles(),
      bakedGISession.getBakedBrickCount());
    drawLine(E3DCOLOR(230, 235, 240, 255), line);

    snprintf(line, sizeof(line), "Output: %s", bakedGISession.getOutputPath());
    drawLine(E3DCOLOR(180, 195, 210, 255), line);

    StdGuiRender::reset_textures();
    const float barX0 = x0 + 12.f;
    const float barX1 = x0 + boxW - 12.f;
    const float barY0 = cy - 6.f;
    const float barY1 = barY0 + 14.f;
    StdGuiRender::solid_frame(barX0, barY0, barX1, barY1, 1.f, E3DCOLOR(35, 42, 55, 240), E3DCOLOR(120, 140, 165, 240));
    const float fillW = (barX1 - barX0 - 4.f) * prog;
    if (fillW > 1.f)
    {
      StdGuiRender::set_color(E3DCOLOR(80, 220, 120, 255));
      StdGuiRender::render_box(barX0 + 2.f, barY0 + 2.f, barX0 + 2.f + fillW, barY1 - 2.f);
    }
  }
  else if (bakedGIVolume.isLoaded())
  {
    const bool smpOn = bakedGIVolume.isSamplingEnabled();
    snprintf(line, sizeof(line), "Mode: BAKED STATIC (%s)  |  Sampling: %s",
      dagi_bake_runtime::tier_name(bakedGIVolume.getLoadedTier()), smpOn ? "ON" : "PAUSED (Sky SH A/B)");
    drawLine(smpOn ? E3DCOLOR(90, 240, 130, 255) : E3DCOLOR(255, 140, 90, 255), line);

    const uint32_t nBricks = bakedGIVolume.getBrickCount();
    const uint32_t totBricks = max(bakedGIVolume.getTotalGridBricks(), 1u);
    const uint32_t at = bakedGIVolume.getAtlasTexels();
    const double vramMb = double(uint64_t(at) * at * (at * 2ull) * 8ull) / (1024.0 * 1024.0);
    snprintf(line, sizeof(line), "Bricks: %u / %u (%.1f%%)  |  Probe: %.2fm  |  Atlas: %ux%ux%u (%.1f MB)",
      nBricks, totBricks, 100.0 * double(nBricks) / double(totBricks), bakedGIVolume.getProbeSize(), at, at, at * 2u, vramMb);
    drawLine(E3DCOLOR(225, 230, 238, 255), line);

    int bx = -1, by = -1, bz = -1;
    uint16_t bIdx = 0xFFFFu;
    const Point3 camPos = currentFrameCamera.viewItm.getcol(3);
    const bool inGrid = bakedGIVolume.queryBrickAt(camPos, bx, by, bz, bIdx);
    if (!inGrid)
      snprintf(line, sizeof(line), "Cam (%.1f, %.1f, %.1f): OUTSIDE BAKED VOLUME  |  3D Vis: %d", camPos.x, camPos.y, camPos.z,
        gi_baked_debug_vis.get());
    else if (bIdx == 0xFFFFu)
      snprintf(line, sizeof(line), "Cam Brick (%d,%d,%d): EMPTY (Sky Fallback)  |  3D Vis: %d", bx, by, bz, gi_baked_debug_vis.get());
    else
      snprintf(line, sizeof(line), "Cam Brick (%d,%d,%d): ACTIVE #%u  |  3D Vis: %d ('gi.debug_vis 1/2')", bx, by, bz, unsigned(bIdx),
        gi_baked_debug_vis.get());
    drawLine(E3DCOLOR(200, 215, 230, 255), line);

    if (bakedGIPromptRebuildHigh)
      drawLine(E3DCOLOR(255, 200, 90, 255), "Notice: High tier missing; loaded Normal tier (run 'gi.bake high')");
    else
      drawLine(E3DCOLOR(150, 165, 185, 255), "Cmds: gi.debug_vis [0..3] | gi.baked_ab | gi.bake [normal|high]");
  }
  else
  {
    snprintf(line, sizeof(line), "Mode: LIVE DYNAMIC daGI2 (%s)", daGI2 ? "ACTIVE" : "OFF");
    drawLine(E3DCOLOR(255, 205, 95, 255), line);
    snprintf(line, sizeof(line), "Level: '%s'  |  History frames: %u", resolveBakeLevelId().c_str(), getGIHistoryFrames());
    drawLine(E3DCOLOR(220, 225, 235, 255), line);
    drawLine(E3DCOLOR(150, 165, 185, 255), "Cmds: gi.bake [normal|high] | gi.bake_status | gi.debug_hud 0");
  }
}

void WorldRenderer::renderBakedGIImGui()
{
  const eastl::string levelId = resolveBakeLevelId();
  const eastl::string dir = resolveBakeCacheDir();
  const eastl::string nPath = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::Normal);
  const eastl::string hPath = dagi_bake_runtime::make_cache_path(dir.c_str(), levelId.c_str(), dagi_bake_runtime::Tier::High);
  const bool nValid = dagi_bake_runtime::probe_cache_valid(nPath.c_str(), 0, 0);
  const bool hValid = dagi_bake_runtime::probe_cache_valid(hPath.c_str(), 0, 0);

  ImGui::Text("Pipeline: %s  |  Driver: %s", isThinGBuffer() ? "Thin GBuffer (compatPC)" : "Full Deferred", d3d::get_driver_name());
  ImGui::Text("Level: %s", levelId.c_str());
  ImGui::Separator();

  bool hudOn = gi_baked_debug_hud.get();
  if (ImGui::Checkbox("Show On-Screen HUD Overlay (gi.debug_hud)", &hudOn))
    gi_baked_debug_hud.set(hudOn);

  if (bakedGIVolume.isLoaded())
  {
    bool smpOn = bakedGIVolume.isSamplingEnabled();
    if (ImGui::Checkbox("Enable Baked GI Sampling (A/B Compare vs Sky SH)", &smpOn))
      bakedGIVolume.setSamplingEnabled(smpOn);

    uint32_t gx = 0, gy = 0, gz = 0;
    bakedGIVolume.getGridDim(gx, gy, gz);
    const uint32_t at = bakedGIVolume.getAtlasTexels();
    const double vramMb = double(uint64_t(at) * at * (at * 2ull) * 8ull) / (1024.0 * 1024.0);
    ImGui::TextColored(ImVec4(0.4f, 0.95f, 0.55f, 1.f), "Active Mode: BAKED STATIC (tier=%s)",
      dagi_bake_runtime::tier_name(bakedGIVolume.getLoadedTier()));
    ImGui::Text("Grid Bricks: %u x %u x %u (%u active / %u total)", gx, gy, gz, bakedGIVolume.getBrickCount(),
      bakedGIVolume.getTotalGridBricks());
    ImGui::Text("Probe Spacing: %.2f m  |  Atlas: %ux%ux%u (%.2f MB)", bakedGIVolume.getProbeSize(), at, at, at * 2u, vramMb);
  }
  else if (isBakedGIBaking())
  {
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "BAKING: %s (Tile %u/%u, %.1fs)",
      bakedGISession.getStageName(), bakedGISession.getCurrentTile() + 1u, bakedGISession.getTotalTiles(),
      bakedGISession.getElapsedSeconds());
    ImGui::ProgressBar(bakedGISession.getProgress());
    if (ImGui::Button("Cancel Bake"))
    {
      bakedGISession.cancel();
      bakedGIPendingStart = false;
      setGIQualityFromSettings();
    }
  }
  else
  {
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "Active Mode: LIVE DYNAMIC daGI2 (%s)", daGI2 ? "ON" : "OFF");
  }

  ImGui::Separator();
  ImGui::Text("3D In-World Debug Visualization:");
  int visMode = gi_baked_debug_vis.get();
  const char *visItems[] = {"Off", "3D SH1 Probe Spheres", "Active Sparse Brick Wireframes", "Probes + Brick Wireframes"};
  if (ImGui::Combo("Debug Mode (gi.debug_vis)", &visMode, visItems, IM_ARRAYSIZE(visItems)))
    gi_baked_debug_vis.set(visMode);
  float visRad = gi_baked_debug_radius.get();
  if (ImGui::SliderFloat("Debug Radius (m)", &visRad, 6.f, 128.f, "%.1f"))
    gi_baked_debug_radius.set(visRad);
  float sphScale = gi_baked_debug_sphere_scale.get();
  if (ImGui::SliderFloat("Probe Sphere Scale", &sphScale, 0.02f, 0.35f, "%.2f"))
    gi_baked_debug_sphere_scale.set(sphScale);

  ImGui::Separator();
  ImGui::Text("Cache Files:");
  ImGui::BulletText("Normal: %s [%s]", nPath.c_str(), nValid ? "VALID" : "MISSING");
  ImGui::BulletText("High:   %s [%s]", hPath.c_str(), hValid ? "VALID" : "MISSING");

  if (!isBakedGIBaking())
  {
    if (ImGui::Button("Bake Normal (64 rays)"))
      triggerBakedGIRebuild(dagi_bake_runtime::Tier::Normal);
    ImGui::SameLine();
    if (ImGui::Button("Bake High (128 rays)"))
      triggerBakedGIRebuild(dagi_bake_runtime::Tier::High);
    if (ImGui::Button("Delete Normal Cache"))
      deleteBakedGICache("normal");
    ImGui::SameLine();
    if (ImGui::Button("Delete High Cache"))
      deleteBakedGICache("high");
  }
}

ECS_TAG(render)
ECS_NO_ORDER
static void baked_gi_hud_es(const RenderEventUI &)
{
  if (auto *wr = static_cast<WorldRenderer *>(get_world_renderer()))
    wr->renderBakedGIDebugHUD();
}

ECS_TAG(render, dev)
ECS_NO_ORDER
static void baked_gi_render_debug_es(const UpdateStageInfoRenderDebug &)
{
  if (auto *wr = static_cast<WorldRenderer *>(get_world_renderer()))
    wr->renderBakedGIDebugBricks();
}

static void baked_gi_imgui_window()
{
  if (auto *wr = static_cast<WorldRenderer *>(get_world_renderer()))
    wr->renderBakedGIImGui();
}

REGISTER_IMGUI_WINDOW("Render", "daGI2 Baked GI", baked_gi_imgui_window);
