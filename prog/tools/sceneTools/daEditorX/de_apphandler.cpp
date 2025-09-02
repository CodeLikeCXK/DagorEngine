// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "de_apphandler.h"
#include "de_plugindata.h"
#include <oldEditor/de_cm.h>
#include <EditorCore/ec_wndGlobal.h>
#include <libTools/util/strUtil.h>
#include <assets/texAssetBuilderTextureFactory.h>

#include <gui/dag_stdGuiRenderEx.h>
#include <3d/dag_render.h>
#include <render/dag_cur_view.h>
#include <math/dag_mathAng.h>
#include <perfMon/dag_perfTimer.h>

#include <debug/dag_debug.h>

#include <stdio.h>

static TEXTUREID compass_tid = BAD_TEXTUREID, compass_nesw_tid = BAD_TEXTUREID;
static d3d::SamplerHandle compass_sampler = d3d::INVALID_SAMPLER_HANDLE;
static d3d::SamplerHandle compass_nesw_sampler = d3d::INVALID_SAMPLER_HANDLE;

static Point2 getCameraAngles()
{
  Point2 ang = dir_to_angles(::grs_cur_view.itm.getcol(2));
  if (ang.y > DegToRad(85))
    ang.x = dir_to_angles(-::grs_cur_view.itm.getcol(1)).x;
  else if (ang.y < -DegToRad(85))
    ang.x = dir_to_angles(::grs_cur_view.itm.getcol(1)).x;
  return ang;
}

DagorEdAppEventHandler::DagorEdAppEventHandler(GenericEditorAppWindow &m) :
  GenericEditorAppWindow::AppEventHandler(m), showCompass(false)
{}
DagorEdAppEventHandler::~DagorEdAppEventHandler()
{
  release_managed_tex(compass_tid);
  release_managed_tex(compass_nesw_tid);
}

//==============================================================================
void DagorEdAppEventHandler::handleViewportPaint(IGenViewportWnd *wnd)
{
  using hdpi::_pxS;

  AsyncTextureLoadingState ld_state;
  static int64_t ld_reft = 0;
  if (texconvcache::get_tex_factory_current_loading_state(ld_state) && //
      (ld_state.pendingLdTotal > 0 || (ld_reft && get_time_usec(ld_reft) < 3000000)))
  {
    using hdpi::_px;
    hdpi::Px rx, ry;
    static_cast<ViewportWindow *>(wnd)->getMenuAreaSize(rx, ry);

    String text;
    if (ld_state.pendingLdTotal > 0)
    {
      const unsigned *c = ld_state.pendingLdCount;
      text.printf(0, "Loading textures: %d pending [%d %s], %d tex loaded, %d tex updated", //
        ld_state.pendingLdTotal, c[0] ? c[0] : (c[1] ? c[1] : (c[2] ? c[2] : c[3])),
        c[0] ? "TQ" : (c[1] ? "BQ" : (c[2] ? "HQ" : "UHQ")), ld_state.totalLoadedCount, ld_state.totalCacheUpdated);
      ld_reft = ref_time_ticks();
    }
    else
      text.printf(0, "All textures loaded: %d tex (%dK read), updated %d tex", //
        ld_state.totalLoadedCount, ld_state.totalLoadedSizeKb, ld_state.totalCacheUpdated);

    StdGuiRender::set_font(0);
    Point2 ts = StdGuiRender::get_str_bbox(text).size();

    StdGuiRender::set_color(ld_state.pendingLdTotal > 0 ? COLOR_RED : COLOR_GREEN);
    StdGuiRender::render_box(_px(rx), 0, _px(rx) + ts.x + _pxS(6), _px(ry));

    StdGuiRender::set_color(COLOR_WHITE);
    StdGuiRender::draw_strf_to(_px(rx) + _pxS(3), _pxS(17), text);
  }

  if (compass_tid == BAD_TEXTUREID)
  {
    compass_tid = add_managed_texture(::make_full_path(sgg::get_exe_path_full(), "../commonData/tex/compass.tga"));
    compass_nesw_tid = add_managed_texture(::make_full_path(sgg::get_exe_path_full(), "../commonData/tex/compass_nesw.tga"));
    acquire_managed_tex(compass_tid);
    acquire_managed_tex(compass_nesw_tid);
    compass_sampler = get_texture_separate_sampler(compass_tid);
    compass_nesw_sampler = get_texture_separate_sampler(compass_nesw_tid);
  }

  GenericEditorAppWindow::AppEventHandler::handleViewportPaint(wnd);


  IGenEditorPlugin *plug = DAGORED2->curPlugin();
  int vpW, vpH;
  wnd->getViewportSize(vpW, vpH);

  if (plug && !plug->getVisible())
  {
    char mess1[100];
    _snprintf(mess1, 99, " Caution: \"%s\" visibility is off ", plug->getMenuCommandName());
    char mess2[100] = " Press F12 key to change plugin visibility ";

    StdGuiRender::set_font(0);
    Point2 ts1 = StdGuiRender::get_str_bbox(mess1).size();
    Point2 ts2 = StdGuiRender::get_str_bbox(mess2).size();
    int ascent = StdGuiRender::get_font_ascent();
    int descent = StdGuiRender::get_font_descent();
    int fontHeight = ascent + descent;
    int padding = 2;
    int width = ts1.x > ts2.x ? ts1.x : ts2.x;
    int height = padding + fontHeight + padding + fontHeight + padding;
    int left = vpW / 2 - width / 2;
    int top = vpH / 2 - height / 2;

    StdGuiRender::set_color(COLOR_BLACK);
    StdGuiRender::render_box(left, top, left + width, top + height);

    StdGuiRender::set_color(COLOR_WHITE);
    StdGuiRender::draw_strf_to(left + (width - ts1.x) / 2, top + padding + ascent, mess1);
    StdGuiRender::draw_strf_to(left + (width - ts2.x) / 2, top + padding + fontHeight + padding + ascent, mess2);
  }
  IEditorCoreEngine::get()->showMessageAt();
  if (showCompass)
  {
    Point3 toNorth = ::grs_cur_view.pos + Point3(0, 0, 50);
    Point2 cp;

    if (!wnd->isOrthogonal() && wnd->worldToClient(toNorth, cp))
    {
      // show NORTH marker
      StdGuiRender::set_color(COLOR_WHITE);
      StdGuiRender::draw_line(cp.x - 20, cp.y - 20, cp.x + 20, cp.y + 20);
      StdGuiRender::draw_line(cp.x + 20, cp.y - 20, cp.x - 20, cp.y + 20);

      cp += Point2(2, 1);
      StdGuiRender::set_color(COLOR_BLACK);
      StdGuiRender::draw_line(cp.x - 20, cp.y - 20, cp.x + 20, cp.y + 20);
      cp.y -= 1;
      StdGuiRender::draw_line(cp.x + 20, cp.y - 20, cp.x - 20, cp.y + 20);
    }

    if (!wnd->isOrthogonal())
    {
      // show CENTER-of-SCREEN marker
      cp.set(vpW / 2, vpH / 2);
      StdGuiRender::set_color(COLOR_BLACK);
      StdGuiRender::draw_line(cp.x + 1 - 6, cp.y + 1, cp.x + 1 + 5, cp.y + 1);
      StdGuiRender::draw_line(cp.x + 1, cp.y + 1 - 6, cp.x + 1, cp.y + 1 + 5);

      StdGuiRender::set_color(COLOR_WHITE);
      StdGuiRender::draw_line(cp.x - 6, cp.y, cp.x + 5, cp.y);
      StdGuiRender::draw_line(cp.x, cp.y - 6, cp.x, cp.y + 5);
    }


    // render compass widget
    Point2 ang = getCameraAngles();
    float alpha = -ang.x - HALFPI;
    Point2 c(vpW - _pxS(132) + _pxS(64), vpH - _pxS(132) + _pxS(64)), c2, ts;
    float r = _pxS(64);
    float sa, ca;
    sincos(alpha, sa, ca);

    //  rotatable compass core
    StdGuiRender::set_color(COLOR_WHITE);
    StdGuiRender::set_texture(compass_tid, compass_sampler);
    StdGuiRender::set_alpha_blend(NONPREMULTIPLIED);
    StdGuiRender::render_quad(c + r * Point2(+sa - ca, -ca - sa), c + r * Point2(-sa - ca, +ca - sa),
      c + r * Point2(-sa + ca, +ca + sa), c + r * Point2(+sa + ca, -ca + sa));

    StdGuiRender::set_texture(compass_nesw_tid, compass_nesw_sampler);
    StdGuiRender::set_alpha_blend(NONPREMULTIPLIED);

    //  world directions
    r = _pxS(54);
    int bx = _pxS(10);
    sincos(-alpha + PI, sa, ca);
    c2 = c + Point2(r * sa, r * ca);
    StdGuiRender::render_rect(c2.x - bx, c2.y - bx, c2.x + bx, c2.y + bx, Point2(0.00, 0), Point2(0.25, 0), Point2(0, 1)); // North
    sincos(-alpha + HALFPI, sa, ca);
    c2 = c + Point2(r * sa, r * ca);
    StdGuiRender::render_rect(c2.x - bx, c2.y - bx, c2.x + bx, c2.y + bx, Point2(0.25, 0), Point2(0.25, 0), Point2(0, 1)); // East
    sincos(-alpha, sa, ca);
    c2 = c + Point2(r * sa, r * ca);
    StdGuiRender::render_rect(c2.x - bx, c2.y - bx, c2.x + bx, c2.y + bx, Point2(0.50, 0), Point2(0.25, 0), Point2(0, 1)); // South
    sincos(-alpha - HALFPI, sa, ca);
    c2 = c + Point2(r * sa, r * ca);
    StdGuiRender::render_rect(c2.x - bx, c2.y - bx, c2.x + bx, c2.y + bx, Point2(0.75, 0), Point2(0.25, 0), Point2(0, 1)); // West

    StdGuiRender::reset_textures();

    // render course and pitch
    StdGuiRender::set_font(0);
    String strPsi(0, "%.1f°", fmodf(RadToDeg(ang.x + 5 * HALFPI), 360));
    String strTeta(0, "%.1f°", RadToDeg(ang.y));

    //  course with moving scale
    StdGuiRender::set_color(COLOR_YELLOW);
    StdGuiRender::render_box(c.x - _pxS(35), c.y - _pxS(115), c.x + _pxS(35), c.y - _pxS(92));
    StdGuiRender::set_color(COLOR_BLACK);
    for (float a = -90, n = fmodf(RadToDeg(ang.x + 5 * HALFPI), 360); a <= 360 + 90; a += 10)
    {
      float x = (a - n) * 1.0f;
      bool bold = fmodf(a, 90) == 0;
      if (fabsf(x) <= 35)
        StdGuiRender::draw_line(c.x + x, c.y - _pxS(115), c.x + x, c.y - _pxS(bold ? 107 : 111), _pxS(bold ? 2 : 1));
    }

    //  pitch with moving scale
    StdGuiRender::set_color(COLOR_GREEN);
    StdGuiRender::render_box(c.x - _pxS(35), c.y - _pxS(90), c.x + _pxS(35), c.y - _pxS(67));
    StdGuiRender::set_color(COLOR_WHITE);
    for (float a = -90, n = RadToDeg(ang.y); a <= 90; a += 10)
    {
      float y = (a - n) * 0.5f;
      bool bold = a == 0;
      if (fabsf(y) <= 11)
        StdGuiRender::draw_line(c.x - _pxS(35), c.y - _pxS(78) + y, c.x - _pxS(bold ? 20 : 28), c.y - _pxS(78) + y,
          _pxS(bold ? 2 : 1));
    }

    //  draw numerical course
    StdGuiRender::set_color(COLOR_BLACK);
    ts = StdGuiRender::get_str_bbox(strPsi).size();
    StdGuiRender::draw_strf_to(c.x + _pxS(30) - ts.x, c.y - _pxS(102) + ts.y / 2, strPsi);

    //  draw numerical pitch
    StdGuiRender::set_color(COLOR_WHITE);
    ts = StdGuiRender::get_str_bbox(strTeta).size();
    StdGuiRender::draw_strf_to(c.x + _pxS(30) - ts.x, c.y - _pxS(81) + ts.y / 2, strTeta);
  }
}

bool DagorEdAppEventHandler::handleMouseLBPress(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif)
{
  if (showCompass)
  {
    int vpW, vpH;
    wnd->getViewportSize(vpW, vpH);

    // handle click to world directions on compass to set proper camera
    Point2 ang = getCameraAngles();
    Point2 c(vpW - 132 + 64 - x, vpH - 132 + 64 - y), c2;
    for (int i = 0; i < 4; i++)
    {
      float r = 54, sa, ca;
      sincos(ang.x - HALFPI * (i + 1), sa, ca);
      if (lengthSq(c + Point2(r * sa, r * ca)) < 100)
      {
        ang.x = (i - 1) * HALFPI;
        wnd->setCameraDirection(angles_to_dir(ang), angles_to_dir(ang + Point2(0, HALFPI)));
        wnd->setCameraPos(::grs_cur_view.itm.getcol(3));
        return true;
      }
    }
  }

  return GenericEditorAppWindow::AppEventHandler::handleMouseLBPress(wnd, x, y, inside, buttons, key_modif);
}
