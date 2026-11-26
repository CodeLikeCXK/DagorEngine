// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <cstdint>
#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/fixed_function.h>
#include <EASTL/vector.h>
#include <math/dag_bounds3.h>
#include <math/dag_TMatrix.h>
#include <daGI2/daGI2.h>

namespace dagi_bake_runtime
{
enum class Tier : uint8_t
{
  Normal = 0,
  High = 1,
  None = 0xFF
};

enum class SourceMode : uint8_t
{
  Auto = 0,    // use baked if present; fall back to dynamic on Full Deferred when deleted, or rebuild on Thin GBuffer
  Baked = 1,   // force static baked mode (rebuild normal tier if missing)
  Dynamic = 2  // force dynamic GI (Full Deferred only; Thin GBuffer forces Baked)
};

struct Selection
{
  Tier loadTier = Tier::None;
  bool useDynamic = false;
  bool rebuildFirst = false;
  Tier rebuildTier = Tier::None;
  bool promptRebuildHigh = false;
};

const char *tier_name(Tier t);
bool parse_tier(const char *s, Tier &out);
const char *source_mode_name(SourceMode m);
bool parse_source_mode(const char *s, SourceMode &out);

eastl::string make_cache_path(const char *dir, const char *level_id, Tier tier);
bool probe_cache_valid(const char *path, uint64_t expected_content_hash, uint64_t expected_lighting_sig);

Selection select_mode(Tier configured, SourceMode source_mode, bool can_use_dynamic, bool normal_valid, bool high_valid,
  bool force_rebuild = false);

// Holds the GPU 3D sparse brick atlas + indirection buffer and binds shader globals for static GI sampling.
class BakedGIVolume
{
public:
  BakedGIVolume();
  ~BakedGIVolume();

  void ensureFallbackBound();
  bool loadFromFile(const char *path, uint64_t expected_content_hash, uint64_t expected_lighting_sig, bool verify_checksum = false);
  void unload();
  void afterReset();

  bool isLoaded() const;
  Tier getLoadedTier() const;
  uint32_t getBrickCount() const;
  uint32_t getTotalGridBricks() const;
  void getGridDim(uint32_t &gx, uint32_t &gy, uint32_t &gz) const;
  uint32_t getAtlasTexels() const;
  float getProbeSize() const;
  const BBox3 &getWorldBounds() const;

  void setSamplingEnabled(bool enabled);
  bool isSamplingEnabled() const;

  // Returns true if world_pos is inside grid bounds; sets brick coords and brick_idx (0xFFFF if empty).
  bool queryBrickAt(const Point3 &world_pos, int &bx, int &by, int &bz, uint16_t &brick_idx) const;

  // 3D in-game debug visualizers (call during UpdateStageInfoRenderDebug)
  void drawDebugBricks(const Point3 &cam_pos, float radius) const;
  void drawDebugProbes(const Point3 &cam_pos, float radius, float sphere_scale);

  struct Impl;

private:
  friend class BakedGISession;
  eastl::unique_ptr<Impl> impl;
};

struct BakeParams
{
  Tier tier = Tier::Normal;
  eastl::string levelId;
  eastl::string outputPath;
  uint64_t contentHash = 0;
  uint64_t lightingSignature = 0;
  BBox3 worldBox;
  float probeSizeOverride = 0.f; // <= 0 uses tier default (Normal: 1.0m, High: 0.5m)
  eastl::vector<TMatrix> indoorInvTms; // inverse OBB transforms ([-0.5, 0.5]^3 local space) for indoor isolation
};

// Frame-driven non-blocking GPU baker (prevents WDDM 2s TDR timeout while updating progress each frame).
class BakedGISession
{
public:
  using RasterizeTileFn = eastl::fixed_function<64, void(DaGI &dagi, const Point3 &tile_center)>;

  BakedGISession();
  ~BakedGISession();

  bool start(const BakeParams &params, DaGI &dagi);
  // Runs one bounded slice of GPU bake work (~15-40ms). Returns true while still running, false when finished or failed.
  bool tick(DaGI &dagi, const RasterizeTileFn &rasterize_tile_cb, BakedGIVolume &out_volume);
  void cancel();

  bool isActive() const;
  bool succeeded() const;
  float getProgress() const; // 0..1
  double getElapsedSeconds() const;
  const char *getStageName() const;
  uint32_t getCurrentTile() const;
  uint32_t getTotalTiles() const;
  uint32_t getBakedBrickCount() const;
  Tier getTargetTier() const;
  const char *getOutputPath() const;

  struct Impl;

private:
  eastl::unique_ptr<Impl> impl;
};
} // namespace dagi_bake_runtime
