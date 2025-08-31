// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <assets/daBuildExpPluginChain.h>
#include <assets/assetPlugin.h>
#include <assets/asset.h>
#include <assets/assetMgr.h>
#include <assets/assetRefs.h>
#include <dag/dag_vector.h>

static const char *TYPE = "efx";

BEGIN_DABUILD_PLUGIN_NAMESPACE(efx)

class EfxRefs : public IDagorAssetRefProvider
{
public:
  const char *__stdcall getRefProviderIdStr() const override { return "efx refs"; }
  const char *__stdcall getAssetType() const override { return TYPE; }

  void __stdcall onRegister() override {}
  void __stdcall onUnregister() override {}

  void __stdcall getAssetRefs(DagorAsset &a, Tab<Ref> &refs) override
  {
    refs.clear();

    const char *fxName = a.props.getStr("fx", "");
    DagorAsset *fxAsset = a.getMgr().findAsset(fxName);
    IDagorAssetRefProvider::Ref &fxRef = refs.push_back();
    fxRef.flags = 0;
    if (fxAsset)
      fxRef.refAsset = fxAsset;
    else
      fxRef.setBrokenRef(fxName);

    const char *sfxName = a.props.getStr("sfx", "");
    if (*sfxName != 0)
    {
      DagorAsset *sfxAsset = a.getMgr().findAsset(sfxName);
      IDagorAssetRefProvider::Ref &sfxRef = refs.push_back();
      sfxRef.flags = 0;
      if (sfxAsset)
        sfxRef.refAsset = sfxAsset;
      else
        sfxRef.setBrokenRef(sfxName);
    }
  }
};

class EfxRefProviderPlugin : public IDaBuildPlugin
{
public:
  bool __stdcall init(const DataBlock &appblk) override { return true; }
  void __stdcall destroy() override { delete this; }

  int __stdcall getExpCount() override { return 0; }
  const char *__stdcall getExpType(int) override { return nullptr; }
  IDagorAssetExporter *__stdcall getExp(int) override { return nullptr; }

  int __stdcall getRefProvCount() override { return 1; }
  const char *__stdcall getRefProvType(int idx) override { return TYPE; }
  IDagorAssetRefProvider *__stdcall getRefProv(int idx) override { return &ref; }

protected:
  EfxRefs ref;
};

DABUILD_PLUGIN_API IDaBuildPlugin *__stdcall get_dabuild_plugin() { return new (midmem) EfxRefProviderPlugin; }

END_DABUILD_PLUGIN_NAMESPACE(efx)
REGISTER_DABUILD_PLUGIN(efx, nullptr)
