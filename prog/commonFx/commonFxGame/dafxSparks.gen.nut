from "dagor.math" import Color3, Color4, Point2, Point3

from "params.gen.nut" import *

include_decl_h("DafxEmitter");

begin_declare_params("DafxSparks");

declare_struct("SparkFxValueCurveOpt", 2,
[
  { name="enabled", type="bool", defVal=0 },
  { name="curve", type="cubic_curve" },
]);

declare_struct("DafxLinearDistribution", 1,
[
  { name="start", type="real" },
  { name="end", type="real" },
]);

declare_struct("DafxRectangleDistribution", 1,
[
  { name="leftTop", type="Point2" },
  { name="rightBottom", type="Point2" },
]);

declare_struct("DafxCubeDistribution", 1,
[
  { name="center", type="Point3" },
  { name="size", type="Point3" },
]);

declare_struct("DafxSphereDistribution", 1,
[
  { name="center", type="Point3" },
  { name="radius", type="real" },
]);

declare_struct("DafxSectorDistribution", 1,
[
  { name="center", type="Point3", defVal=Point3(0,0,0) },
  { name="scale", type="Point3", defVal=Point3(1,1,1) },
  { name="radiusMin", type="real" },
  { name="radiusMax", type="real" },
  { name="azimutMax", type="real", defVal=1 },
  { name="azimutNoise", type="real", defVal=0 },
  { name="inclinationMin", type="real", defVal=0 },
  { name="inclinationMax", type="real", defVal=1 },
  { name="inclinationNoise", type="real", defVal=0 },
]);

declare_struct("DafxSparksSimParams", 12,
[
  { name="pos", type="DafxCubeDistribution" },
  { name="width__mm", type="DafxLinearDistribution" },
  { name="life", type="DafxLinearDistribution" },
  { name="seed", type="int", defVal=0 },
  { name="velocity", type="DafxSectorDistribution" },
  { name="restitution", type="real", defVal=0.2 },
  { name="friction", type="real", defVal=2 },
  { name="color0", type="E3DCOLOR", defVal=Color4(128, 128, 128, 255) },
  { name="color1", type="E3DCOLOR", defVal=Color4(255, 255, 255, 255) },
  { name="color1Portion", type="real", defVal=0 },
  { name="hdrBias", type="real", defVal=0},
  { name="colorEnd", type="E3DCOLOR", defVal=Color4(255, 255, 255, 0) },
  { name="velocityBias", type="real", defVal=0 },
  { name="dragCoefficient", type="real", defVal=1 },
  { name="turbulenceForce", type="DafxLinearDistribution" },
  { name="turbulenceFreq", type="DafxLinearDistribution" },
  { name="liftForce", type="Point3", defVal=Point3(0,0,0) },
  { name="liftTime", type="real", defVal=0 },
  { name="spawnNoise", type="real", defVal=0 },
  { name="spawnNoisePos", type="DafxCubeDistribution" },
  { name="hdrScale1", type="real", defVal=2.0 },
  { name="windForce", type="real", defVal=0 },
  { name="gravity_zone", type="list", list=["default", "disabled", "per_emitter", "per_particle"] },
]);

declare_struct("DafxSparksRenParams", 4,
[
  { name="blending", type="list", list=["additive", "alpha_blend"] },
  { name="motionScale", type="real", defVal=0.05 },
  { name="motionScaleMax", type="real", defVal=10 },
  { name="hdrScale", type="real", defVal=2.0 },
  { name="arrowShape", type="real", defVal=0.0 },
]);

declare_struct("DafxSparksGlobalParams", 2,
[
  { name="spawn_range_limit", type="real", defVal=0 },
  { name="max_instances", type="int", defVal=100 },
  { name="player_reserved", type="int", defVal=10 },
  { name="emission_min", type="real", defVal=0 },
  { name="one_point_number", type="real", defVal=3 },
  { name="one_point_radius", type="real", defVal=5 },
  { name="transform_type", type="list", list=["default", "world_space", "local_space"] },
]);

// DafxSparksOptionalModifiers is used for anything that is not directly binded to ParentSimData/ParentRenData structs.
declare_struct("DafxSparksOptionalModifiers", 3,
[
  { name="widthOverLife", type="SparkFxValueCurveOpt" },
  { name="allowScreenProjDiscard", type="bool", defVal=1 },
  { name="collision", type="list", list=["default", "disabled", "relaxed"] },
]);


declare_struct("DafxSparksQuality", 1,
[
  { name="low_quality", type="bool", defVal=1 },
  { name="medium_quality", type="bool", defVal=1 },
  { name="high_quality", type="bool", defVal=1 },
]);

declare_struct("DafxRenderGroup", 1,
[
  { name="type", type="list", list=["highres", "lowres", "underwater"]},
]);

end_declare_params("dafx_sparks", 5, [
  {struct="DafxEmitterParams"},
  {struct="DafxSparksSimParams"},
  {struct="DafxSparksRenParams"},
  {struct="DafxSparksGlobalParams"},
  {struct="DafxSparksOptionalModifiers"},
  {struct="DafxSparksQuality"},
  {struct="DafxRenderGroup"}
]);
