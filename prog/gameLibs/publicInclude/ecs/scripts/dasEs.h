//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <cstdint>

class SqModules;
typedef struct SQVM *HSQUIRRELVM;

typedef void (*TInitDas)();

namespace ecs
{
class EntityManager;
}

namespace bind_dascript
{
enum class HotReload;
enum class LoadDebugCode;

enum class AotMode
{
  NO_AOT,
  AOT,
};

enum class AotModeIsRequired
{
  NO,
  YES,
};

enum class ResolveECS
{
  NO,
  YES
};

enum class EnableDebugger
{
  NO,
  YES
};

enum class UnloadDebugAgents
{
  NO,
  YES
};

enum class ValidateDasGC
{
  NO,
  YES
};


enum class LogAotErrors
{
  NO,
  YES,
};

struct LoadEntryScriptCtx
{
  int64_t additionalMemoryUsage = 0;
};

enum class DasSyntax
{
  V1_0,
  V1_5,
  V2_0,
};

struct LoadingDasEcsContextRAII
{
  ecs::EntityManager *prev = nullptr;
  ecs::EntityManager *mgr = nullptr;
  LoadingDasEcsContextRAII(ecs::EntityManager *mgr_);
  ~LoadingDasEcsContextRAII();
};

bool get_always_use_main_thread_env();
void set_always_use_main_thread_env(bool value);
void set_worker_threads_mode(bool on);
bool is_in_worker_threads_mode();
bool is_in_documentation();
void set_is_in_documentation(bool value);
void set_das_root(const char *root_path);
void free_serializer_buffer(bool success);
void set_command_line_arguments(int argc, char *argv[]);
int set_load_threads_num(int load_threads_num);
int get_load_threads_num();
DasSyntax set_das_syntax(DasSyntax syntax);
void set_thread_init_script(const char *file_name);
ResolveECS get_resolve_ecs_on_load();
void set_resolve_ecs_on_load(ResolveECS);
void init_das(AotMode enable_aot, HotReload allow_hot_reload, LogAotErrors log_aot_errors, DasSyntax syntax);
void pull_das();
void init_scripts(HotReload hot_reload_mode, LoadDebugCode load_debug_code, const char *pak);
void init_systems(AotMode aot_mode, HotReload hot_reload_mode, LoadDebugCode load_debug_code, LogAotErrors log_aot_errors,
  DasSyntax syntax, const char *pak = nullptr);
bool load_das_script(const char *name, const char *program_text);
bool load_das_script(const char *fname);
bool load_das_script_debugger(const char *fname);
bool load_das_script_with_debugcode(const char *fname);
bool load_entry_script(const char *entry_point_name, TInitDas init, LoadEntryScriptCtx ctx = {});
// internal use only
void begin_loading_queue();
bool stop_loading_queue(TInitDas init, void *user_data = nullptr);
void end_loading_queue(LoadEntryScriptCtx ctx);

bool main_thread_post_load();
bool unload_es_script(const char *fname);
bool reload_all_scripts(const char *entry_point_name, TInitDas init);
void set_das_aot(AotMode value);
AotMode get_das_aot();
void set_das_log_aot_errors(LogAotErrors value);
LogAotErrors get_das_log_aot_errors();
enum ReloadResult
{
  NO_CHANGES,
  RELOADED,
  ERRORS
}; // check times
ReloadResult reload_all_changed();
void shutdown_systems();

// mem_scale_threshold - threshold for memory usage scale, optimal value is about 2.0, minimal value is 1.0
// validate - gc validation mode, check for memory leaks, slowdown execution
void collect_heap(float mem_scale_threshold, ValidateDasGC validate);

void start_multiple_scripts_loading(); // prevents calls to es_reset_order during es registrations
void drop_multiple_scripts_loading();  // unset das_is_in_init_phase, doesn't call es_reset_order
void end_multiple_scripts_loading();   // calls es_reset_order

void load_event_files(bool do_load);
size_t dump_memory();
void dump_statistics();

void init_sandbox(const char *pak = nullptr);
void set_sandbox(bool is_sandbox);
void set_game_mode(bool is_active);

void bind_das_events(SqModules *modules_mgr);
void bind_das(SqModules *modules_mgr);
void unload_all_das_scripts_without_debug_agents();
void das_load_ecs_templates();

void set_loading_profiling_enabled(bool enabled);

bool fill_stack_while_compile_das();
}; // namespace bind_dascript
