// Built with ECS codegen version 1.0
#include <daECS/core/entitySystem.h>
#include <daECS/core/componentTypes.h>
#include "worldRendererGIES.cpp.inl"
ECS_DEF_PULL_VAR(worldRendererGI);
#include <daECS/core/internal/performQuery.h>
//static constexpr ecs::ComponentDesc baked_gi_render_debug_es_comps[] ={};
static void baked_gi_render_debug_es_all(const ecs::UpdateStageInfo &__restrict info, const ecs::QueryView & __restrict components)
{
  G_UNUSED(components);
    baked_gi_render_debug_es(*info.cast<UpdateStageInfoRenderDebug>());
}
static ecs::EntitySystemDesc baked_gi_render_debug_es_es_desc
(
  "baked_gi_render_debug_es",
  "prog/daNetGame/render/world/worldRendererGIES.cpp.inl",
  ecs::EntitySystemOps(baked_gi_render_debug_es_all),
  empty_span(),
  empty_span(),
  empty_span(),
  empty_span(),
  ecs::EventSetBuilder<>::build(),
  (1<<UpdateStageInfoRenderDebug::STAGE)
,"dev,render",nullptr,"*");
static constexpr ecs::ComponentDesc wr_handle_on_hero_teleport_es_comps[] =
{
//start of 1 rq components at [0]
  {ECS_HASH("hero"), ecs::ComponentTypeInfo<ecs::Tag>()}
};
static void wr_handle_on_hero_teleport_es_all_events(const ecs::Event &__restrict evt, const ecs::QueryView &__restrict components)
{
  G_UNUSED(components);
  wr_handle_on_hero_teleport_es(evt
        );
}
static ecs::EntitySystemDesc wr_handle_on_hero_teleport_es_es_desc
(
  "wr_handle_on_hero_teleport_es",
  "prog/daNetGame/render/world/worldRendererGIES.cpp.inl",
  ecs::EntitySystemOps(nullptr, wr_handle_on_hero_teleport_es_all_events),
  empty_span(),
  empty_span(),
  make_span(wr_handle_on_hero_teleport_es_comps+0, 1)/*rq*/,
  empty_span(),
  ecs::EventSetBuilder<EventOnEntityTeleported>::build(),
  0
,"render",nullptr,"*");
//static constexpr ecs::ComponentDesc baked_gi_hud_es_comps[] ={};
static void baked_gi_hud_es_all_events(const ecs::Event &__restrict evt, const ecs::QueryView &__restrict components)
{
  G_UNUSED(components);
  G_FAST_ASSERT(evt.is<RenderEventUI>());
  baked_gi_hud_es(static_cast<const RenderEventUI&>(evt)
        );
}
static ecs::EntitySystemDesc baked_gi_hud_es_es_desc
(
  "baked_gi_hud_es",
  "prog/daNetGame/render/world/worldRendererGIES.cpp.inl",
  ecs::EntitySystemOps(nullptr, baked_gi_hud_es_all_events),
  empty_span(),
  empty_span(),
  empty_span(),
  empty_span(),
  ecs::EventSetBuilder<RenderEventUI>::build(),
  0
,"render",nullptr,"*");
