//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <math/dag_Point3.h>
#include <math/integer/dag_IPoint3.h>
#include <math/dag_bounds3.h>
#include <EASTL/string.h>

class DataBlock;
class BaseTexture;

namespace daprtgi
{

enum class BakingMode
{
  Auto,       // Load cached asset if valid; otherwise bake on load
  LoadOnly,   // Strictly load from asset; fail if missing
  ForceBake   // Always rebake static scene
};

enum class DebugVisualMode
{
  None,
  Probes,
  Cascades,
  Sectors,
  ValidityMask,
  Irradiance
};

struct PRTGISettings
{
  bool enabled = true;
  bool debugOverlay = false;       // Show on-screen stats HUD & ImGui window
  DebugVisualMode debugDraw = DebugVisualMode::None; // 3D world visualization
  
  float gridSpacing = 2.0f;        // Probe grid spacing in meters
  int sectorSize = 32;             // Probes per sector axis (X and Z)
  int cascadeCount = 3;            // Cascades of 3D volume textures (finer -> coarser)
  int updateBudget = 2;            // Number of sectors to relight per frame
  float skyIntensity = 1.0f;       // Sky environment intensity multiplier
  BakingMode bakingMode = BakingMode::Auto;
  eastl::string cacheFolder = "prtgi";
};

struct PRTGIStats
{
  const char *bakingStatus = "Uninitialized";
  const char *activeSystemName = "PRTGI";
  const char *pipelineTypeName = "Full Deferred";
  
  uint32_t totalProbes = 0;
  uint32_t totalSectors = 0;
  uint32_t residentSectors = 0;
  uint32_t updatedSectorsThisFrame = 0;
  uint32_t totalSurfels = 0;
  uint32_t totalBricks = 0;
  size_t factorsMemoryBytes = 0;
  
  float lastRelightDurationMs = 0.0f;
  float lastBakeDurationSec = 0.0f;
  
  int currentNearSector = -1;
  int currentBackgroundSector = -1;
  
  Point3 gridOrigin = Point3(0, 0, 0);
  IPoint3 gridCount = IPoint3(0, 0, 0);
  BBox3 sceneBounds = BBox3();
};

class IDaPRTGI
{
public:
  virtual ~IDaPRTGI() = default;

  virtual void init(const DataBlock *settings) = 0;
  virtual void close() = 0;

  virtual void setSettings(const PRTGISettings &s) = 0;
  virtual const PRTGISettings &getSettings() const = 0;
  virtual const PRTGIStats &getStats() const = 0;

  // Scene bake / load / save lifecycle
  virtual bool loadOrBakeScene(const char *scene_name, bool force_bake = false) = 0;
  virtual bool saveScene(const char *scene_name = nullptr) = 0;
  virtual bool isReady() const = 0;

  // Runtime frame updates
  virtual void update(const Point3 &camera_pos) = 0;
  virtual void relight(const Point3 &camera_pos) = 0;
  virtual void updateConstants() = 0;
  virtual void afterReset() = 0;

  // Debugging & HUD
  virtual void debugRender() = 0;
  virtual void drawDebugOverlay() = 0;
  virtual void drawImGuiWindow() = 0;
  virtual void setDebugOverlay(bool enabled) = 0;
  virtual void setDebugDrawMode(DebugVisualMode mode) = 0;
};

IDaPRTGI *create_prtgi();

} // namespace daprtgi

