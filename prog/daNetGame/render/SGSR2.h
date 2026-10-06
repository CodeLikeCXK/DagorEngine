// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <render/antiAliasing_legacy.h>
#include <render/daFrameGraph/nodeHandle.h>

class SGSR2 : public AntiAliasing
{
public:
  SGSR2(const IPoint2 &outputResolution);

  bool needMotionVectors() const override { return true; }
  float getLodBias() const override { return lodBias; }
  bool isAvailable() const override { return available; }

private:
  bool available;
  dafg::NodeHandle applierNode;
};
