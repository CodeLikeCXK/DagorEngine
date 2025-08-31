// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <3d/tql.h>
#include <resourceName.h>
#include <dag/dag_vector.h>
#include <drv/3d/dag_query.h>

/* This validation is disabled by default because at the end of frame
it iterates over all created buffers and we don't wan't to waste time on it.
The validation is useful while we are modifying buffers to ensure that we update
dynamic buffers every frame. When all the buffers will be fixed, we will make
framemem flag mandatory for dynamic buffers and the validation could be removed.
*/
// #define ADDITIONAL_DYNAMIC_BUFFERS_VALIDATION

// check that updates of buffers do not generate GPU waits
// #define VALIDATE_FOR_BLOCKING_UPDATES 1

namespace drv3d_dx11
{
static constexpr int VBLOCK_DELAYED = 0x10000;

class GenericBuffer final : public D3dResourceNameImpl<Sbuffer>
{
public:
  ID3D11Buffer *buffer = nullptr;
  ID3D11Buffer *stagingBuffer = nullptr; // non-null if allocated
  ID3D11UnorderedAccessView *uav = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  char *sysMemBuf = nullptr;
  Sbuffer::IReloadData *rld = nullptr;
  char *systemCopy = nullptr;
  int handle = -1;
#ifdef ADDITIONAL_DYNAMIC_BUFFERS_VALIDATION
  bool updated = false;
#endif
#ifdef VALIDATE_FOR_BLOCKING_UPDATES
  struct UpdateHistoryElement
  {
    bool completed;
    bool write;
    d3d::EventQuery *completion;
    size_t offset;
    size_t size;
  };
  dag::Vector<UpdateHistoryElement> updateHistory;
  int activeLockFlags;
  void checkBlockingUpdate(size_t offset, size_t size, bool write);
  void trackUpdate(size_t offset, size_t size, bool write);
#else
  void checkBlockingUpdate(size_t, size_t, bool) {}
  void trackUpdate(size_t, size_t, bool) {}
#endif

  enum BufferState
  {
    BUFFER_INVALID,
    BUFFER_LOCKED,
    BUFFER_COPIED,
    UPDATED_BY_COPYTO,
    STAGING_BUFFER_LOCKED,
    STAGING_BUFFER_LOCKED_SHOULD_BE_COPIED
  };

  D3D11_MAPPED_SUBRESOURCE lockMsr = {0};
  UINT bindFlags = 0;
  UINT miscFlags = 0;
  uint32_t bufSize = 0;
  uint32_t bufFlags = 0;
  uint32_t format = 0;
  uint16_t structSize = 0;
  BufferState internalState = BUFFER_INVALID;

  GenericBuffer() = default;
  ~GenericBuffer();

  uint32_t getSize() const override { return (int)bufSize; }
  int getFlags() const override { return (int)bufFlags; }

  int getElementSize() const override { return (int)structSize; }
  int getNumElements() const override { return (int)(structSize > 0 ? bufSize / structSize : 0); }

  void setBufName(const char *name);
  ID3D11ShaderResourceView *getResView();

  bool createBuf();
  bool createSrv();
  bool createUav();
  bool create(uint32_t bufsize, int buf_flags, UINT bind_flags, const char *statName);
  bool createStructured(uint32_t struct_size, uint32_t elements, uint32_t buf_flags, uint32_t buf_format, const char *statName);

  bool recreateBuf(Sbuffer *sb);

  bool copyTo(Sbuffer *dest) override;
  bool copyTo(Sbuffer *dest, uint32_t dst_ofs_bytes, uint32_t src_ofs_bytes, uint32_t size_bytes) override;

  int lock(unsigned ofs_bytes, unsigned size_bytes, void **ptr, int flags) override;
  void *lock(uint32_t ofs_bytes, uint32_t size_bytes, int flags);
  int unlock() override;
  bool updateData(uint32_t ofs_bytes, uint32_t size_bytes, const void *__restrict src, uint32_t lockFlags) override;

  void setRld(Sbuffer::IReloadData *rld);

  bool setReloadCallback(Sbuffer::IReloadData *_rld) override
  {
    setRld(_rld);
    return true;
  }

  void removeFromStates();

  void release();

  void destroyObject();
  void destroy();

private:
  bool createStagingBuffer(ID3D11Buffer *&out_buf);
};
} // namespace drv3d_dx11
