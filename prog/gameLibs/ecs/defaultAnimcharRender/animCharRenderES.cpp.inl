// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <ecs/core/entitySystem.h>
#include <daECS/core/coreEvents.h>
#include <ecs/core/attributeEx.h>
#include <daECS/core/updateStage.h>
#include <ecs/anim/anim.h>
#include <perfMon/dag_statDrv.h>
#include <animChar/dag_animCharVis.h>
#include <3d/dag_render.h>
#include <ecs/render/updateStageRender.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <ecs/anim/animchar_visbits.h>

// this is for demo purposes only, should not be used in production.
// no occlusion, unoptimal culling check!
// just a sample!

ECS_TAG(render)
ECS_AFTER(after_camera_sync)
static __forceinline void animchar_before_render_es(const UpdateStageInfoBeforeRender &stg,
  AnimV20::AnimcharRendComponent &animchar_render, const AnimcharNodesMat44 &animchar_node_wtm, vec4f &animchar_bsph,
  bbox3f &animchar_bbox, animchar_visbits_t &animchar_visbits, bool animchar_render__enabled = true)
{
  mat44f globtm;
  d3d::getglobtm(globtm);
  animchar_render.setVisible(animchar_render__enabled);
  animchar_render.beforeRender(animchar_node_wtm, animchar_bsph, animchar_bbox, Frustum(globtm), 1.0f, nullptr, stg.camPos);
  animchar_visbits = animchar_render.getVisBits();
}

static __forceinline void render_animchar_opaque(AnimV20::AnimcharRendComponent &animchar_render, animchar_visbits_t &animchar_visbits,
  const Point3 &cam_pos, int main_render)
{
  animchar_render.render(cam_pos);
  if (main_render)
    animchar_visbits |= VISFLG_MAIN_CAMERA_RENDERED; //< store visibility result for render trans
}

ECS_TAG(render)
ECS_NO_ORDER
static __forceinline void animchar_render_opaque_es(const UpdateStageInfoRender &stg, AnimV20::AnimcharRendComponent &animchar_render,
  const bbox3f &animchar_bbox, animchar_visbits_t &animchar_visbits)
{
  mat44f globtm;
  d3d::getglobtm(globtm);
  if (AnimV20::AnimcharRendComponent::check_visibility(animchar_visbits, animchar_bbox, true, Frustum(globtm), nullptr))
  {
    render_animchar_opaque(animchar_render, animchar_visbits, stg.viewItm.getcol(3), true);
  }
}

ECS_TAG(render)
ECS_NO_ORDER
static __forceinline void animchar_render_trans_es(const UpdateStageInfoRenderTrans &stg,
  AnimV20::AnimcharRendComponent &animchar_render, const animchar_visbits_t &animchar_visbits)
{
  if (animchar_visbits & VISFLG_MAIN_CAMERA_RENDERED) //< reuse visibility check from render
    animchar_render.renderTrans(stg.viewItm.getcol(3));
}
