// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Baked GI cache file: naming, header format, sparse brick payload and tier/source selection.
// Engine independent on purpose, so the rules can be tested without the renderer.
//
// Binary layout (little-endian .dgibake):
//   [Prefix]   "DGIBAKE\0" (8B) + headerSize (u32)
//   [Header]   formatVersion (u32), producerVersion (u32), tier (u8), pad (3B),
//              levelIdLen (u16) + levelId[], contentHash (u64), lightingSignature (u64),
//              stateCount (u32), brickSize (u32 = 8), brickCount (u32),
//              payloadSize (u64), payloadChecksum (FNV-1a u64)
//   [Payload]  32B meta (origin[3], probeSize, gridDim[3], brickCount)
//            + brickCount * 8B BrickCoord {x, y, z, flags}
//            + brickCount * 512 * 4B sph0 (RGBA8)
//            + brickCount * 512 * 8B sph1 (RGBA16F)
namespace dagi_bake
{
enum class Tier : uint8_t
{
  Normal = 0,
  High = 1,
  None = 0xFF
};

enum class SourceMode : uint8_t
{
  Auto = 0,    // use baked cache if valid on disk; fall back to dynamic on full deferred when deleted, or rebuild on thin
  Baked = 1,   // force static baked cache (rebuilds normal tier at load if missing)
  Dynamic = 2  // force live dynamic daGI2 (full deferred only; ignored on thin gbuffer)
};

constexpr uint32_t FORMAT_VERSION = 1;
constexpr uint32_t MAX_LEVEL_ID_LEN = 64;

constexpr uint32_t BRICK_PROBES_SIDE = 8;
constexpr uint32_t BRICK_PROBES_COUNT = BRICK_PROBES_SIDE * BRICK_PROBES_SIDE * BRICK_PROBES_SIDE; // 512
constexpr uint32_t PROBE_SPH0_BYTES = 4;  // RGBA8: amb.yz, up.yz
constexpr uint32_t PROBE_SPH1_BYTES = 8;  // RGBA16F: amb.x, dir.xyz
constexpr uint32_t BRICK_RECORD_BYTES = 8 + BRICK_PROBES_COUNT * (PROBE_SPH0_BYTES + PROBE_SPH1_BYTES); // coord + probes
constexpr uint16_t INVALID_BRICK_INDEX = 0xFFFFu;

const char *tier_name(Tier tier);
bool parse_tier(const char *name, Tier &out);

const char *source_mode_name(SourceMode mode);
bool parse_source_mode(const char *name, SourceMode &out);

// <level>_gi_<tier>.dgibake, the two tiers never share a file name
std::string cache_file_name(const std::string &level_id, Tier tier);

struct Header
{
  uint32_t formatVersion = FORMAT_VERSION;
  uint32_t producerVersion = 0; // engine and shader hash of the producer
  Tier tier = Tier::Normal;
  std::string levelId;
  uint64_t contentHash = 0;       // static level content
  uint64_t lightingSignature = 0; // time, sun, sky, fixed lights
  uint32_t stateCount = 1;        // lighting states in this file, one for now
  uint32_t brickSize = BRICK_PROBES_SIDE; // probes per brick side
  uint32_t brickCount = 0;
  uint64_t payloadSize = 0;
  uint64_t payloadChecksum = 0;
};

struct BrickCoord
{
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t z = 0;
  uint16_t flags = 0;
};

struct VolumeData
{
  float origin[3] = {0.f, 0.f, 0.f}; // world-space min corner (x, y, z)
  float probeSize = 1.f;             // world-space distance between adjacent probes
  uint32_t gridDim[3] = {0, 0, 0};   // brick grid dimensions (bricksX, bricksY, bricksZ)
  std::vector<BrickCoord> bricks;    // non-empty brick coordinates, size == brickCount
  std::vector<uint32_t> sph0;        // brickCount * BRICK_PROBES_COUNT RGBA8 texels
  std::vector<uint64_t> sph1;        // brickCount * BRICK_PROBES_COUNT RGBA16F texels
};

uint64_t checksum(const void *data, size_t size); // FNV-1a 64

bool pack_volume_payload(const VolumeData &vol, std::vector<uint8_t> &payload, std::string *err);
bool unpack_volume_payload(const uint8_t *data, size_t size, uint32_t expected_bricks, VolumeData &vol, std::string *err);

// Writes header followed by payload. Returns false and fills err on failure.
bool write_file(const std::string &path, const Header &header, const std::vector<uint8_t> &payload, std::string *err);
bool write_volume(const std::string &path, Header header, const VolumeData &vol, std::string *err);

// Reads and validates the header only (magic, version, payload size against the file size).
bool read_header(const std::string &path, Header &header, std::string *err);
// Reads the payload and compares it with the header checksum. Used at packaging and in validation, not at startup.
bool verify_payload(const std::string &path, const Header &header, std::string *err);
// Reads header + payload and unpacks VolumeData (optionally verifying checksum).
bool read_volume(const std::string &path, Header &header, VolumeData &vol, bool check_hash, std::string *err);

// Result of the load decision at level load.
struct Selection
{
  Tier loadTier = Tier::None;      // the tier used for this session (None when useDynamic is true)
  bool useDynamic = false;         // use live dynamic daGI2 instead of baked cache
  bool rebuildFirst = false;       // a file has to be built on this machine before loading
  Tier rebuildTier = Tier::None;   // the tier that gets rebuilt
  bool promptRebuildHigh = false;  // show "high tier can be rebuilt" in the UI
};

// Pure baked selection (equivalent to SourceMode::Baked with canUseDynamic = false).
// Option A: when the configured high tier is missing, the normal tier is loaded and the user is prompted.
Selection select(Tier configured, bool normalValid, bool highValid);

// Full pipeline-aware selection supporting dynamic fallback when .dgibake files are deleted on Full Deferred,
// mandatory baked mode on Thin GBuffer, and explicit rebuild requests.
Selection select(Tier configured, SourceMode sourceMode, bool canUseDynamic, bool normalValid, bool highValid,
  bool forceRebuild = false);
} // namespace dagi_bake
