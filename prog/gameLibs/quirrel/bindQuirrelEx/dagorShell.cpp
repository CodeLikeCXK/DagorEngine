// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <quirrel/sqModules/sqModules.h>
#include <quirrel/bindQuirrelEx/bindQuirrelEx.h>
#include <osApiWrappers/dag_shellExecute.h>

static SQInteger shell_execute(HSQUIRRELVM vm)
{
  bool force_sync = true;
  bool hidden = false;
  eastl::string cmd = "open";
  eastl::string file = "";
  eastl::string dir = "";
  eastl::string params = "";

  Sqrat::Table args = Sqrat::Var<Sqrat::Table>(vm, 2).value;
  force_sync = args.RawGetSlotValue<bool>("force_sync", force_sync);
  hidden = args.RawGetSlotValue<bool>("hidden", hidden);
  cmd = args.RawGetSlotValue<eastl::string>("cmd", cmd);
  file = args.RawGetSlotValue<eastl::string>("file", file);
  params = args.RawGetSlotValue<eastl::string>("params", params);
  dir = args.RawGetSlotValue<eastl::string>("dir", dir);

  if (cmd.empty())
    return sq_throwerror(vm, "cmd should be set!");

  if (file.empty() && dir.empty())
    return sq_throwerror(vm, "file or/and dir should be set!");

  os_shell_execute(cmd.c_str(), file.c_str(), params.c_str(), dir.c_str(), force_sync,
    hidden ? OpenConsoleMode::Hide : OpenConsoleMode::Show);

  return 0;
}

namespace bindquirrel
{

void register_dagor_shell(SqModules *module_mgr)
{
  HSQUIRRELVM vm = module_mgr->getVM();
  Sqrat::Table exports(vm);
  ///@module dagor.shell
  exports.SquirrelFunc("shell_execute", shell_execute, 2, ".t");
  /* qdox
  @param params t : table to get result
  @code params sq
    {cmd:string, file:string, dir:string, params:string, force_sync:boolean, hidden:boolean}
  code@
  file or dir should be set. params is optional
  */
  module_mgr->addNativeModule("dagor.shell", exports);
}

} // namespace bindquirrel