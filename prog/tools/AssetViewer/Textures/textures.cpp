// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "textures.h"
#include "tex_util.h"

#include "../av_appwnd.h"

#include <assets/asset.h>
#include <assets/assetHlp.h>
#include <shaders/dag_shaders.h>

#include <propPanel/control/container.h>
#include <winGuiWrapper/wgw_input.h>

#include <libTools/util/strUtil.h>
#include <libTools/util/iLogWriter.h>
#include <libTools/dtx/ddsxPlugin.h>
#include <de3_dynRenderService.h>
#include <de3_dxpFactory.h>

#include <3d/dag_render.h>
#include <3d/dag_texMgr.h>
#include <3d/dag_texPackMgr2.h>
#include <drv/3d/dag_renderStates.h>
#include <drv/3d/dag_viewScissor.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <drv/3d/dag_platform_pc.h>
#include <3d/ddsxTex.h>
#include <3d/ddsFormat.h>
#include <3d/ddsxTexMipOrder.h>
#include <util/dag_texMetaData.h>
#include <shaders/dag_shaderMesh.h>
#include <ioSys/dag_zlibIo.h>
#include <ioSys/dag_lzmaIo.h>
#include <ioSys/dag_zstdIo.h>
#include <ioSys/dag_oodleIo.h>
#include <ioSys/dag_memIo.h>
#include <ioSys/dag_fileIo.h>

#include <osApiWrappers/dag_direct.h>

#include <shaders/dag_overrideStates.h>
#include <render/renderLightsConsts.hlsli>
#include <3d/tql.h>

enum
{
  TEX_COLOL_MODE_RGB,
  TEX_COLOL_MODE_A,
  TEX_COLOL_MODE_RGBA,
  TEX_COLOL_MODE_TOO_DARK,
  TEX_COLOL_MODE_TOO_BRIGHT,
};

namespace texmgr_internal
{
extern TexQL max_allowed_q;
}

//=============================================================================
TexturesPlugin::TexturesPlugin()
{
  // initScriptPanelEditor("tex.scheme.nut", "texture by scheme");

  shaders::OverrideState state;
  state.set(shaders::OverrideState::BLEND_SRC_DEST);
  state.sblend = BLEND_ONE;
  state.dblend = BLEND_ZERO;
  state.set(shaders::OverrideState::BLEND_SRC_DEST_A);
  state.sblenda = BLEND_ZERO;
  state.dblenda = BLEND_ZERO;
  state.set(shaders::OverrideState::Z_TEST_DISABLE);
  state.set(shaders::OverrideState::Z_WRITE_DISABLE);
  renderVolImageOverrideId = shaders::overrides::create(state);
}


//=============================================================================
TexturesPlugin::~TexturesPlugin()
{
  ::release_managed_tex(texId);
  texId = BAD_TEXTUREID;
}

void TexturesPlugin::setIesData()
{
  iesParams = IesReader::IesParams{};
  iesTexture = curAsset && curAsset->props.getBool("buildIES", false);
  if (iesTexture)
  {
    TextureMetaData tmd;
    tmd.read(curAsset->props, "PC");
    iesScale = tmd.getIesScale();
    iesRotated = bool(tmd.flags & tmd.FLG_IES_ROT);

    iesParams.blurRadius = curAsset->props.getReal("blurRadius", iesParams.blurRadius * RAD_TO_DEG) * DEG_TO_RAD;
    iesParams.phiMin = curAsset->props.getReal("phiMin", iesParams.phiMin * RAD_TO_DEG) * DEG_TO_RAD;
    iesParams.phiMax = curAsset->props.getReal("phiMax", iesParams.phiMax * RAD_TO_DEG) * DEG_TO_RAD;
    iesParams.thetaMin = curAsset->props.getReal("thetaMin", iesParams.thetaMin * RAD_TO_DEG) * DEG_TO_RAD;
    iesParams.thetaMax = curAsset->props.getReal("thetaMax", iesParams.thetaMax * RAD_TO_DEG) * DEG_TO_RAD;
    iesParams.edgeFadeout = curAsset->props.getReal("edgeFadeout", iesParams.edgeFadeout * RAD_TO_DEG) * DEG_TO_RAD;

    IesReader ies = IesReader(iesParams);
    if (ies.read(curAsset->getTargetFilePath(), get_app().getConsole()))
    {

      int width = curAsset->props.getInt("textureWidth", 128);
      int height = curAsset->props.getInt("textureHeight", 128);

      IesReader::TransformParams transformParams;
      transformParams.zoom = iesScale;
      transformParams.rotated = iesRotated;
#if USE_OCTAHEDRAL_MAPPING
      IesReader::TransformParams optimalParams = ies.getOctahedralOptimalTransform(width, height);
      iesValid = ies.isOctahedralTransformValid(width, height, transformParams);
      iesWastedArea = ies.getOctahedralRelativeWaste(width, height, transformParams);
#else
      IesReader::TransformParams optimalParams = ies.getSphericalOptimalTransform(width, height);
      iesValid = ies.isSphericalTransformValid(width, height, transformParams);
      iesWastedArea = ies.getSphericalRelativeWaste(width, height, transformParams);
#endif
      tmd.setIesScale(optimalParams.zoom);
      iesOptimalScale = tmd.getIesScale();
      iesOptimalRotated = optimalParams.rotated;
    }
    else
    {
      iesParams = IesReader::IesParams{};
      iesOptimalScale = -1;
      iesOptimalRotated = false;
    }
  }
}

//=============================================================================
bool TexturesPlugin::begin(DagorAsset *asset)
{
  if (curAsset != asset)
    viewTile = Point2(1, 1);
  if (IDynRenderService *drSrv = EDITORCORE->queryEditorInterface<IDynRenderService>())
    if (drSrv->getDebugShowType() != 0)
      drSrv->setDebugShowType(0);

  curAsset = asset;
  if (curAsset)
  {
    if (trail_strcmp(curAsset->getName(), "$uhq"))
      curUhqAsset = curAsset;
    else
    {
      curUhqAsset = curAsset->getMgr().findAsset(String(0, "%s$uhq", curAsset->getName()), curAsset->getType());
      curHqAsset = curAsset->getMgr().findAsset(String(0, "%s$hq", curAsset->getName()), curAsset->getType());
      curTqAsset = curAsset->getMgr().findAsset(String(0, "%s$tq", curAsset->getName()), curAsset->getType());
    }
  }
  else
    curUhqAsset = curHqAsset = curTqAsset = nullptr;

  ::release_managed_tex(texId);

  setIesData();

  texId = ::add_managed_texture(String(64, "%s*", curAsset->getName()));

  texture = ::acquire_managed_tex(texId);
  if (!texture)
    texId = BAD_TEXTUREID;
  ddsx::tex_pack2_perform_delayed_data_loading();

  mColor = E3DCOLOR(255, 255, 255, 255);
  cMul.set(1, 1, 1, 1), cAdd.set(0, 0, 0, 0);
  showTexQ = ID_SHOW_TEXQ_HIGH;
  isMoving = false;
  if (forceStretch)
    texMove = Point2(0, 0);
  priorMousePos = Point2(0, 0);

  hasAlpha = false;
  chanelsRGB = 3;
  if (texture)
  {
    const char *fmt = d3d::pcwin32::get_texture_format_str(texture);
    if (strchr(fmt, 'A') || (strncmp(fmt, "DXT", 3) == 0 && fmt[3] != '1') || strnicmp(fmt, "BC7", 3) == 0)
      hasAlpha = true;

    if (stricmp(fmt, "ATI1N") == 0)
    {
      chanelsRGB = 1;
      hasAlpha = false;
    }
    else if (stricmp(fmt, "ATI2N") == 0)
    {
      chanelsRGB = 2;
      hasAlpha = false;
    }
  }

  colorModeSVId = ::get_shader_glob_var_id("text_color_mode", true);
  if (colorModeSVId != -1)
    ShaderGlobal::set_int_fast(colorModeSVId, hasAlpha ? TEX_COLOL_MODE_RGBA : TEX_COLOL_MODE_RGB);
  else
    get_app().getConsole().addMessage(ILogWriter::WARNING, "Shader for texture plugin not found");

  aTestSVId = ::get_shader_glob_var_id("tex_a_test_value", true);
  if (aTestSVId)
    ShaderGlobal::set_int_fast(aTestSVId, 1);

  shader.init("tex_view");

  if (spEditor && asset)
    spEditor->load(asset);
  return true;
}

bool TexturesPlugin::end()
{
  shader.close();

  if (spEditor)
    spEditor->destroyPanel();
  if (texture)
    texture->texmiplevel(-1, 0);
  texture = NULL;
  ::release_managed_tex(texId);
  texId = BAD_TEXTUREID;
  curUhqAsset = NULL;
  curHqAsset = NULL;
  curTqAsset = NULL;
  return true;
}

bool TexturesPlugin::reloadAsset(DagorAsset *asset)
{
  ::release_managed_tex(texId);
  discard_unused_managed_texture(texId);

  texture = ::acquire_managed_tex(texId);
  if (!texture)
    texId = BAD_TEXTUREID;
  ddsx::tex_pack2_perform_delayed_data_loading();
  curUhqAsset = curAsset ? curAsset->getMgr().findAsset(String(0, "%s$uhq", curAsset->getName()), curAsset->getType()) : NULL;
  curHqAsset = curAsset ? curAsset->getMgr().findAsset(String(0, "%s$hq", curAsset->getName()), curAsset->getType()) : NULL;
  curTqAsset = curAsset ? curAsset->getMgr().findAsset(String(0, "%s$tq", curAsset->getName()), curAsset->getType()) : NULL;
  setIesData();
  get_app().fillPropPanel();
  return true;
}

//=============================================================================

void TexturesPlugin::renderObjects()
{
  if (!texture)
    return;

  int vp_w = 1, vp_h = 1, tex_w = 1, tex_h = 1;
  if (IGenViewportWnd *vp = EDITORCORE->getViewport(0))
  {
    vp->getViewportSize(vp_w, vp_h);
    TextureInfo ti;
    texture->getinfo(ti);
    tex_w = ti.w, tex_h = ti.h;

    real wk = float(vp_w - 8) / tex_w;
    real hk = float(vp_h - 8) / tex_h;
    real k = wk < hk ? wk : hk;
    tex_w = tex_w * k;
    tex_h = tex_h * k;
  }
  else
    return;

  struct
  {
    int x, y, w, h;
    float minz, maxz;
  } lvp;
  d3d::getview(lvp.x, lvp.y, lvp.w, lvp.h, lvp.minz, lvp.maxz);

  d3d::settm(TM_WORLD, &Matrix44::IDENT);
  d3d::setwire(false);

  if (texture->restype() == RES3D_TEX)
  {
    TextureInfo ti;
    texture->getinfo(ti);
    if (showMipStripe)
    {
      if (viewMipLevel > 0)
      {
        ti.w = max(ti.w >> viewMipLevel, 1);
        ti.h = max(ti.h >> viewMipLevel, 1);
      }
      ti.w *= 2;
    }

    StdGuiRender::start_render(vp_w, vp_h);

    if (colorModeSVId != -1)
      StdGuiRender::set_shader(&shader);

    StdGuiRender::set_color(e3dcolor(color4(mColor) * powf(10, cScaleDb / 10)));

    StdGuiRender::set_texture(texId, d3d::INVALID_SAMPLER_HANDLE); // TODO: Use actual sampler IDs
    StdGuiRender::set_ablend(false);

    if (!forceStretch)
      ti.w *= texScale, ti.h *= texScale;
    real wk = float(vp_w - 8) / ti.w;
    real hk = float(vp_h - 8) / ti.h;

    real k = wk < hk ? wk : hk;

    if (forceStretch)
      ti.w *= k, ti.h *= k;

    Point2 tc0 = Point2(0, 0);
    Point2 tc1 = viewTile;
    int l = (vp_w - ti.w) / 2, t = (vp_h - ti.h) / 2;

    int x0 = l;
    int y0 = t;
    int x1 = l + ti.w;
    int y1 = t + ti.h;

    // moving

    if (!forceStretch)
    {
      static const int TEX_PADDING = 60;
      if (wk < 1.0)
      {
        if (l + texMove.x > TEX_PADDING)
          texMove.x = TEX_PADDING - l;
        else if (l + texMove.x + ti.w < vp_w - TEX_PADDING)
          texMove.x = vp_w - TEX_PADDING - l - ti.w;

        l += texMove.x;
        x0 += texMove.x;
        x1 += texMove.x;
      }

      if (hk < 1.0)
      {
        if (t + texMove.y > TEX_PADDING)
          texMove.y = TEX_PADDING - t;
        else if (t + texMove.y + ti.h < vp_h - TEX_PADDING)
          texMove.y = vp_h - TEX_PADDING - t - ti.h;

        t += texMove.y;
        y0 += texMove.y;
        y1 += texMove.y;
      }
    }

    if (!showMipStripe)
      StdGuiRender::render_rect_t(x0, y0, x1, y1, tc0, tc1);
    else
    {
      texture->getinfo(ti);
      if (forceStretch)
        ti.w *= k, ti.h *= k;
      else
        ti.w *= texScale, ti.h *= texScale;
      for (int i = max(viewMipLevel, 0); i < texture->level_count(); i++)
      {
        texture->texmiplevel(i, i);
        x1 = x0 + max(ti.w >> i, 1);
        y1 = y0 + max(ti.h >> i, 1);
        StdGuiRender::render_rect_t(x0, y0, x1, y1, tc0, tc1);
        StdGuiRender::flush_data();
        x0 = x1;
      }
      texture->texmiplevel(viewMipLevel, viewMipLevel < 0 ? 0 : viewMipLevel);
    }

    StdGuiRender::reset_shader();
    StdGuiRender::flush_data();

    StdGuiRender::end_render();
  }
  else if (texture->restype() == RES3D_CUBETEX)
  {
    d3d::setview((vp_w - tex_w) / 2, (vp_h - tex_h) / 2, tex_w, tex_h, 0, 1);
    shaders::overrides::set_master_state(shaders::overrides::get(renderVolImageOverrideId));
    EDITORCORE->queryEditorInterface<IDynRenderService>()->renderEnviCubeTexture(texture, cMul * powf(10, cScaleDb / 10), cAdd);
    shaders::overrides::reset_master_state();
  }
  else if (texture->restype() == RES3D_VOLTEX)
  {
    d3d::setview((vp_w - tex_w) / 2, (vp_h - tex_h) / 2, tex_w, tex_h, 0, 1);
    shaders::overrides::set_master_state(shaders::overrides::get(renderVolImageOverrideId));
    EDITORCORE->queryEditorInterface<IDynRenderService>()->renderEnviVolTexture(texture, cMul * powf(10, cScaleDb / 10), cAdd,
      Color4(0.5, -0.5, 0.5, 0.5), tcZ);
    shaders::overrides::reset_master_state();
  }

  ShaderElement::invalidate_cached_state_block();
  d3d::setview(lvp.x, lvp.y, lvp.w, lvp.h, lvp.minz, lvp.maxz);
  d3d::setwire(::grs_draw_wire);
}


//=============================================================================
bool TexturesPlugin::supportAssetType(const DagorAsset &asset) const { return strcmp(asset.getTypeStr(), "tex") == 0; }


//=============================================================================

void TexturesPlugin::fillPropPanel(PropPanel::ContainerPropertyControl &panel)
{
  panel.setEventHandler(this);

  // texture info

  if (!texture)
    return;

  if (texture->restype() == RES3D_TEX)
    panel.createPoint2(ID_TEX_TILE, "View with texture tile:", viewTile);

  String _tex_type;
  String _tex_size;
  unsigned _tex_c_flags = 0;
  TextureInfo ti;
  texture->getinfo(ti);

  switch (texture->restype())
  {
    case RES3D_TEX:
    {
      _tex_size = String(32, "%d x %d", ti.w, ti.h);
      _tex_c_flags = ti.cflg;
      _tex_type = "Texture";
    }
    break;

    case RES3D_CUBETEX:
    {
      _tex_size = String(32, "%d", ti.w);
      _tex_c_flags = ti.cflg;
      _tex_type = "Cubemap";
    }
    break;

    case RES3D_VOLTEX:
    {
      _tex_size = String(32, "%d x %d x %d", ti.w, ti.h, ti.d);
      _tex_c_flags = ti.cflg;
      _tex_type = "Volume";
    }
    break;
  }

  PropPanel::ContainerPropertyControl *igrp = panel.createGroupBox(ID_INFO_GRP, "Texture info");

  igrp->createStatic(0, "Type:");
  igrp->createStatic(0, _tex_type, false);

  if (const char *fmtStr = d3d::pcwin32::get_texture_format_str(texture))
  {
    if (strlen(fmtStr) > 10)
      igrp->createStatic(0, String(0, "Format:  %s", fmtStr));
    else
    {
      igrp->createStatic(0, "Format:");
      igrp->createStatic(0, fmtStr, false);
    }
  }
  else
    igrp->createStatic(0, "Format:");

  igrp->createStatic(0, "Size:");
  igrp->createStatic(ID_TEX_SIZE_LABEL, _tex_size, false);

  igrp->createStatic(0, "Memory size:");
  igrp->createStatic(ID_TEX_MEMSZ_LABEL, String(32, "%dK", tql::sizeInKb(texture->ressize())), false);

  igrp->createStatic(0, "Mipmap levels:");
  igrp->createStatic(ID_TEX_MIPS_LABEL, String(32, "%d", texture->level_count()), false);

  igrp->createStatic(0, "Conv. quality:");
  igrp->createStatic(0, texconvcache::is_tex_built_fast(*curAsset, _MAKE4C('PC'), NULL) ? "FAST" : "final", false);

#define GET_PROP(TYPE, PROP, DEF) props.get##TYPE(PROP, &props != &curAsset->props ? curAsset->props.get##TYPE(PROP, DEF) : DEF)
  const DataBlock &props = curAsset->getProfileTargetProps(_MAKE4C('PC'), NULL);
  if (GET_PROP(Bool, "rtMipGen", false))
  {
    igrp->createStatic(0, "RT mipGen:");
    igrp->createStatic(0, GET_PROP(Str, "mipFilter", ""), false);
  }
  if (GET_PROP(Str, "pairedToTex", NULL) && *GET_PROP(Str, "pairedToTex", NULL))
    igrp->createStatic(0, "Paired texture");
  if (GET_PROP(Str, "baseTex", NULL) && *GET_PROP(Str, "baseTex", NULL))
  {
    igrp->createStatic(0, "Base texture:");
    igrp->createStatic(0, String(260, "  %s", GET_PROP(Str, "baseTex", "")));
    E3DCOLOR btt = GET_PROP(E3dcolor, "baseTexTolerance", E3DCOLOR(0, 0, 0, 0));
    igrp->createStatic(0, "BT tolerance:");
    igrp->createStatic(0, String(64, "%d, %d, %d, %d", btt.r, btt.g, btt.g, btt.a), false);
    if (GET_PROP(Bool, "baseTexAlphaPatch", false))
      igrp->createStatic(0, "BT use alpha patch");

    ddsx::Buffer b;
    if (texconvcache::get_tex_asset_built_ddsx(curHqAsset ? *curHqAsset : *curAsset, b, _MAKE4C('PC'), NULL, NULL))
    {
      ddsx::Header hdr;
      InPlaceMemLoadCB crd(b.ptr, b.len);

      crd.read(&hdr, sizeof(hdr));
      int blk_total = (hdr.w / 4) * (hdr.h / 4);
      int blk_empty = 0;

      if (hdr.flags & ddsx::Header::FLG_NEED_PAIRED_BASETEX)
      {
        if (hdr.isCompressionZSTD())
        {
          ZstdLoadCB zcrd(crd, hdr.packedSz);
          zcrd.readInt();
          zcrd.readInt();
          zcrd.readInt();
          blk_empty = zcrd.readInt();
          zcrd.ceaseReading();
        }
        else if (hdr.isCompressionOODLE())
        {
          OodleLoadCB zcrd(crd, hdr.packedSz, hdr.memSz);
          zcrd.readInt();
          zcrd.readInt();
          zcrd.readInt();
          blk_empty = zcrd.readInt();
          zcrd.ceaseReading();
        }
        else if (hdr.isCompressionZLIB())
        {
          ZlibLoadCB zlib_crd(crd, hdr.packedSz);
          zlib_crd.readInt();
          zlib_crd.readInt();
          zlib_crd.readInt();
          blk_empty = zlib_crd.readInt();
          zlib_crd.ceaseReading();
        }
        else if (hdr.isCompression7ZIP())
        {
          LzmaLoadCB lzma_crd(crd, hdr.packedSz);
          lzma_crd.readInt();
          lzma_crd.readInt();
          lzma_crd.readInt();
          blk_empty = lzma_crd.readInt();
          lzma_crd.ceaseReading();
        }
        else
        {
          crd.seekrel(3 * 4);
          blk_empty = crd.readInt();
        }

        igrp->createStatic(0, "BT data diff:");
        igrp->createStatic(0, String(32, "%d%%", (blk_total - blk_empty) * 100 / blk_total), false);
        igrp->createStatic(0, String(64, "  %d / %d DXT blk.", blk_total - blk_empty, blk_total));
      }
      b.free();
    }
  }
  if (int splitAt = GET_PROP(Int, "splitAt", 0))
  {
    igrp->createStatic(0, "Split at:");
    igrp->createStatic(0, String(0, "%dx%d", splitAt, splitAt), false);

    const char *pkg = curAsset->getCustomPackageName("PC", NULL);
    igrp->createStatic(0, "BQ pkg:");
    igrp->createStatic(0, !pkg || strcmp(pkg, "*") == 0 ? "-MAIN-" : pkg, false);
    if (curHqAsset)
    {
      pkg = curHqAsset->getCustomPackageName("PC", NULL);
      igrp->createStatic(0, "HQ pkg:");
      igrp->createStatic(0, !pkg || strcmp(pkg, "*") == 0 ? "-MAIN-" : pkg, false);
    }
  }

  igrp->createStatic(0, "Gamma:");
  igrp->createStatic(0, String(32, "%.1f", GET_PROP(Real, "gamma", 2.2)), false);
  ShaderGlobal::set_int_fast(::get_shader_glob_var_id("tex_sRGB_mode", true), fabsf(GET_PROP(Real, "gamma", 2.2f) - 1.0f) > 1e-3f);

  if (GET_PROP(Int, "stubTexIdx", 0) >= 0)
  {
    int stub_idx = GET_PROP(Int, "stubTexIdx", 0);
    bool ext_tex = (curAsset->isVirtual() && !curAsset->getSrcFileName());
    PropPanel::ContainerPropertyControl *rgrp = igrp->createRadioGroup(ID_SHOW_TEXQ_GRP, "Show tex quality");
    bool valid_stub = curTqAsset != nullptr || stub_idx >= 0;

    if (curAsset && curUhqAsset == curAsset)
      showTexQ = ID_SHOW_TEXQ_UHQ;
    else
    {
      if (valid_stub && !ext_tex)
      {
        rgrp->createRadio(ID_SHOW_TEXQ_STUB, String(0, "stubTexIdx = %d", stub_idx));
        if (curTqAsset)
          rgrp->createRadio(ID_SHOW_TEXQ_THUMB, "Thumbnail");
      }
      rgrp->createRadio(ID_SHOW_TEXQ_BASE, "Base quality");
      rgrp->createRadio(ID_SHOW_TEXQ_HIGH, "High quality");
    }
    if (curUhqAsset)
      rgrp->createRadio(ID_SHOW_TEXQ_UHQ, "Ultra-high quality");
    if (showTexQ == ID_SHOW_TEXQ_UHQ && !curUhqAsset)
      showTexQ = ID_SHOW_TEXQ_HIGH;

    igrp->setInt(ID_SHOW_TEXQ_GRP, showTexQ);
    if (ext_tex)
      igrp->createStatic(0, String(0, "stubTexIdx = %d", stub_idx));
    else if (!valid_stub)
      igrp->createStatic(0, String(0, "Invalid stubTexIdx = %d", stub_idx));
  }
  // other props

  if (viewMipLevel > texture->level_count() - 1)
    viewMipLevel = -1;
  if (texture->level_count() > 1)
    panel.createTrackInt(ID_MIP_LEVEL, "Mipmap level:", viewMipLevel, -1, texture->level_count() - 1, 1);
  if (texture->restype() == RES3D_TEX)
  {
    if (aTestSVId != -1)
      panel.createTrackInt(ID_ALPHA_TEST, "Alpha test value:", viewAtestVal, 0, 255, 1);
    panel.createCheckBox(ID_STRETCH, "Stretch texture", forceStretch);
    panel.createCheckBox(ID_MIP_STRIPE, "Show MIP stripe", showMipStripe);
  }
  if (texture->restype() == RES3D_VOLTEX)
    panel.createTrackFloat(ID_TC_Z, "tc.Z:", tcZ, 0, 1, 0.01);

  if (iesTexture)
  {
    PropPanel::ContainerPropertyControl *pgrp = panel.createGroupBox(ID_IES_INFO_GRP, "Photometry info");
    pgrp->createStatic(0, "Blur radius:");
    pgrp->createStatic(0, String(32, "%3.4f", iesParams.blurRadius * RAD_TO_DEG), false);
    pgrp->createStatic(0, "Horizontal range:");
    pgrp->createStatic(0, String(32, "%4.1f:%4.1f", iesParams.phiMin * RAD_TO_DEG, iesParams.phiMax * RAD_TO_DEG), false);
    pgrp->createStatic(0, "Vertical range:");
    pgrp->createStatic(0, String(32, "%4.1f:%4.1f", iesParams.thetaMin * RAD_TO_DEG, iesParams.thetaMax * RAD_TO_DEG), false);
    pgrp->createStatic(0, "Edge blur:");
    if (iesParams.edgeFadeout < 0)
      pgrp->createStatic(0, "auto", false);
    else
      pgrp->createStatic(0, String(32, "%4.1f", iesParams.edgeFadeout * RAD_TO_DEG), false);

    pgrp->createSeparator(0);
    pgrp->createStatic(0, "Scaling:");
    pgrp->createStatic(0, String(32, "%f", iesScale), false);
    pgrp->createStatic(0, "Rotated:");
    pgrp->createStatic(0, iesRotated ? "yes" : "no", false);
    pgrp->createSeparator(0);
    pgrp->createStatic(0, "Optimal value");
    pgrp->createStatic(0, "Scaling:");
    pgrp->createStatic(0, String(32, "%f", iesOptimalScale), false);
    pgrp->createStatic(0, "Rotated:");
    pgrp->createStatic(0, iesOptimalRotated ? "yes" : "no", false);
    if (!iesValid)
    {
      pgrp->createStatic(0, "Invalid value!");
      pgrp->createStatic(0, "Reduce scaling or disable rotation");
    }
    else if (iesWastedArea >= 0.01)
    {
      pgrp->createStatic(0, "Wasted texture area");
      pgrp->createStatic(0, String(32, "%.1f%%", iesWastedArea * 100), false);
    }
  }

  // color mode
  if (colorModeSVId != -1)
  {
    int targetViewMode = viewRgbaMode;
    if (!hasAlpha && (targetViewMode == ID_COLOR_MODE_A || targetViewMode == ID_COLOR_MODE_RGBA))
      targetViewMode = ID_COLOR_MODE_RGB;

    PropPanel::ContainerPropertyControl *rgrp = panel.createRadioGroup(ID_COLOR_MODE_GRP, "Color channels");
    switch (chanelsRGB)
    {
      case 1:
        rgrp->createRadio(ID_COLOR_MODE_RGB, "R");
        targetViewMode = ID_COLOR_MODE_R;
        break;
      case 2:
        rgrp->createRadio(ID_COLOR_MODE_R, "R");
        rgrp->createRadio(ID_COLOR_MODE_G, "G");
        rgrp->createRadio(ID_COLOR_MODE_RGB, "RG");
        if (targetViewMode != ID_COLOR_MODE_R && targetViewMode != ID_COLOR_MODE_G && targetViewMode != ID_COLOR_MODE_RGB)
          targetViewMode = ID_COLOR_MODE_RGB;
        break;
      default:
        rgrp->createRadio(ID_COLOR_MODE_R, "R");
        rgrp->createRadio(ID_COLOR_MODE_G, "G");
        rgrp->createRadio(ID_COLOR_MODE_B, "B");
        rgrp->createRadio(ID_COLOR_MODE_RGB, "RGB");
    }
    if (hasAlpha)
    {
      rgrp->createRadio(ID_COLOR_MODE_A, "A");
      rgrp->createRadio(ID_COLOR_MODE_RGBA, "RGBA");
    }
    panel.setInt(ID_COLOR_MODE_GRP, targetViewMode);
    // rgrp->createRadio(ID_COLOR_MODE_TOO_DARK, "Check \"too dark\"");
    // rgrp->createRadio(ID_COLOR_MODE_TOO_BRIGHT, "Check \"too bright\"");
    panel.createTrackFloat(ID_COLOR_MODE_SCALE, "Color scale, dB", cScaleDb, -10, texture->restype() == RES3D_TEX ? 0 : 20, 0);
  }

  if (curAsset->props.getBool("convert", false) || (curAsset->isVirtual() && !curAsset->getSrcFileName()))
  {
    panel.createSeparator(0);
    panel.createButton(ID_EXPORT_TEXTURE, "Export to DDS...");
  }

  onChange(ID_COLOR_MODE_GRP, &panel);
  onChange(ID_MIP_LEVEL, &panel);
  onChange(ID_ALPHA_TEST, &panel);
  if (texId)
  {
    TexQL curQL = get_managed_res_cur_tql(texId);
    if (curQL != ((showTexQ == ID_SHOW_TEXQ_STUB) ? TQL_stub : TexQL(showTexQ - ID_SHOW_TEXQ_THUMB + TQL_thumb)))
      onChange(ID_SHOW_TEXQ_GRP, &panel);
  }
}


//=============================================================================
void TexturesPlugin::onChange(int pid, PropPanel::ContainerPropertyControl *panel)
{
  switch (pid)
  {
    case ID_TEX_TILE:
      viewTile = panel->getPoint2(pid);
      EDITORCORE->invalidateViewportCache();
      break;

    case ID_COLOR_MODE_GRP:
    {
      viewRgbaMode = panel->getInt(ID_COLOR_MODE_GRP);

      switch (viewRgbaMode)
      {
        case ID_COLOR_MODE_R: mColor = E3DCOLOR(255, 0, 0, 255); break;

        case ID_COLOR_MODE_G: mColor = E3DCOLOR(0, 255, 0, 255); break;

        case ID_COLOR_MODE_B: mColor = E3DCOLOR(0, 0, 255, 255); break;

        case ID_COLOR_MODE_RGB:
        case ID_COLOR_MODE_A:
        case ID_COLOR_MODE_RGBA:
        case ID_COLOR_MODE_TOO_DARK:
        case ID_COLOR_MODE_TOO_BRIGHT: mColor = E3DCOLOR(255, 255, 255, 255); break;
      }
      switch (viewRgbaMode)
      {
        case ID_COLOR_MODE_R:
        case ID_COLOR_MODE_G:
        case ID_COLOR_MODE_B:
        case ID_COLOR_MODE_RGB:
          cMul = color4(mColor);
          cAdd.set(0, 0, 0, 1);
          break;
        case ID_COLOR_MODE_RGBA:
          cMul = color4(mColor);
          cAdd.set(0, 0, 0, 0);
          break;
        case ID_COLOR_MODE_A:
          cMul.set(0, 0, 0, 0);
          cAdd.set(1, 1, 1, 1);
          break;
      }

      if (colorModeSVId != -1)
        switch (viewRgbaMode)
        {
          case ID_COLOR_MODE_R:
          case ID_COLOR_MODE_G:
          case ID_COLOR_MODE_B: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_RGB); break;
          case ID_COLOR_MODE_RGBA: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_RGBA); break;

          case ID_COLOR_MODE_RGB: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_RGB); break;

          case ID_COLOR_MODE_A: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_A); break;

          case ID_COLOR_MODE_TOO_DARK: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_TOO_DARK); break;

          case ID_COLOR_MODE_TOO_BRIGHT: ShaderGlobal::set_int_fast(colorModeSVId, TEX_COLOL_MODE_TOO_BRIGHT); break;
        }
    }
      EDITORCORE->invalidateViewportCache();
      break;

    case ID_COLOR_MODE_SCALE:
      cScaleDb = panel->getFloat(pid);
      EDITORCORE->invalidateViewportCache();
      break;

    case ID_MIP_LEVEL:
      viewMipLevel = panel->getInt(ID_MIP_LEVEL);
      if (texture)
        texture->texmiplevel(viewMipLevel, viewMipLevel < 0 ? 0 : viewMipLevel);
      break;

    case ID_STRETCH:
      forceStretch = panel->getBool(ID_STRETCH);
      texMove = Point2(0, 0);
      break;

    case ID_MIP_STRIPE:
      showMipStripe = panel->getBool(pid);
      texMove = Point2(0, 0);
      break;

    case ID_ALPHA_TEST:
      viewAtestVal = panel->getInt(ID_ALPHA_TEST);
      if (aTestSVId != -1)
        ShaderGlobal::set_int_fast(aTestSVId, viewAtestVal);
      break;

    case ID_TC_Z: tcZ = panel->getFloat(pid); break;

    case ID_SHOW_TEXQ_GRP:
      showTexQ = panel->getInt(pid);
      if (texture)
      {
        if (showTexQ == ID_SHOW_TEXQ_STUB || (showTexQ == ID_SHOW_TEXQ_THUMB && !curTqAsset))
        {
          if (showTexQ == ID_SHOW_TEXQ_STUB)
            texture->discardTex();
          panel->setCaption(ID_TEX_SIZE_LABEL, "STUB");
        }
        else
        {
          texmgr_internal::max_allowed_q =
            (showTexQ == ID_SHOW_TEXQ_STUB) ? TQL_stub : TexQL(showTexQ - ID_SHOW_TEXQ_THUMB + TQL_thumb);
          ::release_managed_tex(texId);
          discard_unused_managed_texture(texId);

          texId = ::get_managed_texture_id(String(64, "%s*", curAsset->getName()));
          texture = ::acquire_managed_tex(texId);
          if (texture)
          {
            if (get_managed_texture_refcount(texId) > 1)
              dxp_factory_reload_tex(texId, texmgr_internal::max_allowed_q);
            ddsx::tex_pack2_perform_delayed_data_loading();
            String _tex_size;
            TextureInfo ti;
            texture->getinfo(ti);
            switch (texture->restype())
            {
              case RES3D_TEX: _tex_size = String(32, "%d x %d", ti.w, ti.h); break;
              case RES3D_CUBETEX: _tex_size = String(32, "%d", ti.w); break;
              case RES3D_VOLTEX: _tex_size = String(32, "%d x %d x %d", ti.w, ti.h, ti.d); break;
            }
            panel->setCaption(ID_TEX_SIZE_LABEL, _tex_size);
          }
          else
            texId = BAD_TEXTUREID;
        }

        if (texture)
        {
          if (viewMipLevel > texture->level_count() - 1)
            viewMipLevel = -1;
          if (texture->level_count() > 1)
          {
            panel->setMinMaxStep(ID_MIP_LEVEL, -1, texture->level_count() - 1, 1);
            panel->setInt(ID_MIP_LEVEL, viewMipLevel);
          }
          texture->texmiplevel(viewMipLevel, viewMipLevel < 0 ? 0 : viewMipLevel);
          panel->setCaption(ID_TEX_MIPS_LABEL, String(32, "%d", texture->level_count()));
          panel->setCaption(ID_TEX_MEMSZ_LABEL, String(32, "%dK", tql::sizeInKb(texture->ressize())));
        }
      }
      break;
  }
}

static bool get_ddsx1(DagorAsset &a, ddsx::DDSxDataPublicHdr &hdr, Tab<char> &data)
{
  if (a.props.getBool("convert", false))
  {
    if (a.props.getBool("rtMipGen", false))
      if (DagorAsset *hq_a = a.getMgr().findAsset(String(0, "%s$hq", a.getName()), a.getMgr().getTexAssetTypeId()))
        if (get_ddsx1(*hq_a, hdr, data) && hdr.w)
          return true;

    ddsx::Buffer b;
    if (texconvcache::get_tex_asset_built_ddsx(a, b, _MAKE4C('PC'), NULL, NULL))
    {
      ddsx::Header &dh = *(ddsx::Header *)b.ptr;

      data.resize(dh.memSz);
      InPlaceMemLoadCB fcrd((char *)b.ptr + sizeof(ddsx::Header), b.len - sizeof(ddsx::Header));
      if (dh.isCompressionZSTD())
      {
        ZstdLoadCB crd(fcrd, dh.packedSz);
        crd.read(data.data(), data_size(data));
        crd.ceaseReading();
      }
      else if (dh.isCompressionOODLE())
      {
        OodleLoadCB crd(fcrd, dh.packedSz, dh.memSz);
        crd.read(data.data(), data_size(data));
        crd.ceaseReading();
      }
      else if (dh.isCompressionZLIB())
      {
        ZlibLoadCB crd(fcrd, dh.packedSz);
        crd.read(data.data(), data_size(data));
        crd.ceaseReading();
      }
      else if (dh.isCompression7ZIP())
      {
        LzmaLoadCB crd(fcrd, dh.packedSz);
        crd.read(data.data(), data_size(data));
        crd.ceaseReading();
      }
      else
        fcrd.read(data.data(), data_size(data));

      ddsx_forward_mips_inplace(dh, data.data(), data_size(data));
#define COPY_FIELD(X) hdr.X = dh.X
      COPY_FIELD(d3dFormat);
      COPY_FIELD(flags);
      COPY_FIELD(w);
      COPY_FIELD(h);
      COPY_FIELD(depth);
      COPY_FIELD(levels);
      COPY_FIELD(bitsPerPixel);
      COPY_FIELD(hqPartLevels);
      COPY_FIELD(dxtShift);
      COPY_FIELD(lQmip);
      COPY_FIELD(mQmip);
      COPY_FIELD(uQmip);
#undef COPY_FIELD
      b.free();
      return true;
    }
    wingw::message_box(wingw::MBS_EXCL | wingw::MBS_OK, "Cannot convert tex", "texconvcache::get_tex_asset_built_ddsx(%s) failed",
      a.getName());
  }
  else if (ddsx::read_ddsx_contents(String(0, "%s*", a.getName()), data, hdr))
    return true;
  else
    wingw::message_box(wingw::MBS_EXCL | wingw::MBS_OK, "Cannot read tex", "ddsx::read_ddsx_contents(%s*) failed", a.getName());
  return false;
}
static bool get_ddsx(DagorAsset &a, ddsx::DDSxDataPublicHdr &hdr, Tab<char> &data)
{
  bool cvt = a.props.getBool("convert", false);
  TextureMetaData tmd;
  tmd.read(a.props, "PC");

  if (!cvt)
  {
    TEXTUREID id = get_managed_texture_id(String(0, "%s*", a.getName()));
    tmd.decode(get_managed_texture_name(id));
  }
  if (tmd.baseTexName.empty())
    return get_ddsx1(a, hdr, data);

  DagorAsset *base_a = a.getMgr().findAsset(tmd.baseTexName, a.getMgr().getTexAssetTypeId());
  if (!base_a)
  {
    wingw::message_box(wingw::MBS_EXCL | wingw::MBS_OK, "Cannot find base tex", "baseTex=%s (required for %s) not found",
      tmd.baseTexName, a.getName());
    return false;
  }

  if (!get_ddsx(*base_a, hdr, data))
    return false;

  ddsx::DDSxDataPublicHdr diff_hdr;
  Tab<char> diff_data;
  if (!get_ddsx1(a, diff_hdr, diff_data))
    return false;

  // patch base texture with difference to build final DXT* data
  int kaizer_hdr_sz = 3 * sizeof(float);
  InPlaceMemLoadCB crd_diff(diff_data.data() + kaizer_hdr_sz, data_size(diff_data) - kaizer_hdr_sz);
  InPlaceMemLoadCB crd_base(data.data(), data_size(data));

  struct Dxt1ColorBlock
  {
    unsigned c0r : 5, c0g : 6, c0b : 5;
    unsigned c1r : 5, c1g : 6, c1b : 5;
    unsigned idx;
  };
  struct Dxt5AlphaBlock
  {
    unsigned char a0, a1;
    unsigned short idx[3];
  };

  int blk_total = (diff_hdr.w / 4) * (diff_hdr.h / 4);
  int blk_empty = crd_diff.readInt();
  Tab<unsigned> map(tmpmem);
  map.resize((blk_total + 31) / 32);
  if (blk_empty == 0)
    mem_set_ff(map);
  else if (blk_empty == blk_total)
    mem_set_0(map);
  else
    crd_diff.read(map.data(), data_size(map));

  int dxt_blk_sz = (diff_hdr.d3dFormat == _MAKE4C('DXT1')) ? 8 : 16;
  if (hdr.d3dFormat == diff_hdr.d3dFormat)
    data.resize(blk_total * dxt_blk_sz);
  else if (hdr.d3dFormat == _MAKE4C('DXT1'))
  {
    Tab<char> dxtData;
    dxtData.resize(blk_total * dxt_blk_sz);
    for (int i = 0; i < blk_total; i++)
    {
      memset(&dxtData[i * 16], 0xFF, 2);
      memset(&dxtData[i * 16 + 2], 0, 6);
      memcpy(&dxtData[i * 16 + 8], &data[i * 8], 8);
    }
    data = eastl::move(dxtData);
  }
  else if (hdr.d3dFormat == _MAKE4C('DXT5'))
  {
    for (int i = 0; i < blk_total; i++)
      memcpy(&data[i * 8], &data[i * 16 + 8], 8);
    data.resize(blk_total * dxt_blk_sz);
  }

  if (diff_hdr.d3dFormat == _MAKE4C('DXT5'))
    for (int i = 0; i < map.size(); i++)
    {
      if (!map[i])
        continue;
      for (unsigned j = 0, w = map[i]; w; j++, w >>= 1)
      {
        if (!(w & 1))
          continue;

        Dxt5AlphaBlock pa, &ba = *(Dxt5AlphaBlock *)(&data[(i * 32 + j) * dxt_blk_sz]);
        crd_diff.read(&pa, sizeof(pa));

        if (hdr.d3dFormat == _MAKE4C('DXT5'))
        {
          pa.a0 += ba.a0;
          pa.a1 += ba.a1;
          pa.idx[0] = ba.idx[0] ^ pa.idx[0];
          pa.idx[1] = ba.idx[1] ^ pa.idx[1];
          pa.idx[2] = ba.idx[2] ^ pa.idx[2];
        }
        memcpy(&ba, &pa, sizeof(pa));
      }
    }
  for (int i = 0; i < map.size(); i++)
  {
    if (!map[i])
      continue;
    for (unsigned j = 0, w = map[i]; w; j++, w >>= 1)
    {
      if (!(w & 1))
        continue;

      Dxt1ColorBlock pc, &bc = *(Dxt1ColorBlock *)(&data[(i * 32 + j) * dxt_blk_sz + (diff_hdr.d3dFormat == _MAKE4C('DXT5') ? 8 : 0)]);
      crd_diff.read(&pc, sizeof(pc));

      pc.c0r += bc.c0r;
      pc.c0g += bc.c0g;
      pc.c0b += bc.c0b;
      pc.c1r += bc.c1r;
      pc.c1g += bc.c1g;
      pc.c1b += bc.c1b;
      pc.idx = bc.idx ^ pc.idx;
      memcpy(&bc, &pc, sizeof(pc));
    }
  }

  hdr.d3dFormat = diff_hdr.d3dFormat;
  hdr.levels = 1;
  return true;
}

void export_texture_as_dds(const char *fn, DagorAsset &a)
{
  struct DdsBitMaskFormat
  {
    uint32_t bitCount, alphaMask, redMask, greenMask, blueMask, format, flags;
  };
  static const DdsBitMaskFormat bitMaskFormat[] = {
    {24, 0x00000000, 0xff0000, 0xff00, 0xff, D3DFMT_R8G8B8, DDPF_RGB},
    {32, 0xff000000, 0xff0000, 0xff00, 0xff, D3DFMT_A8R8G8B8, DDPF_RGB | DDPF_ALPHAPIXELS},
    {32, 0x00000000, 0xff0000, 0xff00, 0xff, D3DFMT_X8R8G8B8, DDPF_RGB},
    {16, 0x00000000, 0x00f800, 0x07e0, 0x1f, D3DFMT_R5G6B5, DDPF_RGB},
    {16, 0x00008000, 0x007c00, 0x03e0, 0x1f, D3DFMT_A1R5G5B5, DDPF_RGB | DDPF_ALPHAPIXELS},
    {16, 0x00000000, 0x007c00, 0x03e0, 0x1f, D3DFMT_X1R5G5B5, DDPF_RGB},
    {16, 0x0000f000, 0x000f00, 0x00f0, 0x0f, D3DFMT_A4R4G4B4, DDPF_RGB | DDPF_ALPHAPIXELS},
    {8, 0x00000000, 0x0000e0, 0x001c, 0x03, D3DFMT_R3G3B2, DDPF_RGB},
    {8, 0x000000ff, 0x000000, 0x0000, 0x00, D3DFMT_A8, DDPF_ALPHA | DDPF_ALPHAPIXELS},
    {8, 0x00000000, 0x0000FF, 0x0000, 0x00, D3DFMT_L8, DDPF_LUMINANCE},
    {16, 0x0000ff00, 0x0000e0, 0x001c, 0x03, D3DFMT_A8R3G3B2, DDPF_RGB | DDPF_ALPHAPIXELS},
    {16, 0x00000000, 0x000f00, 0x00f0, 0x0f, D3DFMT_X4R4G4B4, DDPF_RGB},
    {32, 0xff000000, 0xff0000, 0xff00, 0xff, D3DFMT_A8B8G8R8, DDPF_RGB | DDPF_ALPHAPIXELS},
    {32, 0x00000000, 0xff0000, 0xff00, 0xff, D3DFMT_X8B8G8R8, DDPF_RGB},
    {16, 0x00000000, 0x0000ff, 0xff00, 0x00, D3DFMT_V8U8, DDPF_BUMPDUDV},
    {16, 0x0000ff00, 0x0000ff, 0x0000, 0x00, D3DFMT_A8L8, DDPF_LUMINANCE | DDPF_ALPHAPIXELS},
    {16, 0x00000000, 0xFFFF, 0x00000000, 0, D3DFMT_L16, DDPF_LUMINANCE},
  };

  FullFileSaveCB cwr(fn);
  if (!cwr.fileHandle)
  {
    wingw::message_box(wingw::MBS_EXCL | wingw::MBS_OK, "Cannot open file for write", "Faled to write file:\n%s", fn);
    return;
  }

  ddsx::DDSxDataPublicHdr hdr;
  Tab<char> data;
  if (!get_ddsx(a, hdr, data))
  {
    cwr.close();
    dd_erase(fn);
    return;
  }
  DagorAsset *hq_a = a.getMgr().findAsset(String(0, "%s$uhq", a.getName()), a.getMgr().getTexAssetTypeId());
  if (!hq_a)
    hq_a = a.getMgr().findAsset(String(0, "%s$hq", a.getName()), a.getMgr().getTexAssetTypeId());
  if (hq_a)
  {
    ddsx::DDSxDataPublicHdr hdrHq;
    Tab<char> dataHq;
    if (hq_a->props.getBool("rtMipGen", false))
      ;
    else if (get_ddsx(*hq_a, hdrHq, dataHq))
    {
      if (!(hdrHq.flags & (hdr.FLG_GENMIP_BOX | hdr.FLG_GENMIP_KAIZER)))
      {
        append_items(dataHq, data.size(), data.data());
        hdrHq.levels += hdr.levels;
      }
      data = eastl::move(dataHq);
      hdr = hdrHq;
    }
    else
    {
      cwr.close();
      dd_erase(fn);
      return;
    }
  }

  // recode to DDS and save
  DDSURFACEDESC2 targetHeader;
  memset(&targetHeader, 0, sizeof(DDSURFACEDESC2));
  targetHeader.dwSize = sizeof(DDSURFACEDESC2);
  targetHeader.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT;
  targetHeader.dwHeight = hdr.h;
  targetHeader.dwWidth = hdr.w;
  targetHeader.dwDepth = hdr.depth;
  targetHeader.dwMipMapCount = hdr.levels;
  targetHeader.ddsCaps.dwCaps = 0;
  targetHeader.ddsCaps.dwCaps2 = 0;

  if (hdr.flags & hdr.FLG_CUBTEX)
    targetHeader.ddsCaps.dwCaps2 |= DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES;
  else if (hdr.flags & hdr.FLG_VOLTEX)
  {
    targetHeader.dwFlags |= DDSD_DEPTH;
    targetHeader.ddsCaps.dwCaps2 |= DDSCAPS2_VOLUME;
  }
  else
    targetHeader.ddsCaps.dwCaps |= DDSCAPS_TEXTURE;

  if (hdr.flags & (hdr.FLG_GENMIP_BOX | hdr.FLG_GENMIP_KAIZER))
    targetHeader.dwMipMapCount = 1;

  DDPIXELFORMAT &pf = targetHeader.ddpfPixelFormat;
  pf.dwSize = sizeof(DDPIXELFORMAT);
  pf.dwFlags = DDPF_FOURCC;
  pf.dwFourCC = hdr.d3dFormat;

  // pixel format search
  for (int i = 0; i < sizeof(bitMaskFormat) / sizeof(bitMaskFormat[0]); i++)
    if (hdr.d3dFormat == bitMaskFormat[i].format)
    {
      pf.dwFourCC = 0;
      pf.dwFlags = bitMaskFormat[i].flags;
      pf.dwRGBBitCount = bitMaskFormat[i].bitCount;
      pf.dwRBitMask = bitMaskFormat[i].redMask;
      pf.dwGBitMask = bitMaskFormat[i].greenMask;
      pf.dwBBitMask = bitMaskFormat[i].blueMask;
      pf.dwRGBAlphaBitMask = bitMaskFormat[i].alphaMask;
      break;
    }

  // dds output
  uint32_t FourCC = MAKEFOURCC('D', 'D', 'S', ' ');
  cwr.write(&FourCC, sizeof(FourCC));
  cwr.write(&targetHeader, sizeof(targetHeader));
  cwr.write(data.data(), data_size(data));
}

void TexturesPlugin::onClick(int pid, PropPanel::ContainerPropertyControl *panel)
{
  if (pid == ID_EXPORT_TEXTURE)
  {
    String fn(0, "%s/%s.dds", ::get_app().getWorkspace().getSdkDir(), curAsset->getName());
    dd_simplify_fname_c(fn);
    fn = wingw::file_save_dlg(EDITORCORE->getWndManager()->getMainWindow(), "Export texture to DDS", "DDS format|*.dds|All files|*.*",
      "dds", ::get_app().getWorkspace().getSdkDir(), fn);

    if (!fn.empty())
      export_texture_as_dds(fn, *curAsset);
  }
}

//=============================================================================

bool TexturesPlugin::handleMouseMove(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif)
{
  if (isMoving)
  {
    texMove += Point2(x, y) - priorMousePos;
    priorMousePos = Point2(x, y);
  }
  return 0;
}


bool TexturesPlugin::handleMouseLBPress(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif)
{
  isMoving = true;
  priorMousePos = Point2(x, y);

  return 0;
}
void TexturesPlugin::handleKeyPress(IGenViewportWnd *wnd, int vk, int modif)
{
  if (vk == '0' && (modif & wingw::M_CTRL))
    texScale = 1.0f;
}
bool TexturesPlugin::handleMouseWheel(IGenViewportWnd *wnd, int wheel_d, int x, int y, int key_modif)
{
  if (wheel_d > 0)
    texScale = clamp(texScale * 1.25f, 0.125f, 16.0f);
  else
    texScale = clamp(texScale / 1.25f, 0.125f, 160.f);
  wnd->activate();
  return true;
}


bool TexturesPlugin::handleMouseLBRelease(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif)
{
  isMoving = false;

  return 0;
}
