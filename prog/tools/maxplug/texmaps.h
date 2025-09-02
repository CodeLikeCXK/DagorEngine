// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#define NUMTEXMAPS 16

class Texmaps : public TexmapContainer
{
public:
  Texmap *texmap[NUMTEXMAPS];

  Texmaps()
  {
    for (int i = 0; i < NUMTEXMAPS; ++i)
      texmap[i] = NULL;
  }
  Texmap *gettex(int i) { return (Texmap *)GetReference(i); }
  void settex(int i, Texmap *t) { ReplaceReference(i, t); }

  Class_ID ClassID() override { return Texmaps_CID; }
#if defined(MAX_RELEASE_R24) && MAX_RELEASE >= MAX_RELEASE_R24
  void GetClassName(TSTR &s, bool localized = true) { s = _T("DagorTexmaps"); }
#else
  void GetClassName(TSTR &s) { s = _T("DagorTexmaps"); }
#endif
  void DeleteThis() override { delete this; }
  int NumSubs() override { return NUMTEXMAPS; }
  Animatable *SubAnim(int i) override
  {
    if (i >= 0 && i < NUMTEXMAPS)
      return texmap[i];
    return NULL;
  }
#if defined(MAX_RELEASE_R24) && MAX_RELEASE >= MAX_RELEASE_R24
  TSTR SubAnimName(int i, bool localized) override
  {
    TSTR s;
    s.printf(_T("tex %d"), i);
    return s;
  }
#else
  TSTR SubAnimName(int i)
  {
    TSTR s;
    s.printf(_T("tex %d"), i);
    return s;
  }
#endif
  int SubNumToRefNum(int n) override { return n; }
  int NumRefs() override { return NUMTEXMAPS; }
  RefTargetHandle GetReference(int i) override
  {
    if (i >= 0 && i < NUMTEXMAPS)
      return texmap[i];
    return NULL;
  }
  void SetReference(int i, RefTargetHandle rtarg) override
  {
    if (i >= 0 && i < NUMTEXMAPS)
      texmap[i] = (Texmap *)rtarg;
  }
  RefTargetHandle Clone(RemapDir &remap) override
  {
    Texmaps *mtl = new Texmaps;
    for (int i = 0; i < NUMTEXMAPS; ++i)
      mtl->ReplaceReference(i, remap.CloneRef(texmap[i]));
#if MAX_RELEASE >= 4000
    BaseClone(this, mtl, remap);
#endif
    return mtl;
  }

#if defined(MAX_RELEASE_R17) && MAX_RELEASE >= MAX_RELEASE_R17
  RefResult NotifyRefChanged(const Interval &changeInt, RefTargetHandle hTarget, PartID &partID, RefMessage message,
    BOOL propagate) override
#else
  RefResult NotifyRefChanged(Interval changeInt, RefTargetHandle hTarget, PartID &partID, RefMessage message) override
#endif

  {
    switch (message)
    {
      case REFMSG_GET_PARAM_DIM:
      {
        GetParamDim *gpd = (GetParamDim *)partID;
        gpd->dim = defaultDim;
        break;
      }
#if defined(MAX_RELEASE_R24) && MAX_RELEASE >= MAX_RELEASE_R24
      case REFMSG_GET_PARAM_NAME_NONLOCALIZED:
#else
      case REFMSG_GET_PARAM_NAME:
#endif
      {
        GetParamName *gpn = (GetParamName *)partID;
        return REF_STOP;
      }
    }
    return (REF_SUCCEED);
  }
};
