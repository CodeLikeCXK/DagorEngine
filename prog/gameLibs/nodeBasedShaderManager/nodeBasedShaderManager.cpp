// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <nodeBasedShaderManager/nodeBasedShaderManager.h>
#include <drv/3d/dag_shaderConstants.h>
#include <drv/3d/dag_buffers.h>
#include <drv/3d/dag_texture.h>
#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_info.h>
#include <ioSys/dag_dataBlock.h>
#include <ioSys/dag_memIo.h>
#include "platformUtils.h"
#include <EASTL/vector_set.h>
#include <memory/dag_framemem.h>
#if !NBSM_COMPILE_ONLY
#include <render/blkToConstBuffer.h>
#include <shaders/dag_shaders.h>
#include <gameRes/dag_gameResources.h>
#include <gameRes/dag_stdGameResId.h>

static ShaderVariableInfo global_time_phaseVarId("global_time_phase", true);
#endif

static eastl::vector_set<NodeBasedShaderManager *> node_based_shaders_with_cached_resources;

NodeBasedShaderManager::~NodeBasedShaderManager()
{
  resetCachedResources();
  node_based_shaders_with_cached_resources.erase(this);
}

void NodeBasedShaderManager::updateBlkDataConstBuffer(const DataBlock &shader_blk)
{
  currentIntParametersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_int")));
  currentInt4ParametersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_int4")));
  currentFloatParametersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_float")));
  currentFloat4ParametersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_float4")));
  currentFloat4x4ParametersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_float4x4")));
}

void NodeBasedShaderManager::updateBlkDataTextures(const DataBlock &shader_blk)
{
  currentTextures2dBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture2D")));
  currentTextures3dBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture3D")));
  currentTextures2dArrayBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture2DArray")));
  currentTextures2dShdArrayBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture2D_shdArray")));
  currentTextures2dNoSamplerBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture2D_nosampler")));
  currentTextures3dNoSamplerBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_texture3D_nosampler")));
}

void NodeBasedShaderManager::updateBlkDataBuffers(const DataBlock &shader_blk)
{
  currentBuffersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_Buffer")));
  currentCBuffersBlk.reset(new DataBlock(*shader_blk.getBlockByNameEx("inputs_CBuffer")));
}

bool NodeBasedShaderManager::update(const DataBlock &shader_blk, String &out_errors, PLATFORM platform_id)
{
  if (compileShaderProgram(shader_blk, out_errors, platform_id))
  {
    updateBlkDataResources(shader_blk);
  }
  return lastCompileIsSuccess;
}

void NodeBasedShaderManager::getPlatformList(Tab<String> &out_platforms, PLATFORM platform_id)
{
  for (int i = 0; i < PLATFORM::COUNT; i++)
  {
    if (platform_id >= 0 && platform_id != i)
      continue;

    out_platforms.push_back(String(PLATFORM_LABELS[i]));
  }
}

void NodeBasedShaderManager::updateBlkDataResources(const DataBlock &shader_blk)
{
  updateBlkDataConstBuffer(shader_blk);
  updateBlkDataTextures(shader_blk);
  updateBlkDataBuffers(shader_blk);
  cachedVariableGeneration = ~0u;
  isDirty = true;
}

#if !NBSM_COMPILE_ONLY
static eastl::string normalize_shader_res_name(const char *shader_name)
{
  const char *lastSlashPtr = strrchr(shader_name, '/');
  const char *nameBegin = lastSlashPtr ? lastSlashPtr + 1 : shader_name;
  const char *jsonPos = strstr(nameBegin, ".json");
  return eastl::string(nameBegin, jsonPos ? jsonPos : (nameBegin + strlen(nameBegin)));
}

/*
Binary format for loc shader:
Header datablock
{
  N String parameters for optional graph names.
}
2^N times:
  Pemutation parameters datablock
  binary data for permutation
*/

void NodeBasedShaderManager::loadShaderFromStream(uint32_t idx, uint32_t platform_id, InPlaceMemLoadCB &load_stream,
  uint32_t variant_id)
{
  G_ASSERTF(platform_id < PLATFORM::COUNT && int(platform_id) >= 0, "Unknown platform id: %d", platform_id);
  G_ASSERTF(variant_id < get_shader_variant_count(shader) && int(variant_id) >= 0, "Unknown variant id for shader #%d: %d",
    (int)shader, variant_id);
  resetCachedResources();
  ShaderBin &permutation = shaderBin[platform_id][variant_id][idx];
  permParameters[idx].loadFromStream(load_stream);
  uint32_t shaderBinSize;
  load_stream.read((&shaderBinSize), sizeof(shaderBinSize));
  permutation.resize((shaderBinSize + sizeof(uint32_t) - 1) / sizeof(uint32_t));
  load_stream.read(permutation.data(), shaderBinSize);
}

bool NodeBasedShaderManager::loadFromResources(const String &shader_path, bool keep_permutation)
{
  PLATFORM platform_id = get_nbsm_platform();
  G_ASSERTF(platform_id < PLATFORM::COUNT && int(platform_id) >= 0, "Unknown platform id: %d", platform_id);
  eastl::string resName = node_based_shader_get_resource_name(shader_path);
  auto data = (dag::Span<uint8_t> *)get_one_game_resource_ex(GAMERES_HANDLE_FROM_STRING(resName.c_str()), LShaderGameResClassId);
  if (!data)
  {
    logerr("shader does not exist '%s'", resName.c_str());
    return false;
  }
  InPlaceMemLoadCB loadStream(data->data(), data_size(*data));
  DataBlock optionalGraphNamesBlock;
  optionalGraphNamesBlock.loadFromStream(loadStream);
  optionalGraphNames.clear();
  for (uint32_t i = 0; i < optionalGraphNamesBlock.paramCount(); ++i)
    optionalGraphNames.emplace_back(optionalGraphNamesBlock.getStr(i));

  G_ASSERTF(optionalGraphNames.size() < 16, "Optional graphs count should be less than 16");
  optionalGraphNames.resize(min<unsigned>(optionalGraphNames.size(), 15u));

  // Workaround for old for format
  if (strcmp(optionalGraphNamesBlock.getStr("subgraph", ""), "") == 0)
  {
    loadStream.close();
    loadStream = InPlaceMemLoadCB(data->data(), data_size(*data));
  }

  const uint32_t permCount = (1 << optionalGraphNames.size());
  G_ASSERTF(!keep_permutation || permParameters.size() == permCount, "We can't keep permutation if permutations count changed.");
  permParameters.resize(permCount);
  if (!keep_permutation)
    permutationId = 0; // graph names have changed, go back to default permutation

  int variantsCnt = get_shader_variant_count(shader);
  for (ShaderBinVariants &shVariant : shaderBin)
  {
    shVariant.resize(variantsCnt);
    for (int variantId = 0; variantId < variantsCnt; ++variantId)
      shVariant[variantId].resize(permCount);
  }
  for (uint32_t i = 0; i < permCount; ++i)
    for (uint32_t variantId = 0; variantId < variantsCnt; ++variantId)
      loadShaderFromStream(i, platform_id, loadStream, variantId);

  updateBlkDataResources(permParameters[permutationId]);

  lastCompileIsSuccess = true;
  release_game_resource((GameResource *)data);
  return true;
}

const NodeBasedShaderManager::ShaderBinPermutations &NodeBasedShaderManager::getPermutationShadersBin(uint32_t variant_id) const
{
  G_ASSERTF(variant_id < get_shader_variant_count(shader) && int(variant_id) >= 0, "Unknown variant id for shader #%d: %d",
    (int)shader, variant_id);
  PLATFORM platform_id = get_nbsm_platform();
  G_ASSERTF(platform_id < PLATFORM::COUNT && int(platform_id) >= 0, "Unknown platform id: %d", platform_id);
  return shaderBin[platform_id][variant_id];
}

const NodeBasedShaderManager::ArrayValue *NodeBasedShaderManager::getArrayValue(const char *name, size_t name_length) const
{
  for (const ArrayValue &arrayValue : arrayValues)
    if (strncmp(arrayValue.first.str(), name, name_length) == 0)
      return &arrayValue;

  return nullptr;
}

void NodeBasedShaderManager::setArrayValue(const char *name, const Tab<Point4> &values)
{
  for (ArrayValue &arrayValue : arrayValues)
    if (strcmp(arrayValue.first.str(), name) == 0)
    {
      arrayValue.second = &values;
      return;
    }

  arrayValues.emplace_back(String(name), &values);
}

void NodeBasedShaderManager::setBaseConstantBuffer()
{
  ShaderGlobal::set_real(global_time_phaseVarId, get_shader_global_time_phase(0, 0));

  dag::Vector<float, framemem_allocator> parametersBuffer;
  const uint32_t REGISTERS_ALIGNMENT = 4;
  parametersBuffer.reserve(
    (currentIntVariables.size() + currentFloatVariables.size() + REGISTERS_ALIGNMENT - 1) / REGISTERS_ALIGNMENT * REGISTERS_ALIGNMENT +
    currentInt4Variables.size() * REGISTERS_ALIGNMENT + currentFloat4Variables.size() * REGISTERS_ALIGNMENT +
    currentFloat4x4Variables.size() * 4 * REGISTERS_ALIGNMENT);
  for (auto &shaderVar : currentIntVariables)
  {
    const int varType = shaderVar.varType;
    const int varId = shaderVar.varId;
    float valueToAdd;
    if (varType == SHVT_INT) // or may be should be error?
      valueToAdd = bitwise_cast<float, int>(ShaderGlobal::get_int_fast(varId));
    else
      valueToAdd = bitwise_cast<float, int>(shaderVar.val);
    parametersBuffer.push_back(valueToAdd);
  }

  for (auto &shaderVar : currentFloatVariables)
  {
    const int varType = shaderVar.varType;
    const int varId = shaderVar.varId;
    float valueToAdd;
    if (varType == SHVT_REAL)
      valueToAdd = ShaderGlobal::get_real_fast(varId);
    else if (varType == SHVT_INT) // or may be should be error?
      valueToAdd = ShaderGlobal::get_int_fast(varId);
    else
      valueToAdd = shaderVar.val;
    parametersBuffer.push_back(valueToAdd);
  }
  parametersBuffer.resize((parametersBuffer.size() + REGISTERS_ALIGNMENT - 1) / REGISTERS_ALIGNMENT * REGISTERS_ALIGNMENT, 0.f);
  for (auto &shaderVar : currentInt4Variables)
  {
    IPoint4 valueToAdd;
    if (shaderVar.varType == SHVT_INT4)
    {
      valueToAdd = ShaderGlobal::get_int4(shaderVar.varId);
    }
    else
    {
      valueToAdd = shaderVar.val;
    }
    for (uint32_t i = 0; i < 4; ++i)
      parametersBuffer.push_back(bitwise_cast<float, int>(valueToAdd[i]));
  }
  for (auto &shaderVar : currentFloat4Variables)
  {
    Point4 valueToAdd;
    if (shaderVar.varType == SHVT_COLOR4)
    {
      Color4 paramValue = ShaderGlobal::get_color4_fast(shaderVar.varId);
      valueToAdd = Point4(reinterpret_cast<const real *>(&paramValue), Point4::CTOR_FROM_PTR);
    }
    else
      valueToAdd = shaderVar.val;
    for (uint32_t i = 0; i < 4; ++i)
      parametersBuffer.push_back(valueToAdd[i]);
  }

  for (auto &shaderVar : currentFloat4x4Variables)
  {
    Matrix44 valueToAdd;
    // TODO: Ideally this should not be checked every frame and need caching later
    if (shaderVar.varType == SHVT_FLOAT4X4)
    {
      valueToAdd = ShaderGlobal::get_float4x4(shaderVar.varId);
    }
    else
      LOGERR_ONCE("Error expected variable %s to be of type %d, but got %d", shaderVar.paramName, SHVT_FLOAT4X4, shaderVar.varType);

    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        parametersBuffer.push_back(valueToAdd[i][j]);
  }
  G_ASSERT(parametersBuffer.size() % 4 == 0);

  if (parametersBuffer.size()) // call d3d::release_cb0_data(STAGE_CS) after draw
    d3d::set_cb0_data(STAGE_CS, reinterpret_cast<float *>(parametersBuffer.data()), parametersBuffer.size() / REGISTERS_ALIGNMENT);
}

void NodeBasedShaderManager::setTextures(int &offset) const
{
  for (int i = 0, ie = nodeBasedTextures.size(); i < ie; ++i, ++offset)
  {
    const NodeBasedTexture &nbTex = nodeBasedTextures[i];
    if (nbTex.dynamicTextureVarId >= 0)
    {
      TEXTUREID id = ShaderGlobal::get_tex_fast(nbTex.dynamicTextureVarId);
      if (!id)
      {
        LOGWARN_ONCE("Node based shader tried to bind %d shadervar, but the texture is null.",
          VariableMap::getVariableName(nbTex.dynamicTextureVarId));
        continue; // TODO This can hide bugs! Add an optional flag to NBS textures.
      }
      G_FAST_ASSERT(get_managed_texture_refcount(id) > 0);
      mark_managed_tex_lfu(id);
      if (nbTex.dynamicSamplerVarId != -1)
        d3d::set_sampler(STAGE_CS, offset, ShaderGlobal::get_sampler(nbTex.dynamicSamplerVarId));
      d3d::set_tex(STAGE_CS, offset, D3dResManagerData::getBaseTex(id));
    }
    else
    {
      if (nbTex.sampler != d3d::INVALID_SAMPLER_HANDLE)
        d3d::set_sampler(STAGE_CS, offset, nbTex.sampler);
      d3d::set_tex(STAGE_CS, offset, nbTex.usedTextureResId);
    }
  }
}

void NodeBasedShaderManager::setBuffers(int &offset) const
{
  for (int i = 0, ie = nodeBasedBuffers.size(); i < ie; ++i, ++offset)
  {
    const NodeBasedBuffer &nbBuf = nodeBasedBuffers[i];
    if (nbBuf.dynamicBufferVarId >= 0)
    {
      D3DRESID id = ShaderGlobal::get_buf_fast(nbBuf.dynamicBufferVarId);
      Sbuffer *buffer = (Sbuffer *)acquire_managed_res(id);
      d3d::set_buffer(STAGE_CS, offset, buffer);
      G_FAST_ASSERT(get_managed_texture_refcount(id) > 1);
      release_managed_res(id);
    }
    else
    {
      d3d::set_buffer(STAGE_CS, offset, nbBuf.usedBufferResId);
    }
  }
}

void NodeBasedShaderManager::setSubConstantBuffers() const
{
  int offset = 1; // 0 is for default constant buffer
  for (int i = 0, ie = nodeBasedCBuffers.size(); i < ie; ++i, ++offset)
  {
    const NodeBasedBuffer &nbCBuf = nodeBasedCBuffers[i];
    if (nbCBuf.dynamicBufferVarId >= 0)
    {
      D3DRESID id = ShaderGlobal::get_buf_fast(nbCBuf.dynamicBufferVarId);
      Sbuffer *buffer = (Sbuffer *)acquire_managed_res(id);
      d3d::set_const_buffer(STAGE_CS, offset, buffer);
      G_FAST_ASSERT(get_managed_texture_refcount(id) > 1);
      release_managed_res(id);
    }
    else
    {
      d3d::set_const_buffer(STAGE_CS, offset, nbCBuf.usedBufferResId);
    }
  }
}

void NodeBasedShaderManager::setConstants()
{
  if (isDirty)
  {
    isDirty = !invalidateCachedResources();
  }
  if (cachedVariableGeneration != VariableMap::generation())
    precacheVariables();
  setBaseConstantBuffer();
  int offset = 0;
  setTextures(offset);
  setBuffers(offset);
  setSubConstantBuffers();
}

void NodeBasedShaderManager::enableOptionalGraph(const String &graph_name, bool enable)
{
  for (uint32_t i = 0; i < optionalGraphNames.size(); ++i)
  {
    if (optionalGraphNames[i] == graph_name)
    {
      if (enable)
        permutationId |= 1 << i;
      else
        permutationId &= ~(1 << i);
    }
  }
  updateBlkDataResources(permParameters[permutationId]);
}

void NodeBasedShaderManager::precacheVariables()
{
  currentFloatVariables.clear();
  for (int i = 0; i < currentFloatParametersBlk->paramCount(); ++i)
  {
    const char *paramName = currentFloatParametersBlk->getStr(i);
    const int shaderVarId = ::get_shader_variable_id(paramName, true);
    const int varType = ShaderGlobal::get_var_type(shaderVarId);
    if (varType == SHVT_REAL || varType == SHVT_INT)
      currentFloatVariables.emplace_back(CachedFloat{paramName, varType, shaderVarId});
    else
    {
      currentFloatVariables.emplace_back(CachedFloat{paramName, 0.f});
      if (shaderVarId >= 0 && varType >= 0)
        logerr("paramName = %s has type %d not int/real", paramName, varType);
    }
  }
  currentIntVariables.clear();
  for (int i = 0; i < currentIntParametersBlk->paramCount(); ++i)
  {
    const char *paramName = currentIntParametersBlk->getStr(i);
    const int shaderVarId = ::get_shader_variable_id(paramName, true);
    const int varType = ShaderGlobal::get_var_type(shaderVarId);
    if (varType == SHVT_INT)
      currentIntVariables.emplace_back(CachedInt{paramName, varType, shaderVarId});
    else
    {
      currentIntVariables.emplace_back(CachedInt{paramName, 0});
      if (shaderVarId >= 0 && varType >= 0)
        logerr("paramName = %s has type %d not int", paramName, varType);
    }
  }
  currentInt4Variables.clear();
  for (int i = 0; i < currentInt4ParametersBlk->paramCount(); ++i)
  {
    const char *paramName = currentInt4ParametersBlk->getStr(i);
    const int shaderVarId = ::get_shader_variable_id(paramName, true);
    const int varType = ShaderGlobal::get_var_type(shaderVarId);
    if (varType == SHVT_INT4)
      currentInt4Variables.emplace_back(CachedInt4{paramName, varType, shaderVarId});
    else
    {
      currentInt4Variables.emplace_back(CachedInt4{paramName, IPoint4(0, 0, 0, 0)});
      if (shaderVarId >= 0 && varType >= 0)
        logerr("paramName = %s has type %d not int4", paramName, varType);
    }
  }
  currentFloat4Variables.clear();
  for (int i = 0; i < currentFloat4ParametersBlk->paramCount(); ++i)
  {
    const char *paramName = currentFloat4ParametersBlk->getStr(i);
    const char *arrayOpen = strstr(paramName, "[");
    const bool isArray = arrayOpen != nullptr;
    if (isArray)
    {
      int fillIx = 0;
      int arraySize = strtol(arrayOpen + 1, nullptr, 10);
      if (const ArrayValue *arrayValue = getArrayValue(paramName, arrayOpen - paramName))
      {
        G_ASSERT(arrayValue->second->size() <= arraySize);
        for (const Point4 &value : *arrayValue->second)
        {
          currentFloat4Variables.emplace_back(CachedFloat4{paramName, value});
          fillIx++;
        }
      }

      for (; fillIx < arraySize; ++fillIx)
        currentFloat4Variables.emplace_back(CachedFloat4{paramName, Point4(0, 0, 0, 0)});
    }
    else
    {
      const int shaderVarId = ::get_shader_variable_id(paramName, true);
      const int varType = ShaderGlobal::get_var_type(shaderVarId);
      if (varType == SHVT_COLOR4)
        currentFloat4Variables.emplace_back(CachedFloat4{paramName, varType, shaderVarId});
      else
        currentFloat4Variables.emplace_back(CachedFloat4{paramName, Point4(0, 0, 0, 0)});
    }
  }
  currentFloat4x4Variables.clear();
  for (int i = 0; i < currentFloat4x4ParametersBlk->paramCount(); i++)
  {
    // WARNING: arrayValue seems to be not implemented (anymore?, there is no setArrayValue anywhere), and float4x4 does not support
    // it.

    const char *paramName = currentFloat4x4ParametersBlk->getStr(i);
    const int shaderVarId = ::get_shader_variable_id(paramName, true);
    const int varType = ShaderGlobal::get_var_type(shaderVarId);
    if (varType == SHVT_FLOAT4X4)
      currentFloat4x4Variables.emplace_back(CachedFloat4x4{paramName, varType, shaderVarId});
    else
      currentFloat4x4Variables.emplace_back(CachedFloat4x4{paramName, Matrix44::ZERO});
  }
  cachedVariableGeneration = VariableMap::generation();
}


void NodeBasedShaderManager::fillTextureCache(const DataBlock &src_block, int offset, bool &has_loaded, bool has_sampler)
{
  for (int i = 0, ei = src_block.paramCount(); i < ei; ++i)
  {
    const char *texName = src_block.getStr(i);
    NodeBasedTexture &nbTex = nodeBasedTextures[i + offset];
    TEXTUREID resId = get_managed_texture_id(texName);
    bool isDynamic = resId == BAD_TEXTUREID;
    nbTex.dynamicSamplerVarId = -1;
    nbTex.sampler = d3d::INVALID_SAMPLER_HANDLE;
    nbTex.dynamicTextureVarId = isDynamic ? ::get_shader_variable_id(texName, true) : -1;
    if (nbTex.dynamicTextureVarId != -1 && has_sampler)
    {
      eastl::string samplerName = eastl::string(texName) + "_samplerstate";
      nbTex.dynamicSamplerVarId = ::get_shader_variable_id(samplerName.c_str(), true);
    }
    nbTex.usedTextureResId = isDynamic ? nullptr : acquire_managed_tex(resId);
    if (nbTex.usedTextureResId != nullptr)
      nbTex.sampler = d3d::request_sampler({});
    if (!isDynamic)
    {
      resIdsToRelease.push_back(resId);
      if (!nbTex.usedTextureResId)
        has_loaded = false;
    }
    else if (ShaderGlobal::get_tex_fast(nbTex.dynamicTextureVarId) == BAD_TEXTUREID)
    {
      nbTex.dynamicTextureVarId = -1;
      nbTex.dynamicSamplerVarId = -1;
    }
  }
}


void NodeBasedShaderManager::fillBufferCache(const DataBlock &src_block, dag::Vector<NodeBasedBuffer> &bufferCache, int offset,
  bool &has_loaded)
{
  for (int i = 0, ei = src_block.paramCount(); i < ei; ++i)
  {
    const char *bufferName = src_block.getStr(i);
    NodeBasedBuffer &nbBuf = bufferCache[i + offset];
    D3DRESID resId = get_managed_res_id(bufferName);
    bool isDynamic = resId == BAD_D3DRESID;
    nbBuf.dynamicBufferVarId = isDynamic ? ::get_shader_variable_id(bufferName, true) : -1;
    nbBuf.usedBufferResId = isDynamic ? nullptr : (Sbuffer *)acquire_managed_res(resId);
    if (!isDynamic)
    {
      resIdsToRelease.push_back(resId);
      if (!nbBuf.usedBufferResId)
        has_loaded = false;
    }
    else if (ShaderGlobal::get_buf_fast(nbBuf.dynamicBufferVarId) == BAD_TEXTUREID)
      nbBuf.dynamicBufferVarId = -1;
  }
}

bool NodeBasedShaderManager::invalidateCachedResources()
{
  node_based_shaders_with_cached_resources.insert(this);
  bool hasLoaded = true;
  for (D3DRESID id : resIdsToRelease)
    release_managed_res(id);
  resIdsToRelease.resize(0);

  {
    int texCnt = 0;
    texCnt += currentTextures2dBlk->paramCount();
    texCnt += currentTextures3dBlk->paramCount();
    texCnt += currentTextures2dArrayBlk->paramCount();
    texCnt += currentTextures2dShdArrayBlk->paramCount();
    texCnt += currentTextures2dNoSamplerBlk->paramCount();
    texCnt += currentTextures3dNoSamplerBlk->paramCount();
    nodeBasedTextures.resize(texCnt);
  }

  nodeBasedBuffers.resize(currentBuffersBlk->paramCount());
  nodeBasedCBuffers.resize(currentCBuffersBlk->paramCount());

  resIdsToRelease.reserve(nodeBasedTextures.size() + nodeBasedBuffers.size() + nodeBasedCBuffers.size());

  int offset = 0;
  auto fillTexCache([&](const DataBlock &src_block, bool has_sampler) {
    fillTextureCache(src_block, offset, hasLoaded, has_sampler);
    offset += src_block.paramCount();
  });

  fillTexCache(*currentTextures2dBlk, true);
  fillTexCache(*currentTextures3dBlk, true);
  fillTexCache(*currentTextures2dArrayBlk, true);
  fillTexCache(*currentTextures2dShdArrayBlk, true);
  fillTexCache(*currentTextures2dNoSamplerBlk, false);
  fillTexCache(*currentTextures3dNoSamplerBlk, false);

  fillBufferCache(*currentBuffersBlk, nodeBasedBuffers, 0, hasLoaded);
  fillBufferCache(*currentCBuffersBlk, nodeBasedCBuffers, 0, hasLoaded);

  return hasLoaded;
}

void NodeBasedShaderManager::resetSubCbuffers()
{
  for (int i = 1; i <= nodeBasedCBuffers.size(); i++)
  {
    d3d::set_const_buffer(STAGE_CS, i, nullptr);
  }
}

void NodeBasedShaderManager::clearAllCachedResources()
{
  for (NodeBasedShaderManager *mgr : node_based_shaders_with_cached_resources)
    mgr->resetCachedResources();
  node_based_shaders_with_cached_resources.clear();
}

const char *node_based_shader_current_platform_suffix() { return PLATFORM_LABELS[get_nbsm_platform()]; }

eastl::string node_based_shader_get_resource_name(const char *shader_path)
{
  return eastl::string(eastl::string::CtorSprintf{}, "%s_%s", normalize_shader_res_name(shader_path).c_str(),
    node_based_shader_current_platform_suffix());
}
#endif
