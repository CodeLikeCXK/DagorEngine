from "dagor.math" import Color3


from "params.gen.nut" import *

include_decl_h("LightfxShadow");
declare_extern_struct("LightfxShadowParams");

begin_declare_params("LightFx");

declare_struct("LightFxColor", 3,
[
  { name="allow_game_override", type="bool", defVal=0 },
  { name="color", type="E3DCOLOR" },
  { name="scale", type="real", defVal=10 },
  { name="rFunc", type="cubic_curve", color=Color3(255,   0,   0) },
  { name="gFunc", type="cubic_curve", color=Color3(  0, 255,   0) },
  { name="bFunc", type="cubic_curve", color=Color3(  0,   0, 255) },
  { name="aFunc", type="cubic_curve", color=Color3(255, 255, 255) },
]);


declare_struct("LightFxSize", 2,
[
  { name="radius", type="real", defVal=10 },
  { name="sizeFunc", type="cubic_curve" },
]);


declare_struct("LightFxParams", 3,
[
  { name="phaseTime", type="real", defVal=1 },
  { name="burstMode", type="bool", defVal=false },
  { name="cloudLight", type="bool", defVal=false },
  { name="color", type="LightFxColor" },
  { name="size", type="LightFxSize" },
  { name="shadow", type="LightfxShadowParams" },
]);



end_declare_params("light", 2, [
  {struct="LightFxParams"},
]);
