//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <EASTL/utility.h> // eastl::swap

/*
 * Owning generic handle. Similar to eastl::unique_ptr<> but more convenient to use for non pointer types
 */
template <typename HandleDeleter, typename T = int, T INVALID_VAL = -1>
class UniqueResHandle
{
  T handle = INVALID_VAL;

public:
  T release()
  {
    T tmp = handle;
    handle = INVALID_VAL;
    return tmp;
  }
  void reset()
  {
    if (handle != INVALID_VAL)
    {
      HandleDeleter()(handle);
      handle = INVALID_VAL;
    }
  }
  UniqueResHandle() {}
  UniqueResHandle(const UniqueResHandle &) = delete;
  UniqueResHandle &operator=(const UniqueResHandle &) = delete;
  UniqueResHandle(UniqueResHandle &&rhs) : handle(rhs.handle) { rhs.handle = INVALID_VAL; }
  UniqueResHandle &operator=(UniqueResHandle &&rhs)
  {
    eastl::swap(handle, rhs.handle);
    return *this;
  }
  UniqueResHandle &operator=(T h)
  {
    if (h != handle)
    {
      reset();
      handle = h;
    }
    return *this;
  }
  bool valid() const { return handle != INVALID_VAL; }
  explicit operator bool() const { return valid(); }

  // NOTE: this class does  not provide an operator T() function.
  // This is because automatic conversion is unsafe and error prone.
  T get() const
  {
    G_ASSERT_RETURN(valid(), INVALID_VAL);
    return handle;
  }
  ~UniqueResHandle() { reset(); }
};
