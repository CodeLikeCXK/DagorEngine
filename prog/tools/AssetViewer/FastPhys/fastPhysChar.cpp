// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <debug/dag_debug3d.h>
#include <animChar/dag_animCharacter2.h>
#include "fastPhysChar.h"

#include "fastPhysEd.h"
#include "fastPhysObject.h"
#include <de3_dynRenderService.h>
#include <render/dag_cur_view.h>


//------------------------------------------------------------------

real FastPhysCharRoot::sphereRadius = 0.20f;

E3DCOLOR FastPhysCharRoot ::normalColor(0, 0, 0);
E3DCOLOR FastPhysCharRoot ::selectedColor(255, 255, 255);


FastPhysCharRoot::FastPhysCharRoot(AnimV20::IAnimCharacter2 *ch, FastPhysEditor &editor) :
  character(ch), mFPEditor(editor), isCharVisible(true)
{
  setName("CharacterRoot");
  objEditor = &editor;
}


void FastPhysCharRoot::updateNodeTm()
{
  if (!character)
    return;

  character->setTm(getWtm());

  if (!((FastPhysEditor *)objEditor)->isSimulationActive)
  {
    character->recalcWtm();
    ((FastPhysEditor *)objEditor)->onCharWtmChanged();
  }
}


bool FastPhysCharRoot::setPos(const Point3 &p)
{
  RenderableEditableObject::setPos(p);
  updateNodeTm();
  return true;
}


void FastPhysCharRoot::setMatrix(const Matrix3 &tm)
{
  RenderableEditableObject::setMatrix(tm);
  updateNodeTm();
}

void FastPhysCharRoot::setWtm(const TMatrix &wtm)
{
  RenderableEditableObject::setWtm(wtm);
  updateNodeTm();
}

void FastPhysCharRoot::renderGeometry(int stage)
{
  if (!character || !isCharVisible)
    return;
  IDynRenderService *rs = EDITORCORE->queryEditorInterface<IDynRenderService>();
  if (!rs)
    return;
  switch (stage)
  {
    case IRenderingService::STG_BEFORE_RENDER: character->beforeRender(); break;

    case IRenderingService::STG_RENDER_DYNAMIC_OPAQUE:
    case IRenderingService::STG_RENDER_DYNAMIC_TRANS:
      rs->renderOneDynModelInstance(character->getSceneInstance(), IDynRenderService::Stage(stage));
      break;
  }
}

void FastPhysCharRoot::render()
{
  if (character && mFPEditor.isSkeletonVisible)
  {
    ::begin_draw_cached_debug_lines();
    ::draw_skeleton_tree(character->getNodeTree(), 0.01f);
    ::end_draw_cached_debug_lines();
  }

  ::set_cached_debug_lines_wtm(getWtm());

  E3DCOLOR color = normalColor;
  if (isFrozen())
    color = IFPObject::frozenColor;
  if (isSelected())
    color = selectedColor;

  ::draw_cached_debug_sphere(Point3(0, 0, 0), sphereRadius, color);
}


void FastPhysCharRoot::update(real dt)
{
  if (character)
    character->act(dt, ::grs_cur_view.pos);
}


bool FastPhysCharRoot::isSelectedByPointClick(IGenViewportWnd *vp, int x, int y) const
{
  return ::is_sphere_hit_by_point(getPos(), sphereRadius, vp, x, y);
}


bool FastPhysCharRoot::isSelectedByRectangle(IGenViewportWnd *vp, const EcRect &rect) const
{
  return ::is_sphere_hit_by_rect(getPos(), sphereRadius, vp, rect);
}


bool FastPhysCharRoot::getWorldBox(BBox3 &box) const
{
  box.makecube(getPos(), sphereRadius * 2);
  return true;
}
