//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include <daPRTGI/daPRTGI.h>
#include "prtgi_data.h"
#include "prtgi_baker.h"
#include "prtgi_scheduler.h"
#include "prtgi_residency.h"
#include "prtgi_relight.h"
#include "prtgi_debug.h"

#include <ioSys/dag_dataBlock.h>
#include <ioSys/dag_fileIo.h>
#include <osApiWrappers/dag_files.h>
#include <osApiWrappers/dag_direct.h>
#include <perfMon/dag_statDrv.h>
#include <debug/dag_debug.h>
#include <gui/dag_imgui.h>

namespace daprtgi
{

class DaPRTGIImpl : public IDaPRTGI
{
public:
  DaPRTGIImpl() = default;
  virtual ~DaPRTGIImpl() { close(); }

  void init(const DataBlock *settings) override
  {
    close();

    if (settings)
    {
      const DataBlock *prtBlk = settings->getBlockByNameEx("prtgi");
      currentSettings.enabled = prtBlk->getBool("enabled", currentSettings.enabled);
      currentSettings.debugOverlay = prtBlk->getBool("debug", currentSettings.debugOverlay);
      currentSettings.gridSpacing = prtBlk->getReal("gridSpacing", currentSettings.gridSpacing);
      currentSettings.sectorSize = prtBlk->getInt("sectorSize", currentSettings.sectorSize);
      currentSettings.cascadeCount = prtBlk->getInt("cascadeCount", currentSettings.cascadeCount);
      currentSettings.updateBudget = prtBlk->getInt("updateBudget", currentSettings.updateBudget);
      currentSettings.skyIntensity = prtBlk->getReal("skyIntensity", currentSettings.skyIntensity);

      const char *bModeStr = prtBlk->getStr("bakingMode", "auto");
      if (stricmp(bModeStr, "offline") == 0 || stricmp(bModeStr, "load_only") == 0)
        currentSettings.bakingMode = BakingMode::LoadOnly;
      else if (stricmp(bModeStr, "force_bake") == 0)
        currentSettings.bakingMode = BakingMode::ForceBake;
      else
        currentSettings.bakingMode = BakingMode::Auto;

      currentSettings.cacheFolder = prtBlk->getStr("cacheFolder", currentSettings.cacheFolder.c_str());

      if (prtBlk->getBool("debugDrawProbes", false))
        currentSettings.debugDraw = DebugVisualMode::Probes;
    }

    relightPass.init();
    debug("PRTGI: Initialized (enabled=%d, debugOverlay=%d, spacing=%.2f, budget=%d)",
      currentSettings.enabled, currentSettings.debugOverlay, currentSettings.gridSpacing, currentSettings.updateBudget);

    stats.bakingStatus = "Initialized (Awaiting Scene)";
  }

  void close() override
  {
    residency.release();
    relightPass.close();
    asset.clear();
    scheduler.reset(nullptr);
    ready = false;
    stats.bakingStatus = "Closed";
  }

  void setSettings(const PRTGISettings &s) override { currentSettings = s; }
  const PRTGISettings &getSettings() const override { return currentSettings; }
  const PRTGIStats &getStats() const override { return stats; }

  bool isReady() const override { return ready; }

  bool loadOrBakeScene(const char *scene_name, bool force_bake) override
  {
    if (!currentSettings.enabled)
    {
      stats.bakingStatus = "Disabled";
      return false;
    }

    char cleanNameBuf[256];
    const char *cleanName = "east_district_demo";
    if (scene_name && scene_name[0])
      cleanName = dd_get_fname_without_path_and_ext(cleanNameBuf, sizeof(cleanNameBuf), scene_name);

    currentSceneName = cleanName;

    dd_mkdir(currentSettings.cacheFolder.c_str());

    eastl::string filePath;
    filePath.sprintf("%s/%s.prtgi.bin", currentSettings.cacheFolder.c_str(), cleanName);

    bool loadSuccess = false;
    if (!force_bake && currentSettings.bakingMode != BakingMode::ForceBake)
    {
      if (dd_file_exist(filePath.c_str()))
      {
        stats.bakingStatus = "Loading from Cache...";
        FullFileLoadCB loadHandle(filePath.c_str());
        if (loadHandle.fileHandle)
        {
          loadSuccess = asset.deserialize(loadHandle);
          if (loadSuccess)
          {
            uint64_t currentHash = baker.computeCurrentSceneGeometryHash();
            if (currentHash != asset.geometryHash)
            {
              logwarn("PRTGI: Cached geometry hash mismatch (cache=0x%llX vs current=0x%llX). Requires rebake.",
                asset.geometryHash, currentHash);
              loadSuccess = false;
            }
          }
        }
      }
    }

    if (!loadSuccess)
    {
      if (currentSettings.bakingMode == BakingMode::LoadOnly)
      {
        logerr("PRTGI: Asset %s not found in LoadOnly mode!", filePath.c_str());
        stats.bakingStatus = "Cache Missing (LoadOnly)";
        return false;
      }

      stats.bakingStatus = "Baking on Load...";
      debug("PRTGI: Baking on load for scene '%s'...", currentSceneName.c_str());

      int64_t tStart = profile_ref_ticks();
      bool bakeSuccess = baker.bakeCurrentScene(asset, currentSettings.gridSpacing, currentSettings.sectorSize,
        [this](const char *step, float prog) {
          stats.bakingStatus = step;
          debug("PRTGI Bake Progress [%.0f%%]: %s", prog * 100.0f, step);
        }, currentSettings.cacheFolder.c_str());

      if (!bakeSuccess)
      {
        stats.bakingStatus = "Bake Failed";
        return false;
      }

      stats.lastBakeDurationSec = profile_time_usec(tStart) / 1000000.0f;
    }

    // Initialize scheduler & residency
    scheduler.reset(&asset);
    residency.init(asset.grid, currentSettings.cascadeCount);

    ready = true;
    stats.bakingStatus = loadSuccess ? "Loaded from Cache (Ready)" : "Baked & Ready";
    updateStatsCache();

    if (!loadSuccess)
      saveScene(currentSceneName.c_str());

    debug("PRTGI: Scene '%s' ready. %u probes, %u sectors.", currentSceneName.c_str(), stats.totalProbes, stats.totalSectors);
    return true;
  }

  bool saveScene(const char *scene_name) override
  {
    if (asset.sectors.empty())
      return false;

    char cleanNameBuf[256];
    const char *targetName = currentSceneName.c_str();
    if (scene_name && scene_name[0])
      targetName = dd_get_fname_without_path_and_ext(cleanNameBuf, sizeof(cleanNameBuf), scene_name);

    dd_mkdir(currentSettings.cacheFolder.c_str());

    eastl::string path;
    path.sprintf("%s/%s.prtgi.bin", currentSettings.cacheFolder.c_str(), targetName);

    FullFileSaveCB saveHandle(path.c_str());
    if (!saveHandle.fileHandle)
    {
      logwarn("PRTGI: Failed to open %s for saving!", path.c_str());
      return false;
    }

    bool res = asset.serialize(saveHandle);
    debug("PRTGI: Saved scene data to %s (success=%d)", path.c_str(), res);
    return res;
  }

  void update(const Point3 &camera_pos) override
  {
    if (!ready || !currentSettings.enabled)
      return;

    // Schedule sectors according to camera position and budget
    activeRequestedSectors = scheduler.select(camera_pos, currentSettings.updateBudget);
    stats.currentNearSector = scheduler.getNearSector();
    stats.currentBackgroundSector = scheduler.getBackgroundSector();
    stats.residentSectors = residency.getResidentCount();
  }

  void relight(const Point3 &camera_pos) override
  {
    if (!ready || !currentSettings.enabled)
      return;

    int64_t tStart = profile_ref_ticks();

    relightPass.relightSectors(activeRequestedSectors, asset, residency, camera_pos, currentSettings.skyIntensity);

    for (int sec : activeRequestedSectors)
      scheduler.updated(sec);

    stats.lastRelightDurationMs = profile_time_usec(tStart) / 1000.0f;
    stats.updatedSectorsThisFrame = (uint32_t)activeRequestedSectors.size();
  }

  void updateConstants() override
  {
    if (!ready || !currentSettings.enabled)
      return;

    relightPass.updateGlobalShaderConstants(asset.grid, residency, Point3(0, 0, 0), currentSettings.cascadeCount);
  }

  void afterReset() override
  {
    if (ready)
    {
      residency.afterReset();
      updateConstants();
    }
  }

  void debugRender() override
  {
    if (!ready) return;
    debugRenderer.drawWorldDebug(asset, residency, currentSettings.debugDraw, lastCameraPos);
  }

  void drawDebugOverlay() override
  {
    if (!ready && !currentSettings.debugOverlay) return;
    debugRenderer.drawOnScreenHUD(stats, currentSettings);
  }

  void drawImGuiWindow() override
  {
    debugRenderer.drawImGuiWindow(stats, currentSettings);
  }

  void setDebugOverlay(bool enabled) override { currentSettings.debugOverlay = enabled; }
  void setDebugDrawMode(DebugVisualMode mode) override { currentSettings.debugDraw = mode; }

private:
  void updateStatsCache()
  {
    stats.totalProbes = asset.grid.totalProbes();
    stats.totalSectors = (uint32_t)asset.sectors.size();
    stats.gridOrigin = asset.grid.origin;
    stats.gridCount = asset.grid.count;
    stats.totalSurfels = 0;
    stats.totalBricks = 0;
    stats.factorsMemoryBytes = 0;

    for (const auto &sec : asset.sectors)
    {
      stats.totalSurfels += (uint32_t)sec.surfels.size();
      stats.totalBricks += (uint32_t)sec.brickInfo.size();
      stats.factorsMemoryBytes += sec.factors.size() * sizeof(BrickFactor);
    }
  }

  PRTGISettings currentSettings;
  PRTGIStats stats;
  PRTAsset asset;
  PRTBaker baker;
  PRTSectorScheduler scheduler;
  PRTSectorResidency residency;
  PRTRelightPass relightPass;
  PRTGIDebugRenderer debugRenderer;

  eastl::string currentSceneName = "default";
  eastl::vector<int> activeRequestedSectors;
  Point3 lastCameraPos = Point3(0, 0, 0);
  bool ready = false;
};

IDaPRTGI *create_prtgi()
{
  return new DaPRTGIImpl();
}

} // namespace daprtgi

