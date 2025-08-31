// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <scriptPanelWrapper/spw_interface.h>
#include <propPanel/control/container.h>
#include <util/dag_simpleString.h>
#include <ioSys/dag_dataBlock.h>
#include <winGuiWrapper/wgw_timer.h>

class CSQPanelWrapper;
class DagorAsset;

class AVScriptPanelEditor : public IScriptPanelEventHandler, public IScriptPanelTargetCB, public ITimerCallBack
{
public:
  AVScriptPanelEditor(const char *script_path, const char *panel_caption);
  ~AVScriptPanelEditor() override;

  static void initVars();

  void createPanel(PropPanel::ContainerPropertyControl &panel);
  void destroyPanel();

  void load(DagorAsset *asset);
  bool scriptExists();

  // IScriptPanelEventHandler
  void onScriptPanelChange() override;

  // ITimerCallBack
  void update() override;

  // IScriptPanelTargetCB
  const char *getTarget(const char *old_choise, const char *type, const char filter[]) override;
  const char *validateTarget(const char *name, const char *type) override;

private:
  const char *selectAsset(const char *old_choise, dag::ConstSpan<int> masks);
  const char *selectGroupName(const char *old_choise);

  DataBlock propsBlk;
  CSQPanelWrapper *scriptPanelWrap;
  SimpleString scriptPath, panelCaption;
  DagorAsset *mAsset;
  WinTimer *updateTimer;
  bool needCallChange;
  int ignoreChangesCnt;
};
