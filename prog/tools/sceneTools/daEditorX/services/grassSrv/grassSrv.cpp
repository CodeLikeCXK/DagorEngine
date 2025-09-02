// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <de3_grassSrv.h>
#include <de3_interface.h>
#include <oldEditor/de_interface.h>

#include <math/dag_mathUtils.h>
#include <ioSys/dag_dataBlockUtils.h>
#include <debug/dag_debug.h>
#include <3d/dag_render.h>
#include <render/dag_cur_view.h>
#include <shaders/dag_shaders.h>
#include <EditorCore/ec_wndGlobal.h>
#include <oldEditor/de_util.h>
#include <drv/3d/dag_matricesAndPerspective.h>
#include <drv/3d/dag_driver.h>
#include <drv/3d/dag_info.h>

#include <landMesh/lmeshRenderer.h>
#include <EASTL/unique_ptr.h>
#include <landMesh/landClass.h>

namespace rendinst::gen
{
extern float custom_max_trace_distance;
extern bool custom_trace_ray(const Point3 &src, const Point3 &dir, real &dist, Point3 *out_norm = NULL);
extern bool custom_get_height(Point3 &pos, Point3 *out_norm = NULL);
extern vec3f custom_update_pregen_pos_y(vec4f pos, int16_t *dest_packed_y, float csz_y, float oy);
extern void custom_get_land_min_max(BBox2 bbox_xz, float &out_min, float &out_max);
} // namespace rendinst::gen

struct RandomGrassRenderHelper : IRandomGrassRenderHelper
{
  int lmeshRenderingMode;
  BBox3 box;
  bool renderRi;
  IRenderingService *hmap;

  RandomGrassRenderHelper();
  ~RandomGrassRenderHelper();
  bool beginRender(const Point3 &center_pos, const BBox3 &box, const TMatrix4 &tm) override;
  void endRender() override;
  void renderHeight(float min_height, float max_height) override;
  void renderColor() override;
  void renderMask() override;
  void renderExplosions() override;
  bool getHeightmapAtPoint(float x, float y, float &out) override;
  bool isValid() const override;
};


RandomGrassRenderHelper::RandomGrassRenderHelper() : hmap(NULL) {}

RandomGrassRenderHelper::~RandomGrassRenderHelper() {}

bool RandomGrassRenderHelper::isValid() const { return hmap != NULL; }

bool RandomGrassRenderHelper::beginRender(const Point3 &center_pos, const BBox3 &box, const TMatrix4 &tm)
{
  hmap = NULL;

  for (int i = 0, plugin_cnt = IEditorCoreEngine::get()->getPluginCount(); i < plugin_cnt; ++i)
  {
    IGenEditorPlugin *p = IEditorCoreEngine::get()->getPlugin(i);
    IRenderingService *iface = p->queryInterface<IRenderingService>();
    if (stricmp(p->getInternalName(), "heightmapLand") == 0)
    {
      hmap = iface;
    }
    else if (strcmp(p->getInternalName(), "_riEntMgr") == 0 || strcmp(p->getInternalName(), "_invalidEntMgr") == 0)
      continue; // skip
  }

  if (!hmap)
    return false;

  this->box = box;

  return true;
}


void RandomGrassRenderHelper::endRender() {}

void RandomGrassRenderHelper::renderColor()
{
  if (!hmap)
    return;

  hmap->renderGeometry(IRenderingService::STG_RENDER_TO_CLIPMAP);
}

void RandomGrassRenderHelper::renderMask()
{
  if (!hmap)
    return;

  hmap->renderGeometry(IRenderingService::STG_RENDER_GRASS_MASK);
}

void RandomGrassRenderHelper::renderHeight(float min_height, float max_height)
{
  if (!hmap)
    return;

  static int heightmap_min_maxVarId = get_shader_variable_id("heightmap_min_max");
  Color4 oldHmap = ShaderGlobal::get_color4_fast(heightmap_min_maxVarId);

  ShaderGlobal::set_color4(heightmap_min_maxVarId, 1.f / (max_height - min_height), -min_height / (max_height - min_height),
    (max_height - min_height), min_height);

  int oldSubDiv = hmap->setSubDiv(0);
  hmap->renderGeometry(IRenderingService::STG_RENDER_HEIGHT_FIELD);
  hmap->setSubDiv(oldSubDiv);

  ShaderGlobal::set_color4(heightmap_min_maxVarId, oldHmap);

  // hmap->setRenderInBBox(BBox3());
}

void RandomGrassRenderHelper::renderExplosions() {}


bool RandomGrassRenderHelper::getHeightmapAtPoint(float x, float y, float &out)
{
  Point3 pos(x, 0, y);
  if (rendinst::gen::custom_get_height(pos))
  {
    out = pos.y;
    return true;
  }
  return false;
}


class GrassService : public IGrassService
{
  eastl::unique_ptr<EditorGrass> randomGrass;
  RandomGrassRenderHelper grassHelper;
  bool force_update;
  bool grassEnabled;

  int grassRenderIteration;

public:
  bool srvDisabled;

  GrassService() : force_update(true), grassEnabled(false), grassRenderIteration(0) { srvDisabled = false; }

  const char *getResName(TEXTUREID id) const override { return ::get_managed_res_name(id); }

  void resetLayersVB() override { randomGrass->resetLayersVB(); }

  void resetGrassMask(LandMeshRenderer &landMeshRenderer, int index, const DataBlock &grassBlk, const String &colorMapId,
    const String &maskName) const override
  {
    Tab<LandClassDetailTextures> &landClasses = landMeshRenderer.getLandClasses();
    if (landClasses[index].grassMaskTexId != BAD_TEXTUREID)
    {
      ::release_managed_tex(landClasses[index].grassMaskTexId);
      landClasses[index].grassMaskTexId = BAD_TEXTUREID;
    }
    landClasses[index].resetGrassMask(grassBlk, colorMapId, maskName.c_str());
    landMeshRenderer.reloadGrassMaskTex(index, landClasses[index].grassMaskTexId);
  }

  bool initSrv()
  {
    Ptr<ShaderMaterial> mat = new_shader_material_by_name("random_grass_filter");
    if (!mat.get())
    {
      DAEDITOR3.conError("GrassService disabled: shader '%s' not found", "random_grass_filter");
      return false;
    }
    init();
    return true;
  }

  void setWaterLevel()
  {
    static int water_level_vid = get_shader_variable_id("water_level", true);
    float waterLevel = ShaderGlobal::get_real_fast(water_level_vid);
    randomGrass->setWaterLevel(waterLevel);
  }

  void init() override {}

  void create_grass(DataBlock &grassBlk) override
  {
    if (d3d::is_stub_driver())
      return;

    randomGrass.reset(); // Resetting it first is needed to prevent ref. count assert.
    randomGrass = eastl::make_unique<EditorGrass>(grassBlk, *grassBlk.getBlockByNameEx("GrassSettingsPlanes"));
    randomGrass->isDissolve = true;
    setWaterLevel();
    grassRenderIteration = 0;
  }

  DataBlock *create_default_grass() override
  {
    if (d3d::is_stub_driver())
    {
      DataBlock *grassBlk = new DataBlock();
      return grassBlk;
    }

    // default grass
    DataBlock defaultGrassBlk;
    String blkName = ::make_full_path(sgg::get_exe_path_full(), "../commonData/_default_grass.blk");
    if (!defaultGrassBlk.load(blkName.str()))
    {
      DAEDITOR3.conError("grass loading error, please check \"%s\" file", blkName.str());
      defaultGrassBlk.clearData();
    }

    DataBlock *grassBlk = new DataBlock(*defaultGrassBlk.getBlockByNameEx("randomGrass"));
    randomGrass.reset(); // Resetting it first is needed to prevent ref. count assert.
    randomGrass = eastl::make_unique<EditorGrass>(*grassBlk, *grassBlk->getBlockByNameEx("GrassSettingsPlanes"));
    randomGrass->isDissolve = true;

    setWaterLevel();
    grassRenderIteration = 0;
    return grassBlk;
  }

  void enableGrass(bool flag) override { grassEnabled = flag; }

  void beforeRender(Stage stage) override
  {
    if (!randomGrass || !grassEnabled)
      return;

    Point3 pointOfInterest = ::grs_cur_view.pos;

    const BBox2 bbox2 = randomGrass->getRenderBbox();
    const int height = 3000;
    Point3 viewDir = ::grs_cur_view.itm.getcol(2);
    TMatrix viewTm;
    TMatrix4 projTm, globTm;
    d3d::gettm(TM_VIEW, viewTm);
    d3d::gettm(TM_PROJ, &projTm);
    d3d::calcglobtm(viewTm, projTm, globTm);
    BBox3 bbox3(Point3::xVy(bbox2[0], -height), Point3::xVy(bbox2[1], height));
    grassHelper.beginRender(pointOfInterest, bbox3, {});
    randomGrass->beforeRender(pointOfInterest, grassHelper, viewDir, viewTm, projTm, Frustum(globTm), force_update);

    force_update = false;

    // bugfix: grass dont receives color from clipmap
    // refresh grass after few iterations - solves problem
    if (grassRenderIteration == 5)
      force_update = true;
    grassRenderIteration++;
  }

  void renderGeometry(Stage stage) override
  {
    if (!randomGrass || !grassEnabled)
      return;

    randomGrass->renderOpaque();
  }

  BBox3 *getGrassBbox() override { return &grassHelper.box; }

  int addDefaultLayer() override { return randomGrass->addDefaultLayer(); }

  bool removeLayer(int layer_i) override { return randomGrass->removeLayer(layer_i); }

  void reloadAll(const DataBlock &grass_blk, const DataBlock &params_blk) override { randomGrass->reload(grass_blk, params_blk); }

  bool changeLayerResource(int layer_i, const char *resName) override { return randomGrass->changeLayerResource(layer_i, resName); }

  int getGrassLayersCount() override { return randomGrass->getGrassLayersCount(); }

  GrassLayerInfo *getLayerInfo(int layer_i) override
  {
    if (!randomGrass)
      return NULL;

    return randomGrass->getGrassLayer(layer_i);
  }

  void setLayerDensity(int layer_i, float new_density) override { return randomGrass->setLayerDensity(layer_i, new_density); }

  void updateLayerVbo(int layer_i) override { randomGrass->updateLayerVbo(layer_i); }

  void forceUpdate() override { force_update = true; }
};


static GrassService srv;

void setup_grass_service(const DataBlock &app_blk)
{
  srv.srvDisabled = app_blk.getBlockByNameEx("projectDefaults")->getBool("disableGrass", false);
}
void *get_generic_grass_service()
{
  if (srv.srvDisabled)
    return NULL;

  static bool is_inited = false;
  if (!is_inited)
  {
    is_inited = true;
    if (!srv.initSrv())
    {
      srv.srvDisabled = true;
      return NULL;
    }
  }
  return &srv;
}
