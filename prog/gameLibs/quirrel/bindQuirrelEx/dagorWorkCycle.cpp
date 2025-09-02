// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <bindQuirrelEx/bindQuirrelEx.h>
#include <sqModules/sqModules.h>
#include <util/dag_delayedAction.h>
#include <perfMon/dag_cpuFreq.h>
#include <perfMon/dag_statDrv.h>
#include <osApiWrappers/dag_miscApi.h>
#include <memory/dag_framemem.h>

#include <sqrat.h>
#include <EASTL/vector_map.h>
#include <EASTL/string.h>
#include <EASTL/vector_multimap.h>
#include <generic/dag_relocatableFixedVector.h>

/// @module dagor.workcycle
namespace bindquirrel
{


static bool are_sq_obj_equal(HSQUIRRELVM vm, const HSQOBJECT &a, const HSQOBJECT &b) { return sq_direct_is_equal(vm, &a, &b); }


// Note using framemem allocator may for unknown reason lead to crash
// during vector destruction on vm_update() exit.
// Further investigation is needed.
using TimerCallbackQueue = dag::RelocatableFixedVector<Sqrat::Function, 8, true /*, framemem_allocator*/>;

struct Timer
{
  float period = 0;
  float timeout = 0;

  Sqrat::Object id;
  Sqrat::Function handler;

  void initOneShot(float timeout_, const Sqrat::Function &handler_)
  {
    period = 0;
    timeout = timeout_;
    handler = handler_;
  }

  void initPeriodic(float period_, const Sqrat::Function &handler_)
  {
    timeout = period = period_;
    handler = handler_;
  }

  bool isLooped() const { return period > 0; }

  bool isFinished() const { return !isLooped() && (timeout <= 0.0f); }

  void update(float dt, TimerCallbackQueue &cb_queue)
  {
    timeout -= dt;
    if (timeout <= 0.0f)
    {
      G_ASSERT(!handler.IsNull());
      if (!handler.IsNull())
        cb_queue.push_back(handler);

      if (isLooped())
        timeout = period;
    }
  }
};


struct VmData
{
  VmData() = default;
  VmData(const VmData &) = default;
  VmData &operator=(const VmData &) = default;
  VmData(VmData &&) = default;
  VmData &operator=(VmData &&) = default;

  ~VmData()
  {
    if (!name.empty()) // name is empty for the move operation source
    {
      debug("workcycle VmData::dtor(%p), name = '%s', %d deferred calls, %d timers", this, name.c_str(), int(deferredCalls.size()),
        int(timers.size()));
    }
  }

  eastl::string name;
  bool autoUpdateFromIdleCycle = false;

  eastl::vector<Sqrat::Function> deferredCalls;
  eastl::vector<Timer> timers;
  int lastUpdateTimestamp = 0;
};

static eastl::vector_map<HSQUIRRELVM, VmData> vms;

static void perform_cycle_actions(void *);
static bool perform_cycle_actions_registered = false;
static void register_perform_cycle_actions_once()
{
  if (!perform_cycle_actions_registered)
  {
    register_regular_action_to_idle_cycle(perform_cycle_actions, nullptr);
    perform_cycle_actions_registered = true;
  }
}


static SQInteger defer(HSQUIRRELVM vm)
{
  SQInteger nparams = 0, nfreevars = 0;
  if (SQ_FAILED(sq_getclosureinfo(vm, 2, &nparams, &nfreevars)))
    return SQ_ERROR;

  if (nparams > 1)
    return sq_throwerror(vm, "Deferred function must not require arguments");

  register_perform_cycle_actions_once();

  auto it = vms.find(vm);
  if (it == vms.end())
    return sq_throwerror(vm, "VM is not registered");

  VmData &v = it->second;

  Sqrat::Var<Sqrat::Object> func(vm, 2);

  v.deferredCalls.push_back(Sqrat::Function(vm, Sqrat::Object(vm), func.value));

  return 0;
}


static SQInteger deferOnce(HSQUIRRELVM vm)
{
  SQInteger nparams = 0, nfreevars = 0;
  if (SQ_FAILED(sq_getclosureinfo(vm, 2, &nparams, &nfreevars)))
    return SQ_ERROR;

  if (nparams > 1)
    return sq_throwerror(vm, "Deferred function must not require arguments");

  register_perform_cycle_actions_once();

  auto it = vms.find(vm);
  if (it == vms.end())
    return sq_throwerror(vm, "VM is not registered");

  VmData &v = it->second;

  Sqrat::Var<Sqrat::Object> func(vm, 2);

  auto cmp = [&func](const Sqrat::Function &x) { return are_sq_obj_equal(x.GetVM(), x.GetFunc(), func.value.GetObject()); };

  if (eastl::find_if(v.deferredCalls.begin(), v.deferredCalls.end(), cmp) != v.deferredCalls.end())
    return 0;

  v.deferredCalls.push_back(Sqrat::Function(vm, Sqrat::Object(vm), func.value));

  return 0;
}


static SQInteger set_timer(HSQUIRRELVM vm, bool periodic, bool reuse)
{
  register_perform_cycle_actions_once();

  auto it = vms.find(vm);
  if (it == vms.end())
    return sq_throwerror(vm, "VM is not registered");

  VmData &v = it->second;

  float dt;
  sq_getfloat(vm, 2, &dt);
  Sqrat::Var<Sqrat::Function> handler(vm, 3);
  Sqrat::Object id;
  if (sq_gettop(vm) > 3)
  {
    id = Sqrat::Var<Sqrat::Object>(vm, 4).value;
    if (id.IsNull())
      return sq_throwerror(vm, "Cannot use null as timer id");
  }
  else
    id = Sqrat::Object(handler.value.GetFunc(), vm);

  Timer *existing = nullptr;
  for (Timer &t : v.timers)
  {
    if (are_sq_obj_equal(vm, t.id, id))
    {
      if (reuse)
      {
        existing = &t;
        break;
      }
      else
        return sq_throwerror(vm, "Duplicate timer id");
    }
  }


  Timer &t = existing ? *existing : v.timers.push_back();
  t.id = id;
  if (periodic)
    t.initPeriodic(dt, handler.value);
  else
    t.initOneShot(dt, handler.value);
  sq_pushobject(vm, id);
  return 1;
}


static void clear_timer(const Sqrat::Object &id_)
{
  const HSQOBJECT &id = id_.GetObject();
  HSQUIRRELVM vm = id_.GetVM();

  auto it = vms.find(vm);
  if (it == vms.end())
    return;

  VmData &v = it->second;

  for (int i = v.timers.size() - 1; i >= 0; --i)
  {
    Timer &t = v.timers[i];
    if (are_sq_obj_equal(vm, t.id, id))
    {
      v.timers.erase(v.timers.begin() + i);
      break; // timer ids are unique
    }
  }
}


static void do_deferred_calls(VmData &v)
{
  if (v.deferredCalls.empty())
    return;

  eastl::vector<Sqrat::Function> functions;
  functions.swap(v.deferredCalls);
  for (Sqrat::Function &f : functions)
    f.Execute();
}


static void update_timers(VmData &v)
{
  int curTime = get_time_msec();

  if (v.lastUpdateTimestamp != 0 && !v.timers.empty())
  {
    float dt = (curTime - v.lastUpdateTimestamp) * 1e-3f;
    TimerCallbackQueue cbQueue;

    for (Timer &t : v.timers)
      t.update(dt, cbQueue);

    for (int i = v.timers.size() - 1; i >= 0; --i)
      if (v.timers[i].isFinished())
        v.timers.erase(v.timers.begin() + i);

    for (Sqrat::Function &f : cbQueue)
      f.Execute();
  }

  v.lastUpdateTimestamp = curTime;
}


static void vm_update(VmData &v)
{
  do_deferred_calls(v);
  update_timers(v);
}


void dagor_workcycle_frame_update(HSQUIRRELVM vm)
{
  auto it = vms.find(vm);
  if (it != vms.end())
    vm_update(it->second);
}


void dagor_workcycle_skip_update(HSQUIRRELVM vm)
{
  auto it = vms.find(vm);
  if (it != vms.end())
    it->second.lastUpdateTimestamp = get_time_msec();
}


static void perform_cycle_actions(void *)
{
  TIME_PROFILE(sq_cycle_actions);

  for (auto it = vms.begin(); it != vms.end(); ++it)
    if (it->second.autoUpdateFromIdleCycle)
      vm_update(it->second);
}


void bind_dagor_workcycle(SqModules *module_mgr, bool auto_update_from_idle_cycle, const char *vm_name)
{
  HSQUIRRELVM vm = module_mgr->getVM();
  Sqrat::Table nsTbl(vm);
  nsTbl //
    .SquirrelFunc("defer", defer, 2, ".c")
    .SquirrelFunc("deferOnce", deferOnce, 2, ".c")
    .SquirrelFunc(
      "setTimeout", [](HSQUIRRELVM vm) { return set_timer(vm, false, false); }, -3, ".nc")
    /* qdox
    @function setTimeout set timer with function to be called on time. if called more than once with same id would call logerr

    @param time_in_seconds n : time in seconds on which function would be called
    @param func c : function to be called on time
    @param id . null : optional id of timer. If not provided closure 'func' is used as id
    */
    .SquirrelFunc(
      "resetTimeout", [](HSQUIRRELVM vm) { return set_timer(vm, false, true); }, -3, ".nc")
    /* qdox
    @function resetTimeout reset timer with function to be called on time. Will replace timer with the same id.

    @param time_in_seconds n : time in seconds on which function would be called
    @param func c : function to be called on time
    @param id . null : optional id of timer. If not provided closure 'func' is used as id
    */
    .SquirrelFunc(
      "setInterval", [](HSQUIRRELVM vm) { return set_timer(vm, true, false); }, -3, ".nc")
    /* qdox
    @function setInterval will set periodical timer with function to be called each period of time.

    @param period_in_seconds n : period of time in seconds on which function would be called
    @param func c : function to be called on time
    @param id . null : optional id of timer. If not provided closure 'func' is used as id
    */
    .Func("clearTimer", clear_timer)
    /**/;

  module_mgr->addNativeModule("dagor.workcycle", nsTbl);

  eastl::string vmNameStr((vm_name && *vm_name) ? vm_name : "<unknown>");
  run_action_on_main_thread([vm, au = auto_update_from_idle_cycle, name = vmNameStr] {
    VmData &v = vms[vm];
    v.autoUpdateFromIdleCycle = au;
    v.name = name;
  });
}

void clear_workcycle_actions(HSQUIRRELVM vm)
{
  auto it = vms.find(vm);
  if (it != vms.end())
  {
    it->second.timers.clear();
    it->second.deferredCalls.clear();
  }
}

void cleanup_dagor_workcycle_module(HSQUIRRELVM vm) { vms.erase(vm); }

} // namespace bindquirrel
