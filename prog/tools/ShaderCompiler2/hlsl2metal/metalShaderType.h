// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

enum class ShaderType : int
{
  Invalid = -1,
  Vertex = 1,
  Pixel = 2,
  Compute = 3,
  Mesh,
  Amplification
};

inline ShaderType profile_to_shader_type(const char *profile)
{
  switch (profile[0])
  {
    case 'c': return ShaderType::Compute;
    case 'v': return ShaderType::Vertex;
    case 'p': return ShaderType::Pixel;
    case 'm': return ShaderType::Mesh;
    case 'a': return ShaderType::Amplification;
    default: return ShaderType::Invalid;
  }
  return ShaderType::Invalid;
}
