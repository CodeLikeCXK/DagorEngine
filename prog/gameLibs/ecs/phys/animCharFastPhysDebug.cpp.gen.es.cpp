// Built with ECS codegen version 1.0
#include <daECS/core/entitySystem.h>
#include <daECS/core/componentTypes.h>
#include "animCharFastPhysDebug.cpp.inl"
ECS_DEF_PULL_VAR(animCharFastPhysDebug);
#include <daECS/core/internal/performQuery.h>
static constexpr ecs::ComponentDesc debug_draw_fast_phys_es_comps[] =
{
//start of 1 rw components at [0]
  {ECS_HASH("animchar"), ecs::ComponentTypeInfo<AnimV20::AnimcharBaseComponent>()},
//start of 2 rq components at [1]
  {ECS_HASH("animchar_fast_phys"), ecs::ComponentTypeInfo<FastPhysTag>()},
  {ECS_HASH("animchar_fast_phys_debug_render"), ecs::ComponentTypeInfo<ecs::Tag>()}
};
static void debug_draw_fast_phys_es_all(const ecs::UpdateStageInfo &__restrict info, const ecs::QueryView & __restrict components)
{
  auto comp = components.begin(), compE = components.end(); G_ASSERT(comp!=compE);
  do
    debug_draw_fast_phys_es(*info.cast<UpdateStageInfoRenderDebug>()
    , ECS_RW_COMP(debug_draw_fast_phys_es_comps, "animchar", AnimV20::AnimcharBaseComponent)
    );
  while (++comp != compE);
}
static ecs::EntitySystemDesc debug_draw_fast_phys_es_es_desc
(
  "debug_draw_fast_phys_es",
  "prog/gameLibs/ecs/phys/animCharFastPhysDebug.cpp.inl",
  ecs::EntitySystemOps(debug_draw_fast_phys_es_all),
  make_span(debug_draw_fast_phys_es_comps+0, 1)/*rw*/,
  empty_span(),
  make_span(debug_draw_fast_phys_es_comps+1, 2)/*rq*/,
  empty_span(),
  ecs::EventSetBuilder<>::build(),
  (1<<UpdateStageInfoRenderDebug::STAGE)
,"dev,render",nullptr,"*");
static constexpr ecs::ComponentDesc get_animchar_by_name_ecs_query_comps[] =
{
//start of 2 ro components at [0]
  {ECS_HASH("eid"), ecs::ComponentTypeInfo<ecs::EntityId>()},
  {ECS_HASH("animchar__res"), ecs::ComponentTypeInfo<ecs::string>()}
};
static ecs::CompileTimeQueryDesc get_animchar_by_name_ecs_query_desc
(
  "get_animchar_by_name_ecs_query",
  empty_span(),
  make_span(get_animchar_by_name_ecs_query_comps+0, 2)/*ro*/,
  empty_span(),
  empty_span());
template<typename Callable>
inline void get_animchar_by_name_ecs_query(Callable function)
{
  perform_query(g_entity_mgr, get_animchar_by_name_ecs_query_desc.getHandle(),
    [&function](const ecs::QueryView& __restrict components)
    {
        auto comp = components.begin(), compE = components.end(); G_ASSERT(comp != compE); do
        {
          function(
              ECS_RO_COMP(get_animchar_by_name_ecs_query_comps, "eid", ecs::EntityId)
            , ECS_RO_COMP(get_animchar_by_name_ecs_query_comps, "animchar__res", ecs::string)
            );

        }while (++comp != compE);
    }
  );
}
