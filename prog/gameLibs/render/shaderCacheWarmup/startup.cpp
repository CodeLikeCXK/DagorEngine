// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/shaderCacheWarmup/shaderCacheWarmup.h>

#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_info.h>
#include <ioSys/dag_dataBlock.h>
#include <ioSys/dag_dataBlockUtils.h>
#include <osApiWrappers/dag_miscApi.h>
#include <startup/dag_globalSettings.h>

namespace shadercache
{
static bool warmup_enabled(const DataBlock *blk)
{
  const char *platform = get_platform_string_id();
  return blk->getBool(platform, false) && blk->getBool(d3d::get_driver_name(), false);
}

static Tab<const char *> get_shaders_to_warmup(const char *pipeline_type, const DataBlock *blk)
{
  const DataBlock *shaders = blk->getBlockByName(pipeline_type);

  Tab<const char *> shaderNames;

  if (shaders && shaders->paramCount() > 0)
  {
    shaderNames.reserve(shaders->paramCount());
    for (size_t i = 0; i < shaders->paramCount(); ++i)
    {
      const char *name = shaders->getParamName(i);
      shaderNames.push_back(name);
    }
  }

  return shaderNames;
}

void warmup_shaders_from_settings(const bool is_loading_thread)
{
  warmup_shaders_from_settings(WarmupParams{}, is_loading_thread, false);
}

void warmup_shaders_from_settings(const WarmupParams &params, const bool is_loading_thread, const bool backgroundWarmup)
{
  const DataBlock *warmupBlk = ::dgs_get_settings()->getBlockByNameEx("shadersWarmup");

  if (warmup_enabled(warmupBlk))
  {
    debug("starting shaders warmup");

    if (backgroundWarmup)
      warmupBlk = warmupBlk->getBlockByNameEx("backgroundWarmup");

    Tab<const char *> graphicsShaders = get_shaders_to_warmup("graphics", warmupBlk);
    Tab<const char *> computeShaders = get_shaders_to_warmup("compute", warmupBlk);

    warmup_shaders(graphicsShaders, computeShaders, params, is_loading_thread);
  }
}
} // namespace shadercache