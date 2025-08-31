//
// Dagor Tech 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <util/dag_oaHashNameMap.h>
#include <util/dag_simpleString.h>
#include <util/dag_hierBitArray.h>
#include <generic/dag_tab.h>
#include <libTools/util/twoStepRelPath.h>

class DataBlock;
class DagorAssetMgr;
class DagorAsset;

namespace mkbindump
{
class BinDumpSaveCB;
}

class AssetExportCache
{
public:
  static const int HASH_SZ = 16;
  struct SharedData;

  AssetExportCache() : rec(midmem), adPos(midmem), assetTypeVer(midmem)
  {
    memset(&target, 0, sizeof(target));
    touchMarkSz = 0;
    timeChanged = 0;
  }

  void reset();

  bool load(const char *cache_fname, const DagorAssetMgr &mgr, int *end_pos = NULL);
  bool save(const char *cache_fname, const DagorAssetMgr &mgr, mkbindump::BinDumpSaveCB *a_data = nullptr, int *header_size = nullptr);

  void resetTouchMark();
  bool checkAllTouched();
  void removeUntouched();

  bool checkFileChanged(const char *fname);
  bool checkDataBlockChanged(const char *fname_ref, DataBlock &blk, int test_mode = 0);
  bool checkAssetExpVerChanged(int asset_type, unsigned classid, int ver);
  bool checkTargetFileChanged(const char *fname);
  void updateFileHash(const char *fname);

  bool getAssetDataPos(const char *full_asset_name, int &data_ofs, int &data_len);
  bool getAssetDataPos(const char *aname)
  {
    int o, l;
    return getAssetDataPos(aname, o, l);
  }
  bool getTargetFileHash(unsigned char out_hash[HASH_SZ]);
  dag::ConstSpan<String> getWarnings() const { return warnings; }

  void setTargetFileHash(const char *fname);
  void setAssetDataPos(const char *full_asset_name, int data_ofs, int data_len);
  void resetAssetDataPos(const char *aname) { setAssetDataPos(aname, -1, 0); }
  void setAssetExpVer(int asset_type, unsigned classid, int ver);
  void setWarnings(const dag::ConstSpan<String> in_warnings) { warnings = in_warnings; }
  bool resetExtraAssets(dag::ConstSpan<DagorAsset *> assets);

  static bool getFileHash(const char *fname, unsigned char out_hash[HASH_SZ]);
  static void getDataHash(const void *data, int data_len, unsigned char out_hash[HASH_SZ]);
  static unsigned getFileTime(const char *fname, unsigned &out_sz);

  static void setSdkRoot(const char *root_dir, const char *subdir = nullptr) { sdkRoot.setSdkRoot(root_dir, subdir); }
  static const char *mkRelPath(const char *fpath, TwoStepRelPath::storage_t &stor) { return sdkRoot.mkRelPath(fpath, stor); }

  bool isTimeChanged() { return timeChanged == 1; }

  static void createSharedData(const char *fname);
  static void reloadSharedData();
  static void *getSharedDataPtr();
  static void setSharedDataPtr(void *p);
  static void sharedDataResetRebuildTypesList();
  static void sharedDataAddRebuildType(int a_type);
  static void sharedDataRemoveRebuildType(int a_type);
  static void sharedDataResetForceRebuildAssetsList();
  static void sharedDataAddForceRebuildAsset(const char *asset_name_typified);
  static bool sharedDataIsAssetInForceRebuildList(const DagorAsset &a);
  static bool sharedDataGetFileHash(const char *fname, unsigned char out_hash[HASH_SZ]);
  static void sharedDataAppendHash(const void *data, size_t data_len, unsigned char inout_hash[HASH_SZ]);
  static bool saveSharedData();

  static void setJobSharedMem(void *p);
  static void *getJobSharedMem();

protected:
  struct FileDataHash
  {
    unsigned char hash[HASH_SZ];
    unsigned timeStamp;
    unsigned changed : 1, _resv0 : 31;
  };
  struct AssetData
  {
    int ofs, len;
  };
  struct AssetTypeVer
  {
    unsigned classId;
    int ver;
  };

  OAHashNameMap<true> fnames;
  OAHashNameMap<true> anames;
  Tab<FileDataHash> rec;
  Tab<AssetData> adPos;
  Tab<AssetTypeVer> assetTypeVer;
  Tab<String> warnings;
  FileDataHash target;

  typedef HierBitArray<ConstSizeBitArray<8>> bitarray_t;
  bitarray_t touchMark;
  int touchMarkSz;
  unsigned timeChanged : 1;

  static TwoStepRelPath sdkRoot;

  void touch(int i) { (i < touchMarkSz) ? touchMark.set(i) : (void)0; }
};
