// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <EditorCore/ec_ViewportWindow.h>

class DagorEdViewportWindow : public ViewportWindow
{
public:
  DagorEdViewportWindow(TEcHandle parent, int left, int top, int w, int h);

private:
  virtual bool onDropFiles(const dag::Vector<String> &files) override;
  virtual bool canStartInteractionWithViewport() override;
};
