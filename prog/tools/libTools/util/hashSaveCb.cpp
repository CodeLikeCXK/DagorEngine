// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <libTools/util/hash.h>
#include <ioSys/dag_genIo.h>
#include <hash/crc32.h>
#include <hash/md5.h>
#include <hash/sha1.h>

struct HashComputeStreamBase : public IGenSave
{
  IGenSave *copyCwr;
  HashComputeStreamBase(IGenSave *cwr) : copyCwr(cwr) {}

  virtual int getHash(void *, int, bool) { return 0; }
  void write(const void *p, int sz) override { writeCopy(p, sz); }
  int tell() override
  {
    G_ASSERT(0);
    return 0;
  }
  void seekto(int) override { G_ASSERT(0); }
  void seektoend(int = 0) override { G_ASSERT(0); }
  const char *getTargetName() override { return "hash://"; }
  void flush() override { /*noop*/ }
  void beginBlock() override { G_ASSERT(0); }
  int getBlockLevel() override
  {
    G_ASSERT(0);
    return 0;
  }
  void endBlock(unsigned = 0) override { G_ASSERT(0); }

  void writeCopy(const void *p, int sz)
  {
    if (copyCwr && sz)
      copyCwr->write(p, sz);
  }
};

struct HashComputeStreamCRC32 : public HashComputeStreamBase
{
  static const int SZ = sizeof(unsigned);
  unsigned crc;

  HashComputeStreamCRC32(IGenSave *cwr) : HashComputeStreamBase(cwr) { crc = 0; }
  void write(const void *ptr, int size) override
  {
    writeCopy(ptr, size);
    crc = calc_crc32_continuous((const unsigned char *)ptr, size, crc);
  }
  int getHash(void *buf, int buf_sz, bool reset) override
  {
    if (buf_sz < SZ)
      return -SZ;
    memcpy(buf, &crc, SZ);
    if (reset)
      crc = 0;
    return SZ;
  }
};

struct HashComputeStreamMD5 : public HashComputeStreamBase
{
  md5_state_t st;
  static const int SZ = sizeof(md5_byte_t[16]);

  HashComputeStreamMD5(IGenSave *cwr) : HashComputeStreamBase(cwr) { md5_init(&st); }
  void write(const void *ptr, int size) override
  {
    writeCopy(ptr, size);
    md5_append(&st, (const md5_byte_t *)ptr, size);
  }
  int getHash(void *buf, int buf_sz, bool reset) override
  {
    if (buf_sz < SZ)
      return -SZ;
    md5_finish(&st, (md5_byte_t *)buf);
    if (reset)
      md5_init(&st);
    return SZ;
  }
};

struct HashComputeStreamSHA1 : public HashComputeStreamBase
{
  sha1_context st;
  static const int SZ = sizeof(unsigned char[20]);

  HashComputeStreamSHA1(IGenSave *cwr) : HashComputeStreamBase(cwr) { sha1_starts(&st); }
  void write(const void *ptr, int size) override
  {
    writeCopy(ptr, size);
    sha1_update(&st, (const unsigned char *)ptr, size);
  }
  int getHash(void *buf, int buf_sz, bool reset) override
  {
    if (buf_sz < SZ)
      return -SZ;
    sha1_finish(&st, (unsigned char *)buf);
    if (reset)
      sha1_starts(&st);
    return SZ;
  }
};

IGenSave *create_hash_computer_cb(int hash_savecb_type, IGenSave *copy_cwr)
{
  if (hash_savecb_type == HASH_SAVECB_CRC32)
    return new HashComputeStreamCRC32(copy_cwr);
  else if (hash_savecb_type == HASH_SAVECB_MD5)
    return new HashComputeStreamMD5(copy_cwr);
  else if (hash_savecb_type == HASH_SAVECB_SHA1)
    return new HashComputeStreamSHA1(copy_cwr);
  return new HashComputeStreamBase(copy_cwr); // stub; we always return object
}
int get_computed_hash(IGenSave *hash_cb, void *out_data, int out_data_sz, bool reset)
{
  return static_cast<HashComputeStreamBase *>(hash_cb)->getHash(out_data, out_data_sz, reset);
}
void destory_hash_computer_cb(IGenSave *hash_cb) { delete hash_cb; }
