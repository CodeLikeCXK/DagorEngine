// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <daScript/daScript.h>
#include <dasModules/dasSystem.h>

#include <math/dag_math3d.h>
#include <osApiWrappers/dag_miscApi.h>
#include <perfMon/dag_perfTimer.h>
#include <perfMon/dag_memoryReport.h>
#include <memory/dag_memStat.h>
#include <dasModules/dasMacro.h>
#include <osApiWrappers/dag_clipboard.h>


DAS_BASE_BIND_ENUM(ConsoleModel, ConsoleModel, UNKNOWN, PS4, PS4_PRO, XBOXONE, XBOXONE_S, XBOXONE_X, XBOX_LOCKHART, XBOX_ANACONDA, PS5,
  PS5_PRO, NINTENDO_SWITCH, TOTAL)

namespace bind_dascript
{
static DasExitFunctionPtr das_exit_function_ptr = nullptr;
static DAS_THREAD_LOCAL(das::Context *) current_context;

RAIIStackwalkOnLogerr::RAIIStackwalkOnLogerr(das::Context *c)
{
  old = *current_context;
  *current_context = c;
}
RAIIStackwalkOnLogerr::~RAIIStackwalkOnLogerr() { *current_context = old; }

static int das_stackwalk_log_on_logerr(int lev_tag, const char *fmt, const void *arg, int anum, const char *ctx_file, int ctx_line)
{
  static const int D3DE_TAG = _MAKE4C('D3DE');
  if (lev_tag == LOGLEVEL_ERR || lev_tag == D3DE_TAG)
    if (*current_context != nullptr && ((*current_context)->category.value & uint32_t(das::ContextCategory::dead)) == 0)
      (*current_context)->stackWalk(nullptr, false, false);

  if (orig_debug_log)
    return orig_debug_log(lev_tag, fmt, arg, anum, ctx_file, ctx_line);
  else
    return 1;
}

void enable_das_stackwalk_log_on_logerr()
{
  G_ASSERTF(orig_debug_log == nullptr, "Don't call this function twice!");
  orig_debug_log = debug_set_log_callback(das_stackwalk_log_on_logerr);
}

void set_das_exit_function_ptr(DasExitFunctionPtr func) { das_exit_function_ptr = func; }

void exit_func(int exit_code)
{
  if (das_exit_function_ptr)
    das_exit_function_ptr(exit_code);
  else
    logerr("DagorSystem.exit(%d) called but das_exit_function_ptr not set", exit_code);
}

#if _TARGET_PC_LINUX
// system memory getter can take 2-10ms, caller does not expect it, override with with_crt = false
int get_memory_allocated_kb_linux(bool) { return dagor_memory_stat::get_memory_allocated_kb(false); }
#endif

class DagorSystem final : public das::Module
{
public:
  DagorSystem() : das::Module("DagorSystem")
  {
    das::ModuleLibrary lib(this);
    addBuiltinDependency(lib, require("DagorDataBlock"));
    addEnumeration(das::make_smart<EnumerationConsoleModel>());
    das::addExtern<DAS_BIND_FUN(fatal_func)>(*this, lib, "fatal", das::SideEffects::modifyExternal, "bind_dascript::fatal_func");
    das::addExtern<DAS_BIND_FUN(exit_func)>(*this, lib, "exit", das::SideEffects::modifyExternal, "bind_dascript::exit_func");
    das::addExtern<DAS_BIND_FUN(logmsg_func)>(*this, lib, "logmessage", das::SideEffects::modifyExternal,
      "bind_dascript::logmsg_func");
    das::addExtern<DAS_BIND_FUN(logerr_func)>(*this, lib, "logerr", das::SideEffects::modifyExternal, "bind_dascript::logerr_func")
      ->annotations.push_back(annotation_declaration(das::make_smart<LogFmtHashFunctionAnnotation>()));
    das::addExtern<DAS_BIND_FUN(logerr_func_hash)>(*this, lib, "logerr", das::SideEffects::modifyExternal,
      "bind_dascript::logerr_func_hash");
    das::addExtern<DAS_BIND_FUN(logwarn_func)>(*this, lib, "logwarn", das::SideEffects::modifyExternal, "bind_dascript::logwarn_func");
    das::addExtern<DAS_BIND_FUN(sleep_msec)>(*this, lib, "sleep_msec", das::SideEffects::modifyExternal, "sleep_msec");

#if _TARGET_PC_LINUX
    das::addExtern<DAS_BIND_FUN(get_memory_allocated_kb_linux)>(*this, lib, "get_memory_allocated_kb",
      das::SideEffects::accessExternal, "bind_dascript::get_memory_allocated_kb_linux");
#else
    das::addExtern<DAS_BIND_FUN(dagor_memory_stat::get_memory_allocated_kb)>(*this, lib, "get_memory_allocated_kb",
      das::SideEffects::accessExternal, "dagor_memory_stat::get_memory_allocated_kb");
#endif

    das::addExtern<DAS_BIND_FUN(memoryreport::get_device_vram_used_kb)>(*this, lib, "get_device_vram_used_kb",
      das::SideEffects::accessExternal, "memoryreport::get_device_vram_used_kb");
    das::addExtern<DAS_BIND_FUN(memoryreport::get_shared_vram_used_kb)>(*this, lib, "get_shared_vram_used_kb",
      das::SideEffects::accessExternal, "memoryreport::get_shared_vram_used_kb");
    das::addExtern<DAS_BIND_FUN(get_platform_string_id)>(*this, lib, "get_platform_string_id", das::SideEffects::modifyExternal,
      "get_platform_string_id"); // although platform doesn't change in runtime, but AoT will generate one code
    das::addExtern<DAS_BIND_FUN(get_console_model)>(*this, lib, "get_console_model", das::SideEffects::modifyExternal,
      "get_console_model"); // although platform doesn't change in runtime, but AoT will generate one code
    das::addExtern<DAS_BIND_FUN(get_console_model_revision)>(*this, lib, "get_console_model_revision",
      das::SideEffects::modifyExternal,
      "get_console_model_revision"); // although platform doesn't change in runtime, but AoT will generate one code
    das::addExtern<DAS_BIND_FUN(get_dagor_frame_no)>(*this, lib, "get_dagor_frame_no", das::SideEffects::accessExternal,
      "bind_dascript::get_dagor_frame_no");
    das::addExtern<DAS_BIND_FUN(get_dagor_game_act_time)>(*this, lib, "get_dagor_game_act_time", das::SideEffects::accessExternal,
      "bind_dascript::get_dagor_game_act_time");
    das::addExtern<DAS_BIND_FUN(clipboard::set_clipboard_ansi_text)>(*this, lib, "set_clipboard_ansi_text",
      das::SideEffects::accessExternal, "clipboard::set_clipboard_ansi_text");
    das::addExtern<DAS_BIND_FUN(profile_ref_ticks)>(*this, lib, "profile_ref_ticks", das::SideEffects::accessExternal,
      "::profile_ref_ticks");
    das::addExtern<DAS_BIND_FUN(profile_usec_from_ticks_delta)>(*this, lib, "profile_usec_from_ticks_delta",
      das::SideEffects::accessExternal, "::profile_usec_from_ticks_delta");
    das::addExtern<DAS_BIND_FUN(profile_usec_passed)>(*this, lib, "profile_usec_passed", das::SideEffects::accessExternal,
      "::profile_usec_passed");
    das::addExtern<DAS_BIND_FUN(profile_time_usec)>(*this, lib, "profile_time_usec", das::SideEffects::accessExternal,
      "::profile_time_usec");
    das::addExtern<DAS_BIND_FUN(get_DAGOR_DBGLEVEL)>(*this, lib, "get_DAGOR_DBGLEVEL", das::SideEffects::accessExternal,
      "bind_dascript::get_DAGOR_DBGLEVEL");
    das::addExtern<DAS_BIND_FUN(get_DAGOR_ADDRESS_SANITIZER)>(*this, lib, "get_DAGOR_ADDRESS_SANITIZER",
      das::SideEffects::accessExternal, "bind_dascript::get_DAGOR_ADDRESS_SANITIZER");
    das::addExtern<DAS_BIND_FUN(get_DAGOR_THREAD_SANITIZER)>(*this, lib, "get_DAGOR_THREAD_SANITIZER",
      das::SideEffects::accessExternal, "bind_dascript::get_DAGOR_THREAD_SANITIZER");
    das::addExtern<DAS_BIND_FUN(get_ARCH_BITS)>(*this, lib, "get_ARCH_BITS", das::SideEffects::accessExternal,
      "bind_dascript::get_ARCH_BITS");
    das::addExtern<DAS_BIND_FUN(get_DAECS_EXTENSIVE_CHECKS)>(*this, lib, "get_DAECS_EXTENSIVE_CHECKS",
      das::SideEffects::accessExternal, "bind_dascript::get_DAECS_EXTENSIVE_CHECKS");
    das::addExternTempRef<DAS_BIND_FUN(dgs_get_settings)>(*this, lib, "dgs_get_settings", das::SideEffects::accessExternal,
      "bind_dascript::dgs_get_settings");

    verifyAotReady();
  }

  virtual das::ModuleAotType aotRequire(das::TextWriter &tw) const override
  {
    tw << "#include <dasModules/dasSystem.h>\n";
    tw << "#include <perfMon/dag_perfTimer.h>\n";
    tw << "#include <memory/dag_memStat.h>\n";
    tw << "#include <perfMon/dag_memoryReport.h>\n";
    return das::ModuleAotType::cpp;
  }
};

} // namespace bind_dascript

REGISTER_MODULE_IN_NAMESPACE(DagorSystem, bind_dascript)
