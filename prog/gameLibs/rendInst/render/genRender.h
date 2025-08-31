// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <rendInst/rendInstGenRender.h>
#include <rendInst/riShaderConstBuffers.h>
#include "riGen/riGenData.h"
#include "riGen/riGenExtra.h"
#include "render/extraRender.h"

#include <generic/dag_smallTab.h>
#include <drv/3d/dag_vertexIndexBuffer.h>
#include <drv/3d/dag_tex3d.h>
#include <math/dag_Point3.h>
#include <shaders/dag_rendInstRes.h>
#include <shaders/dag_overrideStates.h>
#include <shaders/dag_shaderBlock.h>
#include <generic/dag_tab.h>
#include <generic/dag_carray.h>
#include <generic/dag_staticTab.h>
#include <3d/dag_resPtr.h>
#include <3d/dag_multidrawContext.h>


//-V:SWITCH_STATES:501
struct RenderStateContext
{
  ShaderElement *curShader = nullptr;
  GlobalVertexData *curVertexData = nullptr;
  Vbuffer *curVertexBuf = nullptr;
  int curStride = 0;

  bool curShaderValid = false;
  RenderStateContext();
};

//-V:SWITCH_STATES_SHADER:501
#define SWITCH_STATES_SHADER()                                        \
  {                                                                   \
    if (elem.e != context.curShader)                                  \
    {                                                                 \
      context.curShader = elem.e;                                     \
      context.curShader->setReqTexLevel(15);                          \
      context.curShaderValid = context.curShader->setStates(0, true); \
    }                                                                 \
    if (!context.curShaderValid)                                      \
      continue;                                                       \
  }

#define SWITCH_STATES_VDATA()                                                                                                        \
  {                                                                                                                                  \
    if (elem.vertexData != context.curVertexData)                                                                                    \
    {                                                                                                                                \
      context.curVertexData = elem.vertexData;                                                                                       \
      if (context.curVertexBuf != elem.vertexData->getVB() || context.curStride != elem.vertexData->getStride())                     \
        d3d_err(d3d::setvsrc(0, context.curVertexBuf = elem.vertexData->getVB(), context.curStride = elem.vertexData->getStride())); \
    }                                                                                                                                \
  }

#define SWITCH_STATES() {SWITCH_STATES_SHADER() SWITCH_STATES_VDATA()}

inline constexpr bool RENDINST_FLOAT_POS = true; // for debug switching between floats and halfs

inline constexpr int RENDER_ELEM_SIZE_PACKED = 8;
inline constexpr int RENDER_ELEM_SIZE_UNPACKED = 16;
inline constexpr int RENDER_ELEM_SIZE = RENDINST_FLOAT_POS ? RENDER_ELEM_SIZE_UNPACKED : RENDER_ELEM_SIZE_PACKED;

inline constexpr int DYNAMIC_IMPOSTOR_TEX_SHADOW_OFFSET = 2;

extern bool check_occluders;

// TODO: this is ugly :/
extern int globalRendinstRenderTypeVarId;
extern int globalTranspVarId;
extern int useRiGpuInstancingVarId;

// TODO: these should somehow be moved to riShaderConstBuffer.cpp
extern int perinstBuffNo;
extern int instanceBuffNo;

enum : int
{
  RENDINST_RENDER_TYPE_MIXED = 0,
  RENDINST_RENDER_TYPE_RIEX_ONLY = 1
};


inline int get_last_mip_idx(Texture *tex, int dest_size)
{
  TextureInfo ti;
  tex->getinfo(ti);
  int src = min(ti.w, ti.h);
  return min(tex->level_count() - 1, int(src > dest_size ? get_log2i(src / dest_size) : 0));
}

template struct eastl::array<uint32_t, 2>;

namespace rendinst::render
{
// on disc we store in short4N
#define ENCODED_RENDINST_RESCALE (32767. / 256)

struct RiGenPerInstanceParameters
{
  uint32_t perDrawData;
  uint32_t instanceOffset;
};

extern shaders::UniqueOverrideStateId afterDepthPrepassOverride;
extern shaders::UniqueOverrideStateId afterDepthPrepassWithStencilTestOverride;
extern Tab<UniqueBuf> riGenPerDrawDataForLayer;
extern MultidrawContext<rendinst::render::RiGenPerInstanceParameters> riGenMultidrawContext;

extern bool use_ri_depth_prepass;
extern int normal_type;
extern void init_depth_VDECL();
extern void close_depth_VDECL();
extern Vbuffer *oneInstanceTmVb;
extern Vbuffer *rotationPaletteTmVb;
extern void cell_set_encoded_bbox(RiShaderConstBuffers &cb, vec4f origin, float grid2worldcellSz, float ht);
extern ShaderBlockIdHolder globalFrameBlockId;
extern ShaderBlockIdHolder rendinstSceneBlockId;
extern ShaderBlockIdHolder rendinstSceneTransBlockId;
extern ShaderBlockIdHolder rendinstDepthSceneBlockId;
extern int rendinstRenderPassVarId;
extern int rendinstShadowTexVarId;
extern bool per_instance_visibility;
extern bool per_instance_visibility_for_everyone;
extern bool per_instance_front_to_back;
extern bool use_tree_lod0_offset;
extern bool use_lods_by_distance_update;
extern float lods_by_distance_range_sq;

extern int dynamicImpostorTypeVarId, dynamicImpostorBackViewDepVarId, dynamicImpostorBackShadowVarId;
extern int dynamicImpostorViewXVarId, dynamicImpostorViewYVarId;

extern int dynamic_impostor_texture_const_no;
extern float rendinst_ao_mul;

extern float globalDistMul;
extern float globalCullDistMul;
extern float globalLodCullDistMul;

extern float settingsDistMul;
extern float settingsMinCullDistMul;
extern float settingsMinLodBasedCullDistMul;
extern float lodsShiftDistMul;
extern bool forceImpostors;
extern bool use_color_padding;
extern bool vertical_billboards;

enum CoordType
{
  COORD_TYPE_TM = 0,
  COORD_TYPE_POS = 1,
  COORD_TYPE_POS_CB = 2
};
inline void setTextureInstancingUsage(bool) {} // deprecated
extern void setCoordType(CoordType type);
extern void setApexInstancing();
eastl::array<uint32_t, 2> getCommonImmediateConstants();

extern void initClipmapShadows();
extern void closeClipmapShadows();
extern void endRenderInstancing();
extern void startRenderInstancing();
extern void ensurePerDrawBufferExists(int layer);
bool setBlock(int block_id, const UniqueBuf &buf);
bool setPerDrawData(const UniqueBuf &buf);

enum
{
  IMP_COLOR = 0,
  IMP_NORMAL = 1,
  IMP_TRANSLUCENCY = 2,
  IMP_COUNT = 3
};

struct DynamicImpostor
{
  StaticTab<UniqueTex, IMP_COUNT> tex;
  int baseMip;
  float currentHalfWidth, currentHalfHeight;
  float impostorSphCenterY, shadowSphCenterY;
  SizePerRotationArr shadowImpostorSizes;
  int renderMips;
  int numColorTexMips;
  float maxFacingLeavesDelta;
  SmallTab<vec4f, MidmemAlloc> points;
  SmallTab<vec4f, MidmemAlloc> shadowPoints; // for computing shadows only, then deleted. array of point, delta
  DynamicImpostor() :
    // translucencyTexId(BAD_TEXTUREID), translucency(nullptr),
    currentHalfWidth(1.f),
    currentHalfHeight(1.f),
    maxFacingLeavesDelta(0.f),
    impostorSphCenterY(0),
    shadowSphCenterY(0),
    renderMips(1),
    numColorTexMips(1),
    baseMip(0),
    shadowImpostorSizes{}
  {}
  void delImpostor()
  {
    for (int i = 0; i < tex.size(); ++i)
      tex[i].close();
    clear_and_shrink(points);
    clear_and_shrink(shadowPoints);
  }
  ~DynamicImpostor() { delImpostor(); }
};

inline void set_no_impostor_tex() { d3d::settex(dynamic_impostor_texture_const_no, nullptr); }


class RtPoolData
{
public:
  float sphereRadius, sphCenterY, cylinderRadius;
  float clipShadowWk, clipShadowHk, clipShadowOrigX, clipShadowOrigY;
  UniqueTex rendinstGlobalShadowTex;
  d3d::SamplerHandle globalShadowSampler = d3d::INVALID_SAMPLER_HANDLE;
  Texture *rendinstClipmapShadowTex;
  TEXTUREID rendinstClipmapShadowTexId;
  DynamicImpostor impostor;
  Texture *shadowImpostorTexture;
  TEXTUREID shadowImpostorTextureId;
  Color4 shadowCol0, shadowCol1, shadowCol2;
  carray<float, MAX_LOD_COUNT_WITH_ALPHA - 1> lodRange; // no range for alpha lod
  bool hadVisibleImpostor;
  uint64_t impostorDataOffsetCache;
  float plodRadius;
  enum
  {
    HAS_NORMALMAP = 0x01,
    HAS_TRANSLUCENCY = 0x02,
    HAS_CLEARED_LIGHT_TEX = 0x04,
    HAS_TRANSITION_LOD = 0x08
  };

  uint32_t flags;
  bool hasUpdatedShadowImpostor;
  bool hasNormalMap() const { return flags & HAS_NORMALMAP; }
  bool hasTranslucency() const { return flags & HAS_TRANSLUCENCY; }
  bool hasTransitionLod() const { return flags & HAS_TRANSITION_LOD; }
  bool hasPLOD() const { return plodRadius > 0.0f; }

  RtPoolData(RenderableInstanceLodsResource *res) :
    shadowImpostorTexture(nullptr), shadowImpostorTextureId(BAD_TEXTUREID), impostorDataOffsetCache(0)
  {
    const Point3 &sphereCenter = res->bsphCenter;
    sphereRadius = res->bsphRad + sqrtf(sphereCenter.x * sphereCenter.x + sphereCenter.z * sphereCenter.z);
    cylinderRadius = sphereRadius;
    sphCenterY = sphereCenter.y;
    rendinstClipmapShadowTexId = BAD_TEXTUREID;
    rendinstClipmapShadowTex = nullptr;
    flags = 0;
    clipShadowWk = clipShadowHk = sphereRadius;
    clipShadowOrigX = clipShadowOrigY = 0;
    shadowCol0 = shadowCol1 = shadowCol2 = Color4(0, 0, 0, 0);
    hasUpdatedShadowImpostor = false;
    hadVisibleImpostor = true;
    plodRadius = 0.0f;
  }
  ~RtPoolData()
  {
    ShaderGlobal::reset_from_vars_and_release_managed_tex_verified(rendinstClipmapShadowTexId, rendinstClipmapShadowTex);
    ShaderGlobal::reset_from_vars_and_release_managed_tex_verified(shadowImpostorTextureId, shadowImpostorTexture);
  }
  bool hasImpostor() const { return impostor.tex.size() != 0; }

  inline void setImpostorParams(RiShaderConstBuffers &cb, float p0, float p1) const
  {
    cb.setBoundingSphere(p0, p1, sphereRadius, cylinderRadius, impostor.impostorSphCenterY);
  }

  inline void setDynamicImpostorBoundingSphere(RiShaderConstBuffers &cb) const
  {
    setImpostorParams(cb, impostor.currentHalfWidth, impostor.currentHalfHeight);
  }

  inline void setNoImpostor(RiShaderConstBuffers &cb) const { setImpostorParams(cb, 0, 0); }

  inline void setShadowImpostorBoundingSphere(RiShaderConstBuffers &cb) const
  {
    setImpostorParams(cb, impostor.shadowImpostorSizes[0].x, impostor.shadowImpostorSizes[0].y);
    cb.setShadowImpostorSizes(impostor.shadowImpostorSizes);
  }

  void setImpostor(RiShaderConstBuffers &cb, bool forShadow) const
  {
    if (!hasImpostor())
    {
      setNoImpostor(cb);
      return;
    }
    if (forShadow)
    {
      d3d::settex(dynamic_impostor_texture_const_no + DYNAMIC_IMPOSTOR_TEX_SHADOW_OFFSET, rendinstGlobalShadowTex.getArrayTex());
      d3d::set_sampler(STAGE_PS, dynamic_impostor_texture_const_no + DYNAMIC_IMPOSTOR_TEX_SHADOW_OFFSET, globalShadowSampler);
      setShadowImpostorBoundingSphere(cb);
    }
    ShaderElement::invalidate_cached_state_block();
  }
};

}; // namespace rendinst::render
