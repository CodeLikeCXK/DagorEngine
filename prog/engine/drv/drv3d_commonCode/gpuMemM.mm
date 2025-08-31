// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <Metal/Metal.h>

#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_info.h>
#include <drv/3d/dag_commands.h>
#include <osApiWrappers/dag_localConv.h>
#include <util/dag_string.h>
#include <EASTL/optional.h>

static int cache_vram = -1;
static eastl::optional<GpuVendor> cache_vendor;
static bool cache_web_driver = false;
static String cache_gpu_desc = String("unknown");

static bool str_control_entry(const char *key, String &out_str)
{
  size_t size = 0;
  if (sysctlbyname(key, NULL, &size, NULL, 0) == -1)
    return false;

  if (char *lower = (char*)memalloc(size, tmpmem))
  {
    sysctlbyname(key, lower, &size, NULL, 0);
    lower = dd_strlwr(lower);
    out_str.setStr(lower, size - 1);
    memfree(lower, tmpmem);
  }

  return true;
}

static void parse_vendor(const char *gpu_str, GpuVendor &out_vendor, bool &out_web_driver)
{
  out_vendor = GpuVendor::UNKNOWN;
  out_web_driver = false;
  if (char *lower = str_dup(gpu_str, tmpmem))
  {
    lower = dd_strlwr(lower);
    if (strstr(lower, "geforce") || strstr(lower, "nvidia"))
    {
      out_vendor = GpuVendor::NVIDIA;
      out_web_driver = strstr(lower, "web");
    }
    else if (strstr(lower, "ati") || strstr(lower, "amd") || strstr(lower, "radeon"))
    {
      out_vendor = GpuVendor::ATI;
    }
    else if (strstr(lower, "intel"))
    {
      out_vendor = GpuVendor::INTEL;
    }
    else if (strstr(lower, "apple"))
    {
      out_vendor = GpuVendor::APPLE;
    }
    memfree(lower, tmpmem);
  }
}

static void get_graphics_info(GpuVendor &out_vendor, String &out_gpu_desc, int &out_vram, bool &out_web_driver)
{
  out_vendor = GpuVendor::UNKNOWN;
  out_vram = 0;
  out_web_driver = false;

  // Get dictionary of all the PCI Devicces
  CFMutableDictionaryRef matchDict = IOServiceMatching("IOPCIDevice");

  // Create an iterator
  io_iterator_t iterator;

  constexpr mach_port_t defaultMasterPort = NULL;
  if (IOServiceGetMatchingServices(defaultMasterPort,matchDict,
                                   &iterator) == kIOReturnSuccess)
  {
    // Iterator for devices found
    io_registry_entry_t regEntry;

    while ((regEntry = IOIteratorNext(iterator)))
    {
      // Put this services object into a dictionary object.
      CFMutableDictionaryRef serviceDictionary;
      if (IORegistryEntryCreateCFProperties(regEntry,
                                            &serviceDictionary,
                                            kCFAllocatorDefault,
                                            kNilOptions) != kIOReturnSuccess)
      {
        // Service dictionary creation failed.
        IOObjectRelease(regEntry);
        continue;
      }
      const void *GPUModel = CFDictionaryGetValue(serviceDictionary, @"model");

      if (GPUModel != nil && CFGetTypeID(GPUModel) == CFDataGetTypeID() &&
          out_vendor != GpuVendor::NVIDIA && out_vendor != GpuVendor::ATI)
      {
        // Create a string from the CFDataRef.
        NSString *modelName = [[NSString alloc] initWithData:
                                (NSData *)GPUModel encoding:NSASCIIStringEncoding];

        GpuVendor vendor;
        bool webDriver;
        String gpuDesc([modelName UTF8String]);
        parse_vendor(gpuDesc.data(), vendor, webDriver);

        if (vendor != GpuVendor::UNKNOWN)
        {
          out_vendor = vendor;
          out_web_driver = webDriver;
          out_vram = 0;
          out_gpu_desc = gpuDesc;
          const void *vramTotal = CFDictionaryGetValue(serviceDictionary, CFSTR("VRAM,totalMB"));
          const void *vramTotalSize = CFDictionaryGetValue(serviceDictionary, CFSTR("VRAM,totalsize"));
          if (vramTotal && CFGetTypeID(vramTotal) == CFDataGetTypeID() && [(const NSData*)vramTotal length] == sizeof(int32_t))
          {
            int32_t count = 0;
            [(const NSData*)vramTotal getBytes:&count length:sizeof(count)];
            out_vram = count * 1024;
          }
          else if (vramTotal && CFGetTypeID(vramTotal) == CFNumberGetTypeID())
          {
            ssize_t count = 0;
            CFNumberGetValue((CFNumberRef)vramTotal, kCFNumberSInt64Type, &count);
            out_vram = count * 1024;
          }
          else if (vramTotalSize && CFGetTypeID(vramTotalSize) == CFDataGetTypeID() && [(const NSData*)vramTotalSize length] == sizeof(int64_t))
          {
            int64_t count = 0;
            [(const NSData*)vramTotalSize getBytes:&count length:sizeof(count)];
            out_vram = count / 1024;
          }
          else if (vramTotalSize && CFGetTypeID(vramTotalSize) == CFNumberGetTypeID())
          {
            ssize_t count = 0;
            CFNumberGetValue((CFNumberRef)vramTotalSize, kCFNumberSInt64Type, &count);
            out_vram = count / 1024;
          }

          const CFStringRef nvdaType = (const CFStringRef)CFDictionaryGetValue(serviceDictionary, CFSTR("NVDAType"));
          if (nvdaType && CFGetTypeID(nvdaType) == CFStringGetTypeID())
          {
            out_web_driver = [[(NSString*)nvdaType lowercaseString] containsString:@"web"];
          }
        }

        [modelName release];
      }
      // Release the dictionary
      CFRelease(serviceDictionary);
      // Release the serviceObject
      IOObjectRelease(regEntry);
    }
    // Release the iterator
    IOObjectRelease(iterator);
  }
}

void mac_get_vdevice_info(int& vram, GpuVendor& vendor, String &gpu_desc, bool &web_driver)
{
  if (cache_vram != -1)
  {
    vram = cache_vram;
    vendor = *cache_vendor;
    web_driver = cache_web_driver;
    gpu_desc = cache_gpu_desc;
    return;
  }

  vram = 0;
  vendor = GpuVendor::UNKNOWN;
  web_driver = false;

  CFMutableDictionaryRef matchDict = IOServiceMatching("IOAccelerator");

  io_iterator_t iterator;

  constexpr mach_port_t defaultMasterPort = NULL;
  if (IOServiceGetMatchingServices(defaultMasterPort, matchDict, &iterator) == kIOReturnSuccess)
  {
    io_registry_entry_t regEntry;

    while ((regEntry = IOIteratorNext(iterator)))
    {
      CFMutableDictionaryRef serviceDictionary;

      if (IORegistryEntryCreateCFProperties(regEntry, &serviceDictionary,
                          kCFAllocatorDefault, kNilOptions) != kIOReturnSuccess)
      {
        IOObjectRelease(regEntry);
        continue;
      }

      NSString* GPUModel = (NSString*)CFDictionaryGetValue(serviceDictionary, @"CFBundleIdentifier");

      if (GPUModel != nil)
      {
        GpuVendor gpuVendor;
        bool gpuWebDriver;
        parse_vendor([GPUModel UTF8String], gpuVendor, gpuWebDriver);
        if (vram != 0 && gpuVendor != GpuVendor::ATI && gpuVendor != GpuVendor::NVIDIA)
          continue;
        vendor = gpuVendor;
        web_driver = gpuWebDriver;

        if (gpuVendor != GpuVendor::ATI && gpuVendor != GpuVendor::NVIDIA)
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();

            // use only half of available mem for systems with shared mem
            vram = device.recommendedMaxWorkingSetSize / 1024 / 2;

            [device release];
        }
        else
        {
          CFMutableDictionaryRef perf_properties = (CFMutableDictionaryRef)CFDictionaryGetValue(serviceDictionary, CFSTR("PerformanceStatistics"));
          if (perf_properties)
          {
            ssize_t count = 0;
            int used = 0;
            if (const void *vram_used = CFDictionaryGetValue(perf_properties, CFSTR("vramUsedBytes")))
            {
              CFNumberGetValue((CFNumberRef)vram_used, kCFNumberSInt64Type, &count);
              used = count / 1024;
            }
            else if (const void *gart_used = CFDictionaryGetValue(perf_properties, CFSTR("gartUsedBytes")))
            {
              CFNumberGetValue((CFNumberRef)gart_used, kCFNumberSInt64Type, &count);
              used = count / 1024;
            }

            if (const void *vram_free = CFDictionaryGetValue(perf_properties, CFSTR("vramFreeBytes")))
            {
              CFNumberGetValue((CFNumberRef)vram_free, kCFNumberSInt64Type, &count);
              vram = count / 1024 + used;
            }
            else if (const void *gart_free = CFDictionaryGetValue(perf_properties, CFSTR("gartFreeBytes")))
            {
              CFNumberGetValue((CFNumberRef)gart_free, kCFNumberSInt64Type, &count);
              vram = count / 1024 + used;
            }
            else if (const void *gart_size = CFDictionaryGetValue(perf_properties, CFSTR("gartSizeBytes")))
            {
              CFNumberGetValue((CFNumberRef)gart_size, kCFNumberSInt64Type, &count);
              vram = count / 1024 + used;
            }
            else if (const void *vramLargestFree = CFDictionaryGetValue(perf_properties, CFSTR("vramLargestFreeBytes")))
            {
              CFNumberGetValue((CFNumberRef)vramLargestFree, kCFNumberSInt64Type, &count);
              vram = count / 1024 + used;
            }
          }
        }
      }

      CFRelease(serviceDictionary);
      IOObjectRelease(regEntry);
    }

    IOObjectRelease(iterator);
  }

  debug("Mac gpu info from IOA: vendor=%d vram=%d web=%d desc=%s", eastl::to_underlying(vendor), vram, web_driver, gpu_desc.str());

  GpuVendor gpuVendor;
  String gpuInfo;
  int gpuVRam;
  bool gpuWebDriver;
  get_graphics_info(gpuVendor, gpuInfo, gpuVRam, gpuWebDriver);
  if (gpuVendor != GpuVendor::UNKNOWN &&
    !(gpuVendor == GpuVendor::INTEL && gpuVRam == 0 && (vendor != GpuVendor::INTEL || vram != 0)))
  {
    debug("Mac gpu info from PCI: vendor=%d vram=%d web=%d desc=%s", eastl::to_underlying(gpuVendor), gpuVRam, gpuWebDriver, gpuInfo.str());

    if (gpuVRam != 0 || vendor != gpuVendor)
      vram = gpuVRam;
    if (gpuWebDriver || vendor != gpuVendor)
      web_driver = gpuWebDriver;
    gpu_desc = gpuInfo;
    vendor = gpuVendor;
  }
  else if (vendor == GpuVendor::UNKNOWN)
  {
    debug("Unknown vendor found. Accept it as Intel");
    vendor = GpuVendor::INTEL;
  }

  cache_vram = vram;
  cache_vendor = vendor;
  cache_web_driver = web_driver;
  cache_gpu_desc = gpu_desc;
}

bool mac_is_web_gpu_driver()
{
  int vram;
  GpuVendor vendor;
  String gpuDesc;
  bool webDriver;
  mac_get_vdevice_info(vram, vendor, gpuDesc, webDriver);
  return webDriver;
}

bool mac_get_model(String &out_str)
{
  return str_control_entry("hw.model", out_str);
}

unsigned d3d::get_dedicated_gpu_memory_size_kb()
{
  int vram = 0;
  GpuVendor vendor = GpuVendor::UNKNOWN;
  String gpuDesc;
  bool webDriver;

  mac_get_vdevice_info(vram, vendor, gpuDesc, webDriver);

  return vram;
}
