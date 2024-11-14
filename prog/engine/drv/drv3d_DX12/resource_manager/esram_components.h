// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <driver.h>
#include "heap_components.h"

#if DX12_USE_ESRAM
#include "esram_components_xbox.h"
#else
namespace drv3d_dx12::resource_manager
{
using ESRamPageMappingProvider = ResourceMemoryHeapProvider;
}
#endif
