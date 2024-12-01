// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "shcode.h"
#include "varMap.h"
#include "namedConst.h"
#include "shLog.h"
#include <shaders/shUtils.h>
#include <shaders/shOpcodeFormat.h>
#include <shaders/shOpcode.h>

#include <generic/dag_tabUtils.h>
#include <debug/dag_debug.h>

// create if needed passes for dynamic variant #n
ShaderCode::PassTab *ShaderCode::createPasses(int variant)
{
  if (!tabutils::isCorrectIndex(passes, variant))
    return nullptr;

  if (!passes[variant])
    passes[variant] = new PassTab();
  return passes[variant];
}

ShaderCode::ShaderCode(IMemAlloc *mem) : channel(mem), initcode(mem), stvarmap(mem), passes(mem), flags(0) {}

void ShaderCode::link()
{
  for (auto &pass : passes)
    if (pass)
    {
      for (auto &blk : pass->suppBlk)
      {
        const char *bname = blk->name.c_str();
        ShaderStateBlock *sb = !blk->name.empty() ? ShaderStateBlock::findBlock(bname) : ShaderStateBlock::emptyBlock();
        if (sb)
          blk = sb;
        else
          sh_debug(SHLOG_FATAL, "undefined block <%s>", bname);
      }
    }
}

unsigned int ShaderCode::getVertexStride() const
{
  unsigned int vertexStride = 0;
  for (unsigned int channelNo = 0; channelNo < channel.size(); channelNo++)
  {
    switch (channel[channelNo].t)
    {
      case SCTYPE_FLOAT1: vertexStride += 4; break;
      case SCTYPE_FLOAT2: vertexStride += 8; break;
      case SCTYPE_FLOAT3: vertexStride += 12; break;
      case SCTYPE_FLOAT4: vertexStride += 16; break;
      case SCTYPE_SHORT2: vertexStride += 4; break;
      case SCTYPE_SHORT4: vertexStride += 8; break;
      case SCTYPE_E3DCOLOR: vertexStride += 4; break;
      case SCTYPE_UBYTE4: vertexStride += 4; break;
      case SCTYPE_HALF2: vertexStride += 4; break;
      case SCTYPE_HALF4: vertexStride += 8; break;
      case SCTYPE_SHORT2N: vertexStride += 4; break;
      case SCTYPE_SHORT4N: vertexStride += 8; break;
      case SCTYPE_USHORT2N: vertexStride += 4; break;
      case SCTYPE_USHORT4N: vertexStride += 8; break;
      case SCTYPE_UDEC3: vertexStride += 4; break;
      case SCTYPE_DEC3N: vertexStride += 4; break;
      default: DAG_FATAL("Unknown channel #%d type: 0x%p", channelNo, channel[channelNo].t);
    }
  }
  return vertexStride;
}

void ShaderClass::sortStaticVarsByMode()
{
  Tab<eastl::pair<Var, int>> argvars{};
  argvars.reserve(stvar.size());
  int i = 0;
  eastl::transform(stvar.cbegin(), stvar.cend(), argvars.begin(), [&i](const Var &var) { return eastl::make_pair(var, i++); });

  // First statics, then dynamics, with as little reordering as possible
  eastl::stable_partition(argvars.begin(), argvars.end(), [this](const auto &p) { return !stvarsAreDynamic[p.second]; });

  // We don't need this data anymore, and no need to remap it
  stvarsAreDynamic.clear();

  Tab<int> remapping(argvars.size());
  for (size_t i = 0; i < argvars.size(); ++i)
  {
    stvar[i] = argvars[i].first;
    remapping[argvars[i].second] = i;
  }

  // Now patch all stvarmaps for shader codes with the new mapping
  for (ShaderCode *shcode : code)
  {
    if (shcode)
    {
      for (ShaderCode::StVarMap &mapping : shcode->stvarmap)
        mapping.sv = remapping[mapping.sv];
    }
  }
}

int ShaderClass::find_static_var(const int variable_name_id)
{
  for (int i = 0; i < stvar.size(); ++i)
    if (stvar[i].nameId == variable_name_id)
      return i;
  return -1;
}
