//
// Dagor Tech 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <math/dag_geomTree.h>
#include <generic/dag_ptrTab.h>
#include <math/dag_Point3.h>
#include <util/dag_simpleString.h>

class DataBlock;
class FastPhysSystem;
class Matrix3;
namespace mkbindump
{
class BinDumpSaveCB;
}

class FpdAction;
class FpdObject;
class FpdPoint;
class FpdBone;
class FpdClipper;
class FpdContainerAction;
class FpdExporter;


template <class T>
inline T *rtti_cast(DObject *o)
{
  return (o && o->isSubOf(T::HUID)) ? (T *)o : NULL;
}


class IFpdLoad
{
public:
  virtual FpdAction *loadAction(const DataBlock &blk) = 0;
  virtual GeomNodeTree &getGeomTree() = 0;
  virtual FpdObject *getEdObjectByName(const char *name) = 0;

  static FpdAction *load_action(const DataBlock &blk, IFpdLoad &loader);
  static FpdObject *create_object_by_class(const char *cn);
};


class IFpdExport
{
public:
  virtual void getPointGroup(int grp_id, int &first_pt, int &num_pts) = 0;
  virtual void countAction() = 0;
  virtual FpdBone *getBoneByName(const char *name) = 0;
  virtual FpdPoint *getPointByName(const char *name) = 0;
  virtual int getPointIndex(FpdObject *point_object) = 0;
  virtual const char *getNodeName(dag::Index16 node) const = 0;
};


class FpdExporter : public IFpdExport, public IFpdLoad
{
public:
  PtrTab<FpdObject> objects;
  PtrTab<FpdPoint> points;
  Ptr<FpdAction> initAction, updateAction;
  GeomNodeTree nodeTree;
  int actionCounter = 0;

public:
  FpdExporter();
  ~FpdExporter();

  bool load(const DataBlock &blk);
  bool exportFastPhys(mkbindump::BinDumpSaveCB &cwr);

  // IFpdExport implementation
  void getPointGroup(int grp_id, int &first_pt, int &num_pts) override;
  void countAction() override { actionCounter++; }
  FpdBone *getBoneByName(const char *name) override;
  FpdPoint *getPointByName(const char *name) override;
  int getPointIndex(FpdObject *po) override;
  const char *getNodeName(dag::Index16 node) const override { return nodeTree.getNodeName(node); }

  // IFpdLoad implementation
  FpdAction *loadAction(const DataBlock &blk) override { return load_action(blk, *this); }
  GeomNodeTree &getGeomTree() override { return nodeTree; }
  FpdObject *getEdObjectByName(const char *name) override;

protected:
  void exportAction(mkbindump::BinDumpSaveCB &cwr, FpdAction *action);
  void addObject(FpdObject *o);
};


// Base class for ed-actions.
class FpdAction : public DObject
{
public:
  SimpleString actionName;

public:
  virtual FpdObject *getObject() = 0;
  virtual void save(DataBlock &blk, const GeomNodeTree &tree) = 0;
  virtual bool load(const DataBlock &blk, IFpdLoad &loader) = 0;
  virtual void exportAction(mkbindump::BinDumpSaveCB &cwr, IFpdExport &exp) = 0;

  DAG_DECLARE_NEW(midmem)
};


// Base class for container ed-actions.
class FpdContainerAction : public FpdAction
{
public:
  static constexpr unsigned HUID = 0x2BD3B822u; // FpdContainerAction

public:
  FpdContainerAction(const char *name) : subActions(midmem) { actionName = name; }

  FpdObject *getObject() override { return NULL; }
  void save(DataBlock &blk, const GeomNodeTree &tree) override;
  bool load(const DataBlock &blk, IFpdLoad &loader) override;
  void exportAction(mkbindump::BinDumpSaveCB &cwr, IFpdExport &exp) override;

  void insertAction(FpdAction *action, int at = -1);
  const PtrTab<FpdAction> &getSubActions() const { return subActions; }
  void removeActions(int at, int num);

  int getActionIndexByName(const char *name);

  FpdAction *getActionByName(const char *name)
  {
    int id = getActionIndexByName(name);
    if (id < 0 || id >= subActions.size())
      return NULL;
    return subActions[id];
  }

  FpdContainerAction *getContainerByName(const char *name) { return rtti_cast<FpdContainerAction>(getActionByName(name)); }

  bool isSubOf(DClassID id) override { return id == FpdContainerAction::HUID || FpdAction::isSubOf(id); }

protected:
  PtrTab<FpdAction> subActions;
};


// Default integrator action.
class FpdIntegratorAction : public FpdAction
{
public:
  FpdIntegratorAction() { actionName = "integrate"; }

  FpdObject *getObject() override { return NULL; }
  void save(DataBlock &blk, const GeomNodeTree &tree) override;
  bool load(const DataBlock &blk, IFpdLoad &loader) override;
  void exportAction(mkbindump::BinDumpSaveCB &cwr, IFpdExport &exp) override;
};


class FpdObject : public DObject
{
public:
  static constexpr unsigned HUID = 0x0F79300Cu; // FpdObject

public:
  virtual const char *getName() const { return name; }
  virtual bool setName(const char *nm)
  {
    name = nm;
    return true;
  }
  virtual Point3 getPos() const { return pos; }
  virtual bool setPos(const Point3 &p)
  {
    pos = p;
    return true;
  }
  virtual bool getMatrix(Matrix3 &tm) const { return false; }
  virtual bool setMatrix(const Matrix3 &tm) { return false; }

  virtual void save(DataBlock &blk, const GeomNodeTree &tree) = 0;
  virtual bool load(const DataBlock &blk, IFpdLoad &loader) = 0;

  virtual void initActions(FpdContainerAction *init_a, FpdContainerAction *upd_a, IFpdLoad &ld) = 0;
  virtual void getActions(Tab<FpdAction *> &actions) = 0;

  bool isSubOf(DClassID id) override { return id == FpdObject::HUID || DObject::isSubOf(id); }

protected:
  SimpleString name;
  Point3 pos;
};
