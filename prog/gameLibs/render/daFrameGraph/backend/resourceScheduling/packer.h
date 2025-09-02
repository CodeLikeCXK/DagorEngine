// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <EASTL/span.h>
#include <EASTL/fixed_function.h>

// A packer is an algorithm that decides where to place resources in
// memory based on their size and lifetime.

namespace dafg
{

struct PackerInput
{
  constexpr static uint64_t NO_PIN = static_cast<uint64_t>(-1);
  struct Resource
  {
    // Interpreted as a wraparound whenever start >= end
    uint32_t start; // included
    uint32_t end;   // not included

    uint64_t size;
    uint64_t align;

    // Force this resource to be placed at this offset.
    // Only usable for wraparound resources (start >= end).
    uint64_t pin;

    uint32_t doAlign(uint32_t offset) const { return ((offset + align - 1) / align) * align; }

    uint32_t sizeWithPadding(uint32_t offset) const { return size + doAlign(offset) - offset; }
  };

  // List of all resources to be packed. Resources can also wrap around,
  // i.e. finish their lifetime before starting it.
  eastl::span<Resource const> resources;

  // Length of resource lifetime timeline, i.e. moment after which
  // lifetimes wrap around.
  uint32_t timelineSize;
  // Constrains the output such that resulting heapSize is not greater than this number.
  // Some resource might receive a NOT_SCHEDULED offset to facilitate this constraint.
  uint64_t maxHeapSize;
};

// Offsets in memory corresponding to each resource in input order
struct PackerOutput
{
  // Resources might have been NOT_ALLOCATED if their size is zero
  constexpr static uint64_t NOT_ALLOCATED = static_cast<uint64_t>(-1);
  // Resources might receive a NOT_SCHEDULED offset
  // to facilitate the maxHeapSize constraint.
  constexpr static uint64_t NOT_SCHEDULED = static_cast<uint64_t>(-2);
  // WARNING: references memory stored inside the packer object
  eastl::span<uint64_t> offsets;
  uint64_t heapSize;
};

// Packer objects are not meant to be cached and reused.
using Packer = eastl::fixed_function<64, PackerOutput(PackerInput)>;

enum PackerType : int
{
  Baseline = 0,
  GreedyScanline,
  Boxing,      // Experimental. Do not use in production.
  AdHocBoxing, // Experimental. Do not use in production.
  COUNT
};

Packer make_greedy_scanline_packer();
Packer make_boxing_packer();
Packer make_adhoc_boxing_packer();

Packer make_baseline_packer();

} // namespace dafg
