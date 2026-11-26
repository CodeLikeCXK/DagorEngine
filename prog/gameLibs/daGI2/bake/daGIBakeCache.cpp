// Copyright (C) Gaijin Games KFT.  All rights reserved.
#include "daGIBakeCache.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace dagi_bake
{
namespace
{
constexpr char MAGIC[8] = {'D', 'G', 'I', 'B', 'A', 'K', 'E', '\0'};
constexpr size_t PREFIX_SIZE = sizeof(MAGIC) + sizeof(uint32_t); // magic + headerSize
constexpr size_t VOLUME_META_SIZE = 4 * sizeof(uint32_t) + 4 * sizeof(uint32_t); // origin[3]+probeSize + gridDim[3]+brickCount

void put_u8(std::vector<uint8_t> &out, uint8_t v) { out.push_back(v); }

void put_u16(std::vector<uint8_t> &out, uint16_t v)
{
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}

void put_u32(std::vector<uint8_t> &out, uint32_t v)
{
  for (int i = 0; i < 4; ++i)
    out.push_back(uint8_t(v >> (8 * i)));
}

void put_u64(std::vector<uint8_t> &out, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    out.push_back(uint8_t(v >> (8 * i)));
}

void put_f32(std::vector<uint8_t> &out, float v)
{
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  put_u32(out, bits);
}

struct Reader
{
  const uint8_t *data;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  bool take(size_t n)
  {
    if (!ok || n > size - pos)
      ok = false;
    return ok;
  }
  uint8_t u8()
  {
    if (!take(1))
      return 0;
    return data[pos++];
  }
  uint16_t u16()
  {
    if (!take(2))
      return 0;
    uint16_t v = uint16_t(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    return v;
  }
  uint32_t u32()
  {
    if (!take(4))
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= uint32_t(data[pos + i]) << (8 * i);
    pos += 4;
    return v;
  }
  uint64_t u64()
  {
    if (!take(8))
      return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= uint64_t(data[pos + i]) << (8 * i);
    pos += 8;
    return v;
  }
  float f32()
  {
    uint32_t bits = u32();
    float v = 0.f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
};

void set_err(std::string *err, const char *msg)
{
  if (err)
    *err = msg;
}

bool read_prefix(std::ifstream &f, uint32_t &header_size, std::string *err)
{
  char prefix[PREFIX_SIZE];
  f.read(prefix, PREFIX_SIZE);
  if (f.gcount() != std::streamsize(PREFIX_SIZE))
  {
    set_err(err, "file too short");
    return false;
  }
  if (std::memcmp(prefix, MAGIC, sizeof(MAGIC)) != 0)
  {
    set_err(err, "bad magic");
    return false;
  }
  Reader r{reinterpret_cast<const uint8_t *>(prefix) + sizeof(MAGIC), sizeof(uint32_t)};
  header_size = r.u32();
  return r.ok;
}
} // namespace

const char *tier_name(Tier tier)
{
  switch (tier)
  {
    case Tier::Normal: return "normal";
    case Tier::High: return "high";
    default: return "none";
  }
}

bool parse_tier(const char *name, Tier &out)
{
  if (!name)
    return false;
  if (std::strcmp(name, "normal") == 0)
    out = Tier::Normal;
  else if (std::strcmp(name, "high") == 0)
    out = Tier::High;
  else
    return false;
  return true;
}

const char *source_mode_name(SourceMode mode)
{
  switch (mode)
  {
    case SourceMode::Auto: return "auto";
    case SourceMode::Baked: return "baked";
    case SourceMode::Dynamic: return "dynamic";
    default: return "auto";
  }
}

bool parse_source_mode(const char *name, SourceMode &out)
{
  if (!name)
    return false;
  if (std::strcmp(name, "auto") == 0)
    out = SourceMode::Auto;
  else if (std::strcmp(name, "baked") == 0)
    out = SourceMode::Baked;
  else if (std::strcmp(name, "dynamic") == 0)
    out = SourceMode::Dynamic;
  else
    return false;
  return true;
}

std::string cache_file_name(const std::string &level_id, Tier tier)
{
  return level_id + "_gi_" + tier_name(tier) + ".dgibake";
}

uint64_t checksum(const void *data, size_t size)
{
  constexpr uint64_t OFFSET_BASIS = 14695981039346656037ull;
  constexpr uint64_t PRIME = 1099511628211ull;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint64_t h = OFFSET_BASIS;
  for (size_t i = 0; i < size; ++i)
  {
    h ^= p[i];
    h *= PRIME;
  }
  return h;
}

bool pack_volume_payload(const VolumeData &vol, std::vector<uint8_t> &payload, std::string *err)
{
  if (vol.probeSize <= 0.f)
  {
    set_err(err, "invalid probe size");
    return false;
  }
  if (vol.gridDim[0] == 0 || vol.gridDim[1] == 0 || vol.gridDim[2] == 0)
  {
    set_err(err, "invalid brick grid dimensions");
    return false;
  }
  const size_t nBricks = vol.bricks.size();
  if (nBricks > INVALID_BRICK_INDEX)
  {
    set_err(err, "too many bricks for uint16 indirection");
    return false;
  }
  if (vol.sph0.size() != nBricks * BRICK_PROBES_COUNT || vol.sph1.size() != nBricks * BRICK_PROBES_COUNT)
  {
    set_err(err, "probe buffer size mismatch");
    return false;
  }
  for (size_t i = 0; i < nBricks; ++i)
  {
    const BrickCoord &b = vol.bricks[i];
    if (b.x >= vol.gridDim[0] || b.y >= vol.gridDim[1] || b.z >= vol.gridDim[2])
    {
      set_err(err, "brick coordinate out of grid bounds");
      return false;
    }
  }

  payload.clear();
  payload.reserve(VOLUME_META_SIZE + nBricks * BRICK_RECORD_BYTES);
  put_f32(payload, vol.origin[0]);
  put_f32(payload, vol.origin[1]);
  put_f32(payload, vol.origin[2]);
  put_f32(payload, vol.probeSize);
  put_u32(payload, vol.gridDim[0]);
  put_u32(payload, vol.gridDim[1]);
  put_u32(payload, vol.gridDim[2]);
  put_u32(payload, uint32_t(nBricks));

  for (size_t i = 0; i < nBricks; ++i)
  {
    put_u16(payload, vol.bricks[i].x);
    put_u16(payload, vol.bricks[i].y);
    put_u16(payload, vol.bricks[i].z);
    put_u16(payload, vol.bricks[i].flags);
  }
  for (uint32_t v : vol.sph0)
    put_u32(payload, v);
  for (uint64_t v : vol.sph1)
    put_u64(payload, v);
  return true;
}

bool unpack_volume_payload(const uint8_t *data, size_t size, uint32_t expected_bricks, VolumeData &vol, std::string *err)
{
  if (!data || size < VOLUME_META_SIZE)
  {
    set_err(err, "volume payload too short");
    return false;
  }
  Reader r{data, size};
  VolumeData out;
  out.origin[0] = r.f32();
  out.origin[1] = r.f32();
  out.origin[2] = r.f32();
  out.probeSize = r.f32();
  out.gridDim[0] = r.u32();
  out.gridDim[1] = r.u32();
  out.gridDim[2] = r.u32();
  const uint32_t nBricks = r.u32();

  if (!r.ok || out.probeSize <= 0.f || !out.gridDim[0] || !out.gridDim[1] || !out.gridDim[2])
  {
    set_err(err, "bad volume metadata");
    return false;
  }
  if (nBricks != expected_bricks || nBricks > INVALID_BRICK_INDEX)
  {
    set_err(err, "brick count mismatch");
    return false;
  }
  const size_t expectedSize = VOLUME_META_SIZE + size_t(nBricks) * BRICK_RECORD_BYTES;
  if (size != expectedSize)
  {
    set_err(err, "unexpected volume payload size");
    return false;
  }

  out.bricks.resize(nBricks);
  for (uint32_t i = 0; i < nBricks; ++i)
  {
    out.bricks[i].x = r.u16();
    out.bricks[i].y = r.u16();
    out.bricks[i].z = r.u16();
    out.bricks[i].flags = r.u16();
    if (out.bricks[i].x >= out.gridDim[0] || out.bricks[i].y >= out.gridDim[1] || out.bricks[i].z >= out.gridDim[2])
    {
      set_err(err, "brick coordinate out of bounds");
      return false;
    }
  }

  const size_t totalProbes = size_t(nBricks) * BRICK_PROBES_COUNT;
  out.sph0.resize(totalProbes);
  out.sph1.resize(totalProbes);
  for (size_t i = 0; i < totalProbes; ++i)
    out.sph0[i] = r.u32();
  for (size_t i = 0; i < totalProbes; ++i)
    out.sph1[i] = r.u64();

  if (!r.ok || r.pos != size)
  {
    set_err(err, "corrupt volume payload");
    return false;
  }
  vol = std::move(out);
  return true;
}

bool write_file(const std::string &path, const Header &header, const std::vector<uint8_t> &payload, std::string *err)
{
  if (header.levelId.size() > MAX_LEVEL_ID_LEN)
  {
    set_err(err, "level id too long");
    return false;
  }

  std::vector<uint8_t> hdr;
  put_u32(hdr, header.formatVersion);
  put_u32(hdr, header.producerVersion);
  put_u8(hdr, uint8_t(header.tier));
  put_u8(hdr, 0);
  put_u8(hdr, 0);
  put_u8(hdr, 0);
  put_u16(hdr, uint16_t(header.levelId.size()));
  hdr.insert(hdr.end(), header.levelId.begin(), header.levelId.end());
  put_u64(hdr, header.contentHash);
  put_u64(hdr, header.lightingSignature);
  put_u32(hdr, header.stateCount);
  put_u32(hdr, header.brickSize);
  put_u32(hdr, header.brickCount);
  put_u64(hdr, uint64_t(payload.size()));
  put_u64(hdr, checksum(payload.data(), payload.size()));

  std::vector<uint8_t> file;
  file.reserve(PREFIX_SIZE + hdr.size() + payload.size());
  file.insert(file.end(), MAGIC, MAGIC + sizeof(MAGIC));
  put_u32(file, uint32_t(hdr.size()));
  file.insert(file.end(), hdr.begin(), hdr.end());
  file.insert(file.end(), payload.begin(), payload.end());

  // write next to the target and move over it, so a crash never leaves a half written cache
  const std::filesystem::path target(path);
  if (target.has_parent_path())
  {
    std::error_code mkdirEc;
    std::filesystem::create_directories(target.parent_path(), mkdirEc);
  }
  const std::filesystem::path tmp = std::filesystem::path(path + ".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
    {
      set_err(err, "cannot open temp file");
      return false;
    }
    f.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    if (!f)
    {
      set_err(err, "write failed");
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  if (ec)
  {
    std::filesystem::remove(target, ec);
    ec.clear();
    std::filesystem::rename(tmp, target, ec);
  }
  if (ec)
  {
    set_err(err, "rename failed");
    return false;
  }
  return true;
}

bool write_volume(const std::string &path, Header header, const VolumeData &vol, std::string *err)
{
  std::vector<uint8_t> payload;
  if (!pack_volume_payload(vol, payload, err))
    return false;
  header.brickSize = BRICK_PROBES_SIDE;
  header.brickCount = uint32_t(vol.bricks.size());
  return write_file(path, header, payload, err);
}

bool read_header(const std::string &path, Header &header, std::string *err)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    set_err(err, "cannot open file");
    return false;
  }
  uint32_t headerSize = 0;
  if (!read_prefix(f, headerSize, err))
    return false;

  std::vector<uint8_t> hdr(headerSize);
  f.read(reinterpret_cast<char *>(hdr.data()), std::streamsize(headerSize));
  if (f.gcount() != std::streamsize(headerSize))
  {
    set_err(err, "header truncated");
    return false;
  }

  Reader r{hdr.data(), hdr.size()};
  Header h;
  h.formatVersion = r.u32();
  h.producerVersion = r.u32();
  h.tier = Tier(r.u8());
  r.u8();
  r.u8();
  r.u8();
  const uint16_t levelLen = r.u16();
  if (levelLen > MAX_LEVEL_ID_LEN || !r.take(levelLen))
  {
    set_err(err, "bad level id");
    return false;
  }
  h.levelId.assign(reinterpret_cast<const char *>(hdr.data() + r.pos), levelLen);
  r.pos += levelLen;
  h.contentHash = r.u64();
  h.lightingSignature = r.u64();
  h.stateCount = r.u32();
  h.brickSize = r.u32();
  h.brickCount = r.u32();
  h.payloadSize = r.u64();
  h.payloadChecksum = r.u64();
  if (!r.ok || r.pos != hdr.size())
  {
    set_err(err, "bad header");
    return false;
  }
  if (h.formatVersion != FORMAT_VERSION)
  {
    set_err(err, "format version mismatch");
    return false;
  }

  f.seekg(0, std::ios::end);
  const uint64_t fileSize = uint64_t(f.tellg());
  if (fileSize != PREFIX_SIZE + uint64_t(headerSize) + h.payloadSize)
  {
    set_err(err, "payload size does not match file size");
    return false;
  }

  header = std::move(h);
  return true;
}

bool verify_payload(const std::string &path, const Header &header, std::string *err)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    set_err(err, "cannot open file");
    return false;
  }
  uint32_t headerSize = 0;
  if (!read_prefix(f, headerSize, err))
    return false;
  f.seekg(std::streamoff(PREFIX_SIZE + headerSize), std::ios::beg);

  std::vector<uint8_t> payload(size_t(header.payloadSize));
  f.read(reinterpret_cast<char *>(payload.data()), std::streamsize(payload.size()));
  if (f.gcount() != std::streamsize(payload.size()))
  {
    set_err(err, "payload truncated");
    return false;
  }
  if (checksum(payload.data(), payload.size()) != header.payloadChecksum)
  {
    set_err(err, "payload checksum mismatch");
    return false;
  }
  return true;
}

bool read_volume(const std::string &path, Header &header, VolumeData &vol, bool check_hash, std::string *err)
{
  if (!read_header(path, header, err))
    return false;
  if (header.brickSize != BRICK_PROBES_SIDE)
  {
    set_err(err, "unsupported brick size");
    return false;
  }

  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    set_err(err, "cannot open file");
    return false;
  }
  uint32_t headerSize = 0;
  if (!read_prefix(f, headerSize, err))
    return false;
  f.seekg(std::streamoff(PREFIX_SIZE + headerSize), std::ios::beg);

  std::vector<uint8_t> payload(size_t(header.payloadSize));
  f.read(reinterpret_cast<char *>(payload.data()), std::streamsize(payload.size()));
  if (f.gcount() != std::streamsize(payload.size()))
  {
    set_err(err, "payload truncated");
    return false;
  }
  if (check_hash && checksum(payload.data(), payload.size()) != header.payloadChecksum)
  {
    set_err(err, "payload checksum mismatch");
    return false;
  }
  return unpack_volume_payload(payload.data(), payload.size(), header.brickCount, vol, err);
}

Selection select(Tier configured, bool normalValid, bool highValid)
{
  return select(configured, SourceMode::Baked, /*canUseDynamic=*/false, normalValid, highValid, /*forceRebuild=*/false);
}

Selection select(Tier configured, SourceMode sourceMode, bool canUseDynamic, bool normalValid, bool highValid, bool forceRebuild)
{
  const Tier targetTier = (configured == Tier::High) ? Tier::High : Tier::Normal;
  Selection s;

  if (forceRebuild)
  {
    s.loadTier = targetTier;
    s.rebuildFirst = true;
    s.rebuildTier = targetTier;
    return s;
  }

  // Thin GBuffer cannot run live full GI: it always stays on the baked path.
  const SourceMode effectiveMode = canUseDynamic ? sourceMode : SourceMode::Baked;

  if (effectiveMode == SourceMode::Dynamic)
  {
    s.useDynamic = true;
    s.loadTier = Tier::None;
    return s;
  }

  if (effectiveMode == SourceMode::Auto)
  {
    // On Full Deferred with Auto mode: use prebuilt cache when present, or fall back to live dynamic GI
    // when the player deletes the active .dgibake file(s).
    if (targetTier == Tier::High)
    {
      if (highValid)
      {
        s.loadTier = Tier::High;
        return s;
      }
      if (normalValid)
      {
        // Option A: high missing while normal is still present -> load normal and prompt high rebuild
        s.loadTier = Tier::Normal;
        s.promptRebuildHigh = true;
        return s;
      }
      s.useDynamic = true;
      s.loadTier = Tier::None;
      return s;
    }

    if (normalValid)
    {
      s.loadTier = Tier::Normal;
      return s;
    }
    s.useDynamic = true;
    s.loadTier = Tier::None;
    return s;
  }

  // SourceMode::Baked (mandatory on Thin GBuffer, or explicitly chosen on Full Deferred)
  if (targetTier == Tier::High)
  {
    if (highValid)
    {
      s.loadTier = Tier::High;
      return s;
    }
    // Option A: fall back to the normal tier and offer the high rebuild in the UI
    s.promptRebuildHigh = true;
    if (normalValid)
    {
      s.loadTier = Tier::Normal;
      return s;
    }
    // Neither tier is on disk: build the normal tier so the level is playable
    s.loadTier = Tier::Normal;
    s.rebuildFirst = true;
    s.rebuildTier = Tier::Normal;
    return s;
  }

  s.loadTier = Tier::Normal;
  if (!normalValid)
  {
    s.rebuildFirst = true;
    s.rebuildTier = Tier::Normal;
  }
  return s;
}
} // namespace dagi_bake
