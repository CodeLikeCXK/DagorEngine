// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#if _TARGET_PC_WIN // TODO: tools Linux porting: SplashScreen

class SplashScreen
{
public:
  SplashScreen();
  ~SplashScreen();

  static void kill();

private:
  static void *hwnd;
  static void *bitmap;

  static int bmpW;
  static int bmpH;

  static unsigned registerSplashClass();
  static int __stdcall wndProc(void *h_wnd, unsigned msg, void *w_param, void *l_param);
};

#else

class SplashScreen
{
public:
  SplashScreen() {}
  ~SplashScreen() {}

  static void kill() {}
};

#endif