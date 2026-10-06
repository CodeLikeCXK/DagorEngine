// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <render/antiAliasing_legacy.h>
#include <render/daFrameGraph/nodeHandle.h>

class SGSR : public AntiAliasing
{
public:
  SGSR(const IPoint2 &outputResolution);

  bool needMotionVectors() const override { return false; }
  float getLodBias() const override { return lodBias; }
  bool isAvailable() const override { return available; }

private:
  bool available;
  dafg::NodeHandle applierNode;
};
