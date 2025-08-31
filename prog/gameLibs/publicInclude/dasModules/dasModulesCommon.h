//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <daScript/daScript.h>
#include <math/dag_math3d.h>
#include <math/dag_color.h>
#include <math/dag_e3dColor.h>
#include <math/integer/dag_IPoint2.h>
#include <math/integer/dag_IPoint3.h>
#include <math/integer/dag_IPoint4.h>
#include <vecmath/dag_vecMath.h>

namespace das
{
#define MAKE_TYPE_FACTORY_ALIAS(TYPE, DAS_DECL_TYPE, DAS_TYPE) \
  template <>                                                  \
  struct typeFactory<TYPE>                                     \
  {                                                            \
    static TypeDeclPtr make(const ModuleLibrary &lib)          \
    {                                                          \
      G_UNUSED(lib);                                           \
      auto t = make_smart<TypeDecl>(Type::DAS_DECL_TYPE);      \
      t->alias = #TYPE;                                        \
      t->aotAlias = true;                                      \
      return t;                                                \
    }                                                          \
  };                                                           \
  template <>                                                  \
  struct typeName<TYPE>                                        \
  {                                                            \
    static string name() { return #TYPE; }                     \
  };                                                           \
  template <>                                                  \
  struct das_alias<TYPE> : das::das_alias_vec<TYPE, DAS_TYPE>  \
  {};

MAKE_TYPE_FACTORY_ALIAS(Point4, tFloat4, float4)
MAKE_TYPE_FACTORY_ALIAS(Point3, tFloat3, float3)
MAKE_TYPE_FACTORY_ALIAS(Point2, tFloat2, float2)

MAKE_TYPE_FACTORY_ALIAS(IPoint4, tInt4, int4)
MAKE_TYPE_FACTORY_ALIAS(IPoint3, tInt3, int3)
MAKE_TYPE_FACTORY_ALIAS(IPoint2, tInt2, int2)

#undef MAKE_TYPE_FACTORY_ALIAS

#define MAKE_POINT_TYPE_WRAPPER(TYPE) \
                                      \
  template <>                         \
  struct WrapType<TYPE>               \
  {                                   \
    enum                              \
    {                                 \
      value = true                    \
    };                                \
    typedef vec4f type;               \
    typedef vec4f rettype;            \
  };                                  \
                                      \
  template <>                         \
  struct WrapRetType<TYPE>            \
  {                                   \
    typedef TYPE##_WrapArg type;      \
  };                                  \
                                      \
  template <>                         \
  struct WrapArgType<TYPE>            \
  {                                   \
    typedef TYPE##_WrapArg type;      \
  };

struct Point4_WrapArg : Point4
{
  Point4_WrapArg(vec4f t) : Point4(v_extract_x(t), v_extract_y(t), v_extract_z(t), v_extract_w(t)) {}
  operator vec4f() const { return das::vec_loadu(&x); }
};

struct Point3_WrapArg : Point3
{
  Point3_WrapArg(vec4f t) : Point3(v_extract_x(t), v_extract_y(t), v_extract_z(t)) {}
  operator vec4f() const { return das::vec_loadu(&x); }
};

struct Point2_WrapArg : Point2
{
  Point2_WrapArg(vec4f t) : Point2(v_extract_x(t), v_extract_y(t)) {}
  operator vec4f() const { return das::vec_loadu(&x); }
};

MAKE_POINT_TYPE_WRAPPER(Point4)
MAKE_POINT_TYPE_WRAPPER(Point3)
MAKE_POINT_TYPE_WRAPPER(Point2)

#undef MAKE_POINT_TYPE_WRAPPER

template <>
struct das_alias<TMatrix> : das::das_alias_vec<TMatrix, float3x4>
{};

template <>
struct das_alias<TMatrix4> : das::das_alias_vec<TMatrix4, float4x4>
{};

template <typename TAnnotation, typename TParentAnnotation>
inline bool add_annotation(das::Module *module, const das::smart_ptr<TAnnotation> &ann,
  const das::smart_ptr<TParentAnnotation> &parent_ann)
{
  bool result = module->addAnnotation(ann);
  if (result)
    ann->from(parent_ann.get());
  return result;
}

template <>
struct typeFactory<TMatrix>
{
  static TypeDeclPtr make(const ModuleLibrary &lib)
  {
    auto t = typeFactory<das::float3x4>::make(lib);
    t->alias = "TMatrix";
    t->aotAlias = true;
    return t;
  }
};
template <>
struct typeName<TMatrix>
{
  static string name() { return "TMatrix"; }
};

template <>
struct typeFactory<TMatrix4>
{
  static TypeDeclPtr make(const ModuleLibrary &lib)
  {
    auto t = typeFactory<das::float4x4>::make(lib);
    t->alias = "TMatrix4";
    t->aotAlias = true;
    return t;
  }
};
template <>
struct typeName<TMatrix4>
{
  static string name() { return "TMatrix4"; }
};


template <>
struct das_alias<Matrix3> : das::das_alias_vec<Matrix3, float3x3>
{};

template <>
struct typeFactory<Matrix3>
{
  static TypeDeclPtr make(const ModuleLibrary &lib)
  {
    auto t = typeFactory<das::float3x3>::make(lib);
    t->alias = "Matrix3";
    t->aotAlias = true;
    return t;
  }
};

template <>
struct typeName<Matrix3>
{
  static string name() { return "Matrix3"; }
};


template <>
struct cast<E3DCOLOR> : cast<uint32_t>
{};

template <>
struct WrapType<E3DCOLOR>
{
  enum
  {
    value = true
  };
  typedef uint32_t type;
  typedef uint32_t rettype;
};

// aliasing
template <>
struct ToBasicType<Point4>
{
  static constexpr int type = Type::tFloat4;
};
template <>
struct cast<Point4> : cast_fVec<Point4>
{};

template <>
struct ToBasicType<Point3>
{
  static constexpr int type = Type::tFloat3;
};
template <>
struct cast<Point3> : cast_fVec<Point3>
{};

template <>
struct ToBasicType<Point2>
{
  static constexpr int type = Type::tFloat2;
};
template <>
struct cast<Point2> : cast_fVec_half<Point2>
{};

template <>
struct ToBasicType<IPoint4>
{
  static constexpr int type = Type::tInt4;
};
template <>
struct cast<IPoint4> : cast_iVec<IPoint4>
{};

template <>
struct ToBasicType<IPoint3>
{
  static constexpr int type = Type::tInt3;
};
template <>
struct cast<IPoint3> : cast_iVec<IPoint3>
{};

template <>
struct ToBasicType<IPoint2>
{
  static constexpr int type = Type::tInt2;
};
template <>
struct cast<IPoint2> : cast_iVec_half<IPoint2>
{};
}; // namespace das

MAKE_TYPE_FACTORY(Color3, Color3)
MAKE_TYPE_FACTORY(Color4, Color4)
MAKE_TYPE_FACTORY(E3DCOLOR, E3DCOLOR)
