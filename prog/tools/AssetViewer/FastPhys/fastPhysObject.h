// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <propPanel/c_control_event_handler.h>

#include <EditorCore/ec_rendEdObject.h>

class FpdObject;
class FastPhysEditor;

//------------------------------------------------------------------

class IFPObject : public RenderableEditableObject, public PropPanel::ControlEventHandler
{
public:
  static constexpr unsigned HUID = 0x8DCB76A6u; // IFPObject

  static E3DCOLOR frozenColor;

public:
  IFPObject(FpdObject *obj, FastPhysEditor &editor);
  ~IFPObject() override;

  FpdObject *getObject() const { return mObject.get(); }
  FastPhysEditor *getObjEditor() const;

  static IFPObject *createFPObject(FpdObject *obj, FastPhysEditor &editor); // factory for wrapper object

  virtual void refillPanel(PropPanel::ContainerPropertyControl *panel) = 0;

  // RenderableEditableObject
  void update(real dt) override {}
  void beforeRender() override {}
  void renderTrans() override {}

  void render() override {}
  bool isSelectedByPointClick(IGenViewportWnd *vp, int x, int y) const override { return true; }
  bool isSelectedByRectangle(IGenViewportWnd *vp, const EcRect &rect) const override { return true; }
  bool getWorldBox(BBox3 &box) const override { return true; }
  void onPPChange(int pid, bool edit_finished, PropPanel::ContainerPropertyControl &panel,
    dag::ConstSpan<RenderableEditableObject *> objects) override {};

  EO_IMPLEMENT_RTTI(HUID)

protected:
  Ptr<FpdObject> mObject;
  FastPhysEditor &mFPEditor;
};


//------------------------------------------------------------------
