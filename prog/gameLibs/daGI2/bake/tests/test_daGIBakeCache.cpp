// Standalone test for daGIBakeCache. Not part of the engine build (the daGI2 jamfile globs only its own folder).
// Build: cl /std:c++17 /EHsc /W4 /I.. test_daGIBakeCache.cpp ..\daGIBakeCache.cpp
#include "../daGIBakeCache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace dagi_bake;

static int g_failed = 0;

#define CHECK(cond)                                                    \
  do                                                                   \
  {                                                                    \
    if (!(cond))                                                       \
    {                                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failed;                                                      \
    }                                                                  \
  } while (0)

static void test_names_and_tiers()
{
  CHECK(cache_file_name("east_district", Tier::Normal) == "east_district_gi_normal.dgibake");
  CHECK(cache_file_name("east_district", Tier::High) == "east_district_gi_high.dgibake");
  CHECK(std::string(tier_name(Tier::Normal)) == "normal");
  CHECK(std::string(tier_name(Tier::High)) == "high");
  Tier t = Tier::None;
  CHECK(parse_tier("normal", t) && t == Tier::Normal);
  CHECK(parse_tier("high", t) && t == Tier::High);
  CHECK(!parse_tier("Normal", t));
  CHECK(!parse_tier("", t));
  CHECK(!parse_tier(nullptr, t));

  SourceMode m = SourceMode::Baked;
  CHECK(parse_source_mode("auto", m) && m == SourceMode::Auto);
  CHECK(parse_source_mode("baked", m) && m == SourceMode::Baked);
  CHECK(parse_source_mode("dynamic", m) && m == SourceMode::Dynamic);
  CHECK(!parse_source_mode("invalid", m));
}

static void test_checksum()
{
  // FNV-1a 64 reference values
  CHECK(checksum("", 0) == 14695981039346656037ull);
  CHECK(checksum("a", 1) == 0xaf63dc4c8601ec8cull);
}

static void test_roundtrip_and_corruption()
{
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dgibake_test";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / cache_file_name("east_district", Tier::High)).string();

  Header h;
  h.producerVersion = 0x1234abcd;
  h.tier = Tier::High;
  h.levelId = "east_district";
  h.contentHash = 0x0102030405060708ull;
  h.lightingSignature = 0x1122334455667788ull;
  h.brickSize = 8;
  h.brickCount = 42;

  std::vector<uint8_t> payload(1000);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = uint8_t(i * 31 + 7);

  std::string err;
  CHECK(write_file(path, h, payload, &err));

  Header r;
  CHECK(read_header(path, r, &err));
  CHECK(r.formatVersion == FORMAT_VERSION);
  CHECK(r.producerVersion == 0x1234abcd);
  CHECK(r.tier == Tier::High);
  CHECK(r.levelId == "east_district");
  CHECK(r.contentHash == 0x0102030405060708ull);
  CHECK(r.lightingSignature == 0x1122334455667788ull);
  CHECK(r.stateCount == 1);
  CHECK(r.brickSize == 8);
  CHECK(r.brickCount == 42);
  CHECK(r.payloadSize == payload.size());
  CHECK(verify_payload(path, r, &err));

  // flip one payload byte: header still reads, checksum must fail
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-10, std::ios::end);
    char c = 0;
    f.read(&c, 1);
    f.seekp(-10, std::ios::end);
    c = char(c ^ 0x55);
    f.write(&c, 1);
  }
  CHECK(read_header(path, r, &err));
  CHECK(!verify_payload(path, r, &err));

  // append one byte: header size no longer matches the file
  {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f.put('x');
  }
  CHECK(!read_header(path, r, &err));

  // bad magic
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(0, std::ios::beg);
    f.put('X');
  }
  CHECK(!read_header(path, r, &err));

  // sparse brick volume roundtrip
  const std::string volPath = (dir / cache_file_name("east_district", Tier::Normal)).string();
  VolumeData vol;
  vol.origin[0] = -128.f;
  vol.origin[1] = -16.f;
  vol.origin[2] = -256.f;
  vol.probeSize = 1.5f;
  vol.gridDim[0] = 16;
  vol.gridDim[1] = 4;
  vol.gridDim[2] = 32;
  vol.bricks = {{0, 0, 0, 0}, {15, 3, 31, 1}};
  vol.sph0.resize(2 * BRICK_PROBES_COUNT, 0x80402010u);
  vol.sph1.resize(2 * BRICK_PROBES_COUNT, 0x3c003c003c003c00ull);
  vol.sph0[513] = 0xdeadbeefu;
  vol.sph1[513] = 0x1122334455667788ull;

  Header vh;
  vh.tier = Tier::Normal;
  vh.levelId = "east_district";
  CHECK(write_volume(volPath, vh, vol, &err));

  Header vr;
  VolumeData volOut;
  CHECK(read_volume(volPath, vr, volOut, true, &err));
  CHECK(vr.brickCount == 2);
  CHECK(volOut.probeSize == 1.5f);
  CHECK(volOut.gridDim[0] == 16 && volOut.gridDim[1] == 4 && volOut.gridDim[2] == 32);
  CHECK(volOut.bricks.size() == 2 && volOut.bricks[1].x == 15 && volOut.bricks[1].z == 31);
  CHECK(volOut.sph0[513] == 0xdeadbeefu);
  CHECK(volOut.sph1[513] == 0x1122334455667788ull);

  std::filesystem::remove_all(dir);
}

static void test_selection()
{
  // normal configured (pure baked / thin gbuffer)
  Selection s = select(Tier::Normal, true, true);
  CHECK(s.loadTier == Tier::Normal && !s.useDynamic && !s.rebuildFirst && !s.promptRebuildHigh);

  s = select(Tier::Normal, true, false);
  CHECK(s.loadTier == Tier::Normal && !s.useDynamic && !s.rebuildFirst);

  s = select(Tier::Normal, false, true);
  CHECK(s.loadTier == Tier::Normal && s.rebuildFirst && s.rebuildTier == Tier::Normal && !s.promptRebuildHigh);

  s = select(Tier::Normal, false, false);
  CHECK(s.loadTier == Tier::Normal && s.rebuildFirst && s.rebuildTier == Tier::Normal);

  // high configured, option A
  s = select(Tier::High, true, true);
  CHECK(s.loadTier == Tier::High && !s.rebuildFirst && !s.promptRebuildHigh);

  s = select(Tier::High, true, false);
  CHECK(s.loadTier == Tier::Normal && !s.rebuildFirst && s.promptRebuildHigh);

  s = select(Tier::High, false, true);
  CHECK(s.loadTier == Tier::High && !s.rebuildFirst && !s.promptRebuildHigh);

  s = select(Tier::High, false, false);
  CHECK(s.loadTier == Tier::Normal && s.rebuildFirst && s.rebuildTier == Tier::Normal && s.promptRebuildHigh);

  // Full Deferred + Auto: deleting .dgibake falls back to live dynamic GI without forcing rebuild
  s = select(Tier::Normal, SourceMode::Auto, /*canUseDynamic=*/true, /*normalValid=*/false, /*highValid=*/false);
  CHECK(s.useDynamic && s.loadTier == Tier::None && !s.rebuildFirst);

  s = select(Tier::High, SourceMode::Auto, /*canUseDynamic=*/true, /*normalValid=*/true, /*highValid=*/false);
  CHECK(!s.useDynamic && s.loadTier == Tier::Normal && s.promptRebuildHigh && !s.rebuildFirst);

  s = select(Tier::High, SourceMode::Auto, /*canUseDynamic=*/true, /*normalValid=*/false, /*highValid=*/false);
  CHECK(s.useDynamic && s.loadTier == Tier::None && !s.rebuildFirst);

  // Thin GBuffer ignores Dynamic sourceMode and forces rebuild when deleted
  s = select(Tier::Normal, SourceMode::Dynamic, /*canUseDynamic=*/false, /*normalValid=*/false, /*highValid=*/false);
  CHECK(!s.useDynamic && s.loadTier == Tier::Normal && s.rebuildFirst && s.rebuildTier == Tier::Normal);

  // Explicit rebuild request works on both Full Deferred and Thin GBuffer
  s = select(Tier::High, SourceMode::Auto, /*canUseDynamic=*/true, /*normalValid=*/true, /*highValid=*/true, /*forceRebuild=*/true);
  CHECK(!s.useDynamic && s.loadTier == Tier::High && s.rebuildFirst && s.rebuildTier == Tier::High);
}

int main()
{
  test_names_and_tiers();
  test_checksum();
  test_roundtrip_and_corruption();
  test_selection();
  if (g_failed == 0)
    std::printf("daGIBakeCache: all checks passed\n");
  else
    std::printf("daGIBakeCache: %d check(s) failed\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
