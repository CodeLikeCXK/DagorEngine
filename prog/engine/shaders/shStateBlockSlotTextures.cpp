// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <drv/3d/dag_shaderConstants.h>
#include <drv/3d/dag_texture.h>
#include <drv/3d/dag_buffers.h>

#include "shStateBlock.h"
#include "concurrentElementPool.h"
#include "concurrentRangePool.h"

using namespace shaders;

struct SBSamplerState
{
  using TexDataCont = ConcurrentRangePool<TEXTUREID, 12>;
  static TexDataCont data;
  static ConcurrentElementPool<TexStateIdx, SBSamplerState, 10> all; // first one is default
  static OSSpinlock mutex;

  static_assert(DEFAULT_TEX_STATE_ID == decltype(all)::FIRST_ID);

  TexDataCont::Range textures; // we start with PS textures, then VS textures
  uint8_t psCount;             // don't think we will need more than 4 bits
  uint8_t vsCount;             // Redundant, but it doesn't hurt storing it
  uint8_t psBase;              // don't think we will need more than 4 bits
  uint8_t vsBase;

  void apply(int tex_level)
  {
    const auto dataView = data.view(textures);

    const auto setTex = [&](TEXTUREID tid, ShaderStage stage, int slot) {
      mark_managed_tex_lfu(tid, tex_level);
      const auto sampler = get_texture_separate_sampler(tid);
      if (sampler != d3d::INVALID_SAMPLER_HANDLE)
      {
        d3d::set_tex(stage, slot, D3dResManagerData::getBaseTex(tid), false);
        d3d::set_sampler(stage, slot, sampler);
      }
      else
        d3d::set_tex(stage, slot, D3dResManagerData::getBaseTex(tid));
    };

    for (uint32_t i = 0, e = psCount; i < e; ++i)
      setTex(dataView[i], STAGE_PS, psBase + i);
    for (uint32_t i = 0, e = vsCount; i < e; ++i)
      setTex(dataView[psCount + i], STAGE_VS, vsBase + i);
  }

  void reqTexLevel(int tex_level)
  {
    const auto dataView = data.view(textures);
    for (uint32_t i = 0, e = psCount; i < e; ++i)
      mark_managed_tex_lfu(dataView[i], tex_level);
    for (uint32_t i = vsBase, e = vsCount; i < e; ++i)
      mark_managed_tex_lfu(dataView[psCount + i], tex_level);
  }

  static TexStateIdx create(const TEXTUREID *ps, uint8_t ps_base, uint8_t ps_cnt, const TEXTUREID *vs, uint8_t vs_base, uint8_t vs_cnt)
  {
    OSSpinlockScopedLock lock(mutex);

    {
      TexStateIdx id = all.findIf([&](const SBSamplerState &c) {
        if (c.psCount != ps_cnt || c.vsCount != vs_cnt || c.psBase != ps_base || c.vsBase != vs_base)
          return false;
        const auto dataView = data.view(c.textures);
        if (memcmp(dataView.data(), ps, ps_cnt * sizeof(*ps)) == 0 && memcmp(dataView.data() + ps_cnt, vs, vs_cnt * sizeof(*vs)) == 0)
          return true;
        return false;
      });
      if (id != TexStateIdx::Invalid)
        return id;
    }

    const auto dataId = data.allocate(ps_cnt + vs_cnt);
    const auto dataView = data.view(dataId);
    memcpy(dataView.data(), ps, ps_cnt * sizeof(*ps));
    memcpy(dataView.data() + ps_cnt, vs, vs_cnt * sizeof(*vs));

    const TexStateIdx result = all.allocate();
    all[result] = SBSamplerState{dataId, ps_cnt, vs_cnt, ps_base, vs_base};
    return result;
  }
  static void clear()
  {
    data.clear();
    all.clear();
    all[all.allocate()] = SBSamplerState{TexDataCont::Range::Empty, 0, 0, 0, 0};
  }
};

decltype(SBSamplerState::data) SBSamplerState::data;
decltype(SBSamplerState::all) SBSamplerState::all; // first one is default
decltype(SBSamplerState::mutex) SBSamplerState::mutex;


struct ConstState
{
  using ConstsDataCont = ConcurrentRangePool<Point4, 13>;
  static ConstsDataCont dataConsts;
  static ConcurrentElementPool<ConstStateIdx, ConstState, 11> all;        // first one is default
  static ConcurrentElementPool<ConstStateIdx, Sbuffer *, 11> allConstBuf; // parallel to all
  static OSSpinlock mutex;

  static_assert(DEFAULT_CONST_STATE_ID == decltype(all)::FIRST_ID);
  static_assert(DEFAULT_CONST_STATE_ID == decltype(allConstBuf)::FIRST_ID);

  ConstsDataCont::Range consts;

  void apply(ConstStateIdx self_idx)
  {
    Sbuffer *constBuf = allConstBuf[self_idx];
    if (!constBuf)
      return;

    d3d::set_const_buffer(STAGE_VS, 1, constBuf);
    d3d::set_const_buffer(STAGE_PS, 1, constBuf);
  }

  static ConstStateIdx create(const Point4 *consts_data, uint8_t consts_count)
  {
    OSSpinlockScopedLock lock(mutex);
    {
      ConstStateIdx cid = all.findIf([&](const ConstState &c) {
        const auto constsView = dataConsts.view(c.consts);
        if (constsView.size() != consts_count)
          return false;

        return memcmp(constsView.data(), consts_data, consts_count * sizeof(*consts_data)) == 0; // -V1014
      });
      if (cid != ConstStateIdx::Invalid)
        return cid;
    }

    const auto consts = dataConsts.allocate(consts_count);
    const auto constsView = dataConsts.view(consts);
    memcpy(constsView.data(), consts_data, consts_count * sizeof(*consts_data));

    const ConstStateIdx id = all.allocate();
    all[id] = ConstState{consts};
    {
      ConstStateIdx bufId = allConstBuf.allocate();
      G_UNUSED(bufId);
      G_FAST_ASSERT(id == bufId);
    }
    return id;
  }
  static void clear()
  {
    dataConsts.clear();
    all.clear();
    all[all.allocate()] = ConstState{ConstsDataCont::Range::Empty};
    for (auto buf : allConstBuf)
      destroy_d3dres(buf);
    allConstBuf.clear();
    allConstBuf.allocate();
  }

  static void prepareConstBuf(ConstStateIdx const_state_idx)
  {
    const ConstState &state = all[const_state_idx];
    if (!interlocked_acquire_load_ptr(allConstBuf[const_state_idx]) && ConstsDataCont::range_size(state.consts) > 0)
    {
      String s(0, "staticCbuf%d", eastl::to_underlying(const_state_idx));
      const auto constsView = dataConsts.view(state.consts);
      Sbuffer *constBuf = d3d::buffers::create_persistent_cb(constsView.size(), s.c_str());
      if (!constBuf)
      {
        logerr("Could not create Sbuffer for const buf.");
        return;
      }
      constBuf->updateDataWithLock(0, constsView.size_bytes(), constsView.data(), VBLOCK_DISCARD);
      if (DAGOR_UNLIKELY(interlocked_compare_exchange_ptr(allConstBuf[const_state_idx], constBuf, (Sbuffer *)nullptr) != nullptr))
        constBuf->destroy(); // unlikely case when other thread created/written buffer for this slot first
    }
  }
};

decltype(ConstState::dataConsts) ConstState::dataConsts;
decltype(ConstState::all) ConstState::all;                 // first one is default
decltype(ConstState::allConstBuf) ConstState::allConstBuf; // parallel to all
decltype(ConstState::mutex) ConstState::mutex;

ShaderStateBlock create_slot_textures_state(const TEXTUREID *ps, uint8_t ps_base, uint8_t ps_cnt, const TEXTUREID *vs, uint8_t vs_base,
  uint8_t vs_cnt, const Point4 *consts_data, uint8_t consts_count, bool static_block)
{
  ShaderStateBlock block;
  block.samplerIdx = SBSamplerState::create(ps, ps_base, ps_cnt, vs, vs_base, vs_cnt);
  block.constIdx = ConstState::create(consts_data, consts_count);
  if (static_block)
    ConstState::prepareConstBuf(block.constIdx);
  return block;
}

void apply_slot_textures_state(ConstStateIdx const_state_idx, TexStateIdx sampler_state_id, int tex_level)
{
  SBSamplerState::all[sampler_state_id].apply(tex_level);
  ConstState::all[const_state_idx].apply(const_state_idx);
}

void clear_slot_textures_states()
{
  SBSamplerState::clear();
  ConstState::clear();
}

void slot_textures_req_tex_level(TexStateIdx sampler_state_id, int tex_level)
{
  SBSamplerState::all[sampler_state_id].reqTexLevel(tex_level);
}

void dump_slot_textures_states_stat()
{
  debug("%d const states (%d total), %d texture states (%d total)", ConstState::all.totalElements(),
    ConstState::dataConsts.totalElements(), SBSamplerState::all.totalElements(), SBSamplerState::data.totalElements());
}
