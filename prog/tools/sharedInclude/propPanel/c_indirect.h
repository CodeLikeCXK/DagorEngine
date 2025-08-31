//
// Dagor Tech 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

class DataBlock;

namespace PropPanel
{

class ContainerPropertyControl;
class PropPanelScheme;

/// Interface for indirect control managing.
/// setParams(...) will be called for every control
/// created in ContainerPropertyControl::createIndirect(...)
class ISetControlParams
{
public:
  ISetControlParams(ContainerPropertyControl &parent_panel) : mParentPanel(parent_panel) {}
  virtual void setParams(int control_id) = 0;

protected:
  ContainerPropertyControl &mParentPanel;
};


//-----------------------------------------------------------

namespace schemebasedpropertypanel
{
//===============================================================================
//  load/get property panel parameter from DataBlock
//===============================================================================

struct Parameter
{
public:
  Parameter(const DataBlock &blk);
  Parameter(int indent_size);
  ~Parameter();
  void addParam(ContainerPropertyControl *gr, const DataBlock &data);
  bool getParam(const ContainerPropertyControl *gr, DataBlock &data);
  void setTrackBounds(real min_, real max_, real step_)
  {
    min = min_;
    max = max_;
    step = step_;
    type = TYPE_REALTRACK;
  }

private:
  enum
  {
    TYPE_INDENT,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_REAL,
    TYPE_POINT2,
    TYPE_POINT3,
    TYPE_E3DCOLOR,
    TYPE_REALTRACK,
  };

  String name, caption;
  int type;
  real min, max, step;
  union Value
  {
    int indentSize;
    const char *s;
    int i;
    real r;
    bool b;
    float p[3];

    Point2 &p2() { return *(Point2 *)p; } // -V641
    Point3 &p3() { return *(Point3 *)p; }
    E3DCOLOR &c() { return *(E3DCOLOR *)&i; }
  } defVal;

  int getId() const { return (int)(uintptr_t)this; }
};

//===============================================================================
//  load/get property panel group to DataBlock
//===============================================================================

struct ParamGroup
{
public:
  ParamGroup(const char *_name, const char *_caption, const DataBlock &blk);
  ParamGroup(const DataBlock &blk); // create without scheme
  ~ParamGroup();
  void addParams(ContainerPropertyControl *gr, const DataBlock &data);
  bool getParams(const ContainerPropertyControl *gr, DataBlock &data);

private:
  struct Item
  {
    union
    {
      Parameter *p;
      ParamGroup *g;
    };
    bool group;
  };
  Tab<Item> items;
  String name, caption;

  int getId() const { return (int)(uintptr_t)this; }
  friend class PropPanel::PropPanelScheme;
};
} // namespace schemebasedpropertypanel


//===============================================================================
//  scheme to load/get items from DataBlock and property panel
//===============================================================================

class PropPanelScheme
{
public:
  PropPanelScheme() : rootScheme(NULL) {}
  virtual ~PropPanelScheme();

  virtual void load(const DataBlock &scheme_raw, bool make_on_fly = false);
  virtual void load(const String &scheme_name, const DataBlock *data_blk = NULL);
  virtual void setParams(ContainerPropertyControl *gr, const DataBlock &data);
  virtual bool getParams(const ContainerPropertyControl *gr, DataBlock &data);
  virtual void clear();

private:
  schemebasedpropertypanel::ParamGroup *rootScheme;
};

} // namespace PropPanel