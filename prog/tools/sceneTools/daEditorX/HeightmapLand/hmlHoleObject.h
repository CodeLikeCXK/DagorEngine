// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include "hmlPlugin.h"

#include <EditorCore/ec_ObjectEditor.h>


static constexpr unsigned CID_HmapLandHoleObject = 0xB00BA8AFu; // HmapLandHoleObject
static const int HMAP_TYPE_HOLE = 0;


class DataBlock;


class HmapLandHoleObject : public RenderableEditableObject
{
public:
  Point3 boxSize;

  HmapLandHoleObject();

  void update(real dt) override;
  void beforeRender() override;
  void render() override;
  void renderTrans() override;

  bool isSelectedByRectangle(IGenViewportWnd *vp, const EcRect &rect) const override;
  bool isSelectedByPointClick(IGenViewportWnd *vp, int x, int y) const override;
  bool getWorldBox(BBox3 &box) const override;

  void fillProps(PropPanel::ContainerPropertyControl &panel, DClassID for_class_id,
    dag::ConstSpan<RenderableEditableObject *> objects) override;

  void onPPChange(int pid, bool edit_finished, PropPanel::ContainerPropertyControl &panel,
    dag::ConstSpan<RenderableEditableObject *> objects) override;

  void rotateObject(const Point3 &delta, const Point3 &origin, IEditorCoreEngine::BasisType basis) override {}

  void scaleObject(const Point3 &delta, const Point3 &origin, IEditorCoreEngine::BasisType basis) override;

  void putRotateUndo() override {}

  void save(DataBlock &blk);
  void load(const DataBlock &blk);


  EO_IMPLEMENT_RTTI(CID_HmapLandHoleObject)


  void buildBoxEdges(IGenViewportWnd *vp, Point2 edges[12][2]) const;
  void setBoxSize(Point3 &s);
  Point3 getBoxSize() { return boxSize; }


  E3DCOLOR normal_color, selected_color;
};
