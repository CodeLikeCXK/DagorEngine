//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include "ssao_common.h"
#include <3d/dag_texMgr.h>
#include <3d/dag_resPtr.h>
#include <3d/dag_textureIDHolder.h>
#include <drv/3d/dag_consts.h>
#include <render/viewDependentResource.h>
#include <generic/dag_carray.h>
#include <math/dag_Point3.h>
#include <math/dag_TMatrix4.h>
#include <EASTL/utility.h>
#include <EASTL/array.h>
#include <render/subFrameSample.h>
#include <shaders/dag_postFxRenderer.h>
#include <shaders/dag_computeShaders.h>

class BaseTexture;
typedef BaseTexture Texture;

class ISSAORenderer
{
public:
  ISSAORenderer() = default;
  ISSAORenderer(int, int, int, uint32_t = SSAO_NONE);
  virtual ~ISSAORenderer() = default;

  virtual int getWidth() const { return aoWidth; }
  virtual int getHeight() const { return aoHeight; }

  virtual void reset() = 0;
  virtual void changeResolution(int, int) {}

  virtual Texture *getSSAOTex() = 0;
  virtual TEXTUREID getSSAOTexId() = 0;

  virtual void render(const TMatrix &view_tm, const TMatrix4 &proj_tm, BaseTexture *tex, const ManagedTex *ssaoTex,
    const ManagedTex *prevSsaoTex, const ManagedTex *tmpTex, const DPoint3 &world_pos,
    SubFrameSample sub_sample = SubFrameSample::Single)
  {
    render(view_tm, proj_tm, tex, ssaoTex, prevSsaoTex, tmpTex, &world_pos, sub_sample);
  }
  virtual void render(const TMatrix &view_tm, const TMatrix4 &proj_tm, BaseTexture *tex = nullptr)
  {
    render(view_tm, proj_tm, tex, nullptr, nullptr, nullptr, nullptr);
  }
  virtual void render(const TMatrix &view_tm, const TMatrix4 &proj_tm, const ManagedTex &tex)
  {
    render(view_tm, proj_tm, tex.getTex2D(), nullptr, nullptr, nullptr, nullptr);
  }
  virtual void setCurrentView(int view) = 0;

protected:
  virtual void render(const TMatrix &view_tm, const TMatrix4 &proj_tm, BaseTexture *ssaoDepthTexUse, const ManagedTex *ssaoTex,
    const ManagedTex *prevSsaoTex, const ManagedTex *tmpTex, const DPoint3 *world_pos,
    SubFrameSample sub_sample = SubFrameSample::Single) = 0;

  int aoWidth, aoHeight;
  eastl::unique_ptr<PostFxRenderer> aoRenderer;
  eastl::unique_ptr<ComputeShaderElement> aoRendererCS;
};
