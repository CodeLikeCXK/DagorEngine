//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <gui/dag_stdGuiRender.h>
#include <dasModules/aotDagorStdGuiRender.h>
#include <daScript/daScript.h>
#include <daRg/dag_renderState.h>
#include <daRg/dag_renderObject.h>
#include <daRg/dag_element.h>
#include <daRg/dag_picture.h>

MAKE_TYPE_FACTORY(RenderState, darg::RenderState)
MAKE_TYPE_FACTORY(ElemRenderData, darg::ElemRenderData)
MAKE_TYPE_FACTORY(Element, darg::Element)
MAKE_TYPE_FACTORY(Properties, darg::Properties)
MAKE_TYPE_FACTORY(Picture, ::darg::Picture)

template <>
struct das::isCloneable<::darg::Picture> : false_type
{};

namespace darg
{
E3DCOLOR props_get_color(const ::darg::Properties &props, const char *key, E3DCOLOR def);
bool props_get_bool(const ::darg::Properties &props, const char *key, bool def);
float props_get_float(const ::darg::Properties &props, const char *key, float def);
int props_get_int(const ::darg::Properties &props, const char *key, int def);
::darg::Picture *props_get_picture(const ::darg::Properties &props, const char *key);
DataBlock *props_get_blk(const ::darg::Properties &props, const char *key);
} // namespace darg