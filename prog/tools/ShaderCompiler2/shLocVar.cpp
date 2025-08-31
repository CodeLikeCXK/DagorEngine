// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "shLocVar.h"
#include "shTargetContext.h"
#include <generic/dag_tabUtils.h>
#include <util/dag_globDef.h>

// using namespace ShaderParser;

/*********************************
 *
 * class LocalVarTable
 *
 *********************************/
// ctor/dtor
LocalVarTable::LocalVarTable(shc::TargetContext &a_ctx) : varTable(midmem), ctx{a_ctx} { addBuiltinConstants(); }


LocalVarTable::~LocalVarTable() { clear(); }

void LocalVarTable::addBuiltinConstants()
{
  int name_id = ctx.varNameMap().addVarId("PI");
  LocalVar pi(name_id, shexpr::VT_REAL, false);
  pi.isConst = true;
  pi.cv.r = M_PI;
  addVariable(pi);
}


// clear all table
void LocalVarTable::clear()
{
  clear_and_shrink(varTable);
  addBuiltinConstants();
}


// add new local variable (return variable ID)
int LocalVarTable::addVariable(const LocalVar &var)
{
  if (getVariableId(var.varNameId) != -1)
    return -1;

  varTable.push_back(var);
  return varTable.size() - 1;
}


// return local variable ID by name (-1, if failed)
int LocalVarTable::getVariableId(int var_name_id) const
{
  if (var_name_id != -1)
  {
    for (int i = 0; i < varTable.size(); i++)
    {
      if (varTable[i].varNameId == var_name_id)
        return i;
    }
  }

  return -1;
}


// return local variable ID by ptr (-1, if failed)
// int LocalVarTable::getVariableId(LocalVar* var)
//{
//  return var ? tabutils::getIndex(varTable, var) : -1;
//}


// create copy
void LocalVarTable::copyFrom(const LocalVarTable &c)
{
  clear();
  append_items(varTable, c.varTable.size(), c.varTable.data());
}

// class LocalVarTable
//
