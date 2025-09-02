// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <ioSys/dag_genIo.h>

#include <propPanel/control/container.h>

#include <scriptHelpers/tunedParams.h>
#include <math/dag_Point2.h>

#include <debug/dag_debug.h>

using namespace ScriptHelpers;

namespace ScriptHelpers
{
static Tab<String> typeNames(tmpmem);

class TunedGradientBoxParam : public TunedElement
{
public:
  static const int MAX_POINTS = 64;

  int controlPid;
  Tab<PropPanel::GradientKey> values;
  PropPanel::ContainerPropertyControl *mPanel;

  TunedGradientBoxParam(const char *nm) : controlPid(-1), mPanel(NULL)
  {
    name = nm;
    values.push_back({0, 0});
    values.push_back({0, 0xffffffff});
  }

  void resetPropPanel() override { mPanel = nullptr; }
  void fillPropPanel(int &pid, PropPanel::ContainerPropertyControl &ppcb) override
  {
    mPanel = &ppcb;
    controlPid = ++pid;
    ppcb.createGradientBox(controlPid, getName());

    initCurveControl(ppcb);
  }

  void initCurveControl(PropPanel::ContainerPropertyControl &ppcb) { mPanel->setGradient(controlPid, &values); }

  void getControlPoints(PropPanel::PGradient points)
  {
    if ((controlPid > -1) && (mPanel))
      mPanel->getGradient(controlPid, points);
  }

  //---------------------------------------------

  TunedElement *cloneElem() override { return new TunedGradientBoxParam(*this); }

  int subElemCount() const override { return 0; }
  TunedElement *getSubElem(int index) const override { return NULL; }

  void getValues(int &pid, PropPanel::ContainerPropertyControl &panel) override
  {
    ++pid;
    Tab<PropPanel::GradientKey> pts(tmpmem);
    getControlPoints(&pts);
    values = pts;
  }

  void saveData(mkbindump::BinDumpSaveCB &cwr, SaveDataCB *save_cb) override
  {
    cwr.writeInt32e((uint32_t)values.size());
    for (size_t i = 0; i < values.size(); ++i)
    {
      cwr.writeReal(values[i].position);
      cwr.writeInt32e(values[i].color);
    }
  }

  void saveValues(DataBlock &blk, SaveValuesCB *save_cb) override
  {
    for (int i = 0; i < values.size(); ++i)
    {
      blk.setReal(String(32, "pos_%d", i), values[i].position);
      blk.setE3dcolor(String(32, "color_%d", i), values[i].color);
    }

    for (size_t i = values.size(); i < MAX_POINTS; ++i)
    {
      blk.removeParam(String(32, "pos_%d", i));
      blk.removeParam(String(32, "color_%d", i));
    }
  }

  void loadValues(const DataBlock &blk) override
  {
    clear_and_shrink(values);

    for (int i = 0; i < MAX_POINTS; ++i)
      if (blk.paramExists(String(32, "pos_%d", i)))
      {
        PropPanel::GradientKey v;
        v.position = blk.getReal(String(32, "pos_%d", i), 0);
        v.color = blk.getE3dcolor(String(32, "color_%d", i), 0xffffffff);
        v.position = clamp<float>(v.position, 0, 1);
        values.push_back(v);
      }
      else
        break;

    if (mPanel)
      initCurveControl(*mPanel);
  }
};

TunedElement *create_tuned_gradient_box(const char *name) { return new TunedGradientBoxParam(name); }
}; // namespace ScriptHelpers
