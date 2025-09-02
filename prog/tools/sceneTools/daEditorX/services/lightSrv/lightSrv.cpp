// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <de3_lightService.h>
#include <de3_lightProps.h>
#include <de3_interface.h>
#include <sceneBuilder/nsb_StdTonemapper.h>
#include <libTools/util/colorUtil.h>
#include <libTools/staticGeom/geomObject.h>
#include <shaders/dag_shaderVar.h>
#include <math/dag_mathAng.h>
#include <generic/dag_tab.h>
#include <util/dag_globDef.h>
#include <shaders/dag_shaderBlock.h>

static E3DCOLOR color4ToE3dcolor(Color4 col, float &brightness)
{
  brightness = col.r;
  if (col.g > brightness)
    brightness = col.g;
  if (col.b > brightness)
    brightness = col.b;

  if (brightness <= 0.0)
    return E3DCOLOR(0, 0, 0, 0);

  col *= 255.0 / brightness;
  return E3DCOLOR(col.r, col.g, col.b, col.a);
}

class GenericLightService : public ISceneLightService
{
public:
  GenericLightService() : sun(midmem) { memset(&nullSun, 0, sizeof(nullSun)); }

  void reset() override
  {
    GeomObject::setTonemapper(&tonemap);

    syncLtColors = true;
    syncLtDirs = true;
    writeNumberedSuns = false;

    setDefSkyProps(sky);
    setSunCount(1);
    setDefSunProps(sun[0], 1600);
    tonemap = StaticSceneBuilder::StdTonemapper();

    sunLtDir0GvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("from_sun_direction"));
    sunCol0GvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sun_color_0"));
    sunLtDir1GvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sun_light_dir_1"));
    sunCol1GvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sun_color_1"));
    skyColGvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sky_color"));
    if (skyColGvId == -1)
      skyColGvId = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("ambient_sph_color"));

    sunColLtmapGvid = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sun_color_lightmap"));
    sunDirLtmapGvid = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("sun_direction_lightmap"));

    dlGvId.lightDir = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("light_dir"));

    dlGvId.lightSpecularColor = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("light_specular_color"));

    dlGvId.worldLt.dir0 = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("world_lt_dir_0"));
    dlGvId.worldLt.col0 = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("world_lt_color_0"));
    dlGvId.worldLt.dir1 = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("world_lt_dir_1"));
    dlGvId.worldLt.col1 = ShaderGlobal::get_glob_var_id(VariableMap::getVariableId("world_lt_color_1"));
  }

  void setSunCount(int sun_count) override
  {
    if (sun_count > sun.size())
    {
      int i = sun.size();
      sun.resize(sun_count);
      while (i < sun_count)
      {
        setDefSunProps(sun[i], i == 0 ? 1600 : 1);
        i++;
      }
    }
    else if (sun_count < sun.size())
      sun.resize(sun_count);
  }
  int getSunCount() const override { return sun.size(); }

  SkyLightProps &getSky() override { return sky; }
  SunLightProps *getSun(int sun_idx) override { return sun_idx < sun.size() ? &sun[sun_idx] : NULL; }
  const SunLightProps &getSunEx(int idx) override { return idx < sun.size() ? sun[idx] : nullSun; }

  void setSyncLtColors(bool sync) override { syncLtColors = sync; }
  void setSyncLtDirs(bool sync) override { syncLtDirs = sync; }
  bool getSyncLtColors() override { return syncLtColors; }
  bool getSyncLtDirs() override { return syncLtDirs; }
  bool updateDirLightFromSunSky() override
  {
    if (!syncLtDirs && !syncLtColors)
      return false;

    Color3 sky0, sun1, sun2;
    Point3 dir1, dir2;

    getDirectLight(dir1, sun1, dir2, sun2, sky0, false);

    if (!syncLtDirs)
    {
      dir1 = getSunEx(0).ltDir;
      dir2 = getSunEx(1).ltDir;
    }

    if (!syncLtColors)
    {
      sun1 = getSunEx(0).ltCol;
      sun2 = getSunEx(1).ltCol;
      sky0 = sky.ambCol;
    }

    bool changed = change_color3(sky.ambCol, sky0);
    if (sun.size() > 0)
      changed |= change_point3(sun[0].ltDir, dir1) | change_color3(sun[0].ltCol, sun1);
    if (sun.size() > 1)
      changed |= change_point3(sun[1].ltDir, dir2) | change_color3(sun[1].ltCol, sun2);
    return changed;
  }
  void setupSkyLighting()
  {
    static int normSkyColorVarId = ::get_shader_glob_var_id("norm_sky_color", true);
    static int normSunColor0VarId = ::get_shader_glob_var_id("norm_sun_color_0", true);
    static int normSunColor1VarId = ::get_shader_glob_var_id("norm_sun_color_1", true);
    static int skyColorScaleVarId = ::get_shader_glob_var_id("sky_color_scale", true);

    Color3 skyColor = sky.ambCol;
    Color3 sunColor0 = sun.size() > 0 ? sun[0].ltCol : Color3(0, 0, 0);
    Color3 sunColor1 = sun.size() > 1 ? sun[1].ltCol : Color3(0, 0, 0);

    Color3 normalizedSkyColor = skyColor;
    Color3 normalizedSunColor0 = sunColor0;
    Color3 normalizedSunColor1 = sunColor1;

    float scale = length(normalizedSkyColor) + length(normalizedSunColor0) + length(normalizedSunColor1);

    normalizedSkyColor /= scale;
    normalizedSunColor0 /= scale;
    normalizedSunColor1 /= scale;

    ShaderGlobal::set_color4_fast(normSkyColorVarId, normalizedSkyColor.r, normalizedSkyColor.g, normalizedSkyColor.b, 0.f);
    ShaderGlobal::set_color4_fast(normSunColor0VarId, normalizedSunColor0.r, normalizedSunColor0.g, normalizedSunColor0.b, 0.f);
    ShaderGlobal::set_color4_fast(normSunColor1VarId, normalizedSunColor1.r, normalizedSunColor1.g, normalizedSunColor1.b, 0.f);

    ShaderGlobal::set_color4_fast(skyColorScaleVarId, scale, scale, scale, 0.f);

    ShaderGlobal::set_color4_fast(skyColGvId, skyColor.r, skyColor.g, skyColor.b, 0.f);
    ShaderGlobal::set_color4_fast(sunCol0GvId, sunColor0.r, sunColor0.g, sunColor0.b, 0.f);
    ShaderGlobal::set_color4_fast(sunCol1GvId, sunColor1.r, sunColor1.g, sunColor1.b, 0.f);

    static int global_frame_const_blockid = ShaderGlobal::getBlockId("global_const_block");
    ShaderGlobal::setBlock(global_frame_const_blockid, ShaderGlobal::LAYER_GLOBAL_CONST);
  }

  void updateShaderVars() override
  {
    if (sun.size() > 0)
    {
      ShaderGlobal::set_color4_fast(sunLtDir0GvId, -Color4(sun[0].ltDir.x, sun[0].ltDir.y, sun[0].ltDir.z, 0));

      ShaderGlobal::set_color4_fast(dlGvId.worldLt.dir0, -Color4(sun[0].ltDir.x, sun[0].ltDir.y, sun[0].ltDir.z, 0));
      ShaderGlobal::set_color4_fast(dlGvId.worldLt.col0, ::color4(sun[0].ltCol, 0));

      ShaderGlobal::set_color4_fast(dlGvId.lightDir, -Color4(sun[0].ltDir.x, sun[0].ltDir.y, sun[0].ltDir.z, 0));
      ShaderGlobal::set_color4_fast(dlGvId.lightSpecularColor, Color4(1, 1, 1, 0));

      ShaderGlobal::set_color4_fast(sunDirLtmapGvid, Color4(sun[0].ltDir.x, sun[0].ltDir.y, sun[0].ltDir.z, 0));
      ShaderGlobal::set_color4_fast(sunColLtmapGvid, ::color4(sun[0].ltCol, 0));
    }
    if (sun.size() > 1)
    {
      ShaderGlobal::set_color4_fast(sunLtDir1GvId, -Color4(sun[1].ltDir.x, sun[1].ltDir.y, sun[1].ltDir.z, 0));

      ShaderGlobal::set_color4_fast(dlGvId.worldLt.dir1, -Color4(sun[1].ltDir.x, sun[1].ltDir.y, sun[1].ltDir.z, 0));
      ShaderGlobal::set_color4_fast(dlGvId.worldLt.col1, ::color4(sun[1].ltCol, 0));
    }
    setupSkyLighting();
  }

  bool applyLightingToGeometry() const override { return applySunLighting() || applySkyLighting(); }

  StaticSceneBuilder::StdTonemapper &toneMapper() override { return tonemap; }
  void saveToneMapper(DataBlock &blk) const override { tonemap.save(blk); }
  void loadToneMapper(const DataBlock &blk) override { tonemap.load(blk); }

  // direct lighting
  void getDirectLight(Point3 &sun1_dir, Color3 &sun1_col, Point3 &sun2_dir, Color3 &sun2_col, Color3 &sky_col, bool native) override
  {
    if (native)
    {
      sun1_dir = getSunEx(0).ltDir;
      sun1_col = getSunEx(0).ltCol;

      sun2_dir = getSunEx(1).ltDir;
      sun2_col = getSunEx(1).ltCol;

      sky_col = sky.ambCol;
    }
    else
    {
      const Point3 postMultiply = tonemap.getPostScale();

      Color4 col4 = tonemap.mapColor(::color4(sky.color));
      col4 = Color4(col4.r / postMultiply.x, col4.g / postMultiply.y, col4.b / postMultiply.z);
      sky_col = color3(col4 * sky.brightness / 4);

      if (sun.size() > 0)
      {
        const real initBright = sun[0].brightness * (1 - ::cosf(sun[0].angSize / 2)) * 0.9 / 2;
        col4 = tonemap.mapColor(::color4(sun[0].color) * initBright);
        col4 = Color4(col4.r / postMultiply.x, col4.g / postMultiply.y, col4.b / postMultiply.z);

        sun1_dir = ::sph_ang_to_dir(Point2(sun[0].azimuth, sun[0].zenith));
        sun1_col = color3(col4);
      }
      else
      {
        sun1_dir = Point3(0, -1, 0);
        sun1_col = Color3(0, 0, 0);
      }

      if (sun.size() > 1)
      {
        const real initBright = sun[1].brightness * (1 - ::cosf(sun[1].angSize / 2)) * 0.9 / 2;
        col4 = tonemap.mapColor(::color4(sun[1].color) * initBright);
        col4 = Color4(col4.r / postMultiply.x, col4.g / postMultiply.y, col4.b / postMultiply.z);

        sun2_dir = ::sph_ang_to_dir(Point2(sun[1].azimuth, sun[1].zenith));
        sun2_col = color3(col4);
      }
      else
      {
        sun2_dir = -sun2_dir;
        sun2_col = Color3(0, 0, 0);
      }
    }
  }

  void saveSunsAndSky(DataBlock &blk) const override
  {
    DataBlock *b;
    blk.setBool("sync_dir_light_dir", syncLtDirs);
    blk.setBool("sync_dir_light_color", syncLtColors);

    b = blk.addNewBlock("sky");
    b->setE3dcolor("sky_color", sky.color);
    b->setReal("sky_brightness", sky.brightness);
    b->setE3dcolor("earth_color", sky.earthColor);
    b->setReal("earth_brightness", sky.earthBrightness);
    b->setPoint3("dl_color", Point3::rgb(sky.ambCol));
    b->setE3dcolor("dl_color_user", sky.ambColUser);
    b->setReal("dl_color_mul_user", sky.ambColMulUser);
    b->setBool("enabled", sky.enabled);

    for (int i = 0; i < sun.size(); i++)
    {
      if (writeNumberedSuns)
        b = blk.addNewBlock(String(16, "sun%d", i));
      else
        b = blk.addNewBlock("sun");

      b->setPoint2("azimuthZenith", Point2(sun[i].azimuth, sun[i].zenith));
      b->setReal("angSize", sun[i].angSize);
      b->setE3dcolor("color", sun[i].color);
      b->setReal("brightness", sun[i].brightness);
      b->setPoint3("dl_color", Point3::rgb(sun[i].ltCol));
      b->setE3dcolor("dl_color_user", sun[i].ltColUser);
      b->setReal("dl_color_mul_user", sun[i].ltColMulUser);
      b->setPoint3("dl_dir", normalize(sun[i].ltDir));
      b->setReal("specPower", sun[i].specPower);
      b->setReal("specStrength", sun[i].specStrength);
      b->setBool("enabled", sun[i].enabled);
    }
  }
  void loadSunsAndSky(const DataBlock &blk) override
  {
    const DataBlock *b;
    syncLtDirs = blk.getBool("sync_dir_light_dir", true);
    syncLtColors = blk.getBool("sync_dir_light_color", true);
    writeNumberedSuns = false;

    setDefSkyProps(sky);
    b = blk.getBlockByName("sky");
    if (b)
    {
      Point3 col = b->getPoint3("dl_color", Point3::rgb(sky.ambCol));
      sky.color = b->getE3dcolor("sky_color", sky.color);
      sky.brightness = b->getReal("sky_brightness", sky.brightness);
      sky.earthColor = b->getE3dcolor("earth_color", sky.earthColor);
      sky.earthBrightness = b->getReal("earth_brightness", sky.earthBrightness);
      sky.ambCol = Color3(col.x, col.y, col.z);

      E3DCOLOR defColor;
      float defMul;
      defColor = color4ToE3dcolor(::color4(sky.ambCol, 1.0), defMul);
      sky.ambColUser = b->getE3dcolor("dl_color_user", defColor);
      sky.ambColMulUser = b->getReal("dl_color_mul_user", defMul);

      sky.enabled = b->getBool("enabled", sky.enabled);
    }

    int nid = blk.getNameId("sun");
    sun.clear();
    if (nid >= 0)
      for (int i = 0; i < blk.blockCount(); i++)
        if (blk.getBlock(i)->getBlockNameId() == nid)
          loadSunBlock(blk.getBlock(i));

    if (!sun.size())
    {
      for (int i = 0; i < 16; i++)
      {
        b = blk.getBlockByName(String(16, "sun%d", i));
        if (b)
          loadSunBlock(b);
      }
      if (sun.size())
      {
        DAEDITOR3.conWarning("read suns in obsolete form, from blocks sun0, sun1,...");
        writeNumberedSuns = true;
      }
    }
  }

  void loadSunBlock(const DataBlock *b)
  {
    SunLightProps &s = sun.push_back();

    setDefSunProps(s, sun.size() == 1 ? 1600 : 1);
    Point2 az = b->getPoint2("azimuthZenith", Point2(s.azimuth, s.zenith));
    Point3 col = b->getPoint3("dl_color", Point3::rgb(s.ltCol));

    s.azimuth = az.x;
    s.zenith = az.y;

    s.angSize = b->getReal("angSize", s.angSize);
    s.color = b->getE3dcolor("color", s.color);
    s.brightness = b->getReal("brightness", s.brightness);
    s.ltCol = Color3(col.x, col.y, col.z);

    E3DCOLOR defColor;
    float defMul;
    defColor = color4ToE3dcolor(::color4(s.ltCol, 1.0), defMul);
    s.ltColUser = b->getE3dcolor("dl_color_user", defColor);
    s.ltColMulUser = b->getReal("dl_color_mul_user", defMul);

    s.ltDir = b->getPoint3("dl_dir", normalize(s.ltDir));
    s.specPower = b->getReal("specPower", s.specPower);
    s.specStrength = b->getReal("specStrength", s.specStrength);

    s.enabled = b->getBool("enabled", s.enabled);
  }

protected:
  struct DirLtVarIds
  {
    int dir0 = -1, col0 = -1;
    int dir1 = -1, col1 = -1;
  };

  Tab<SunLightProps> sun;
  SkyLightProps sky;
  SunLightProps nullSun;
  StaticSceneBuilder::StdTonemapper tonemap;
  bool syncLtColors = false, syncLtDirs = false;
  bool writeNumberedSuns = false;

  int sunLtDir0GvId = -1;
  int sunCol0GvId = -1;
  int sunLtDir1GvId = -1;
  int sunCol1GvId = -1;
  int skyColGvId = -1;
  int sunColLtmapGvid = -1;
  int sunDirLtmapGvid = -1;

  struct DynLtVarIds
  {
    DirLtVarIds worldLt;
    int lightDir = -1;
    int lightSpecularColor = -1;
  } dlGvId;

  void setDefSkyProps(SkyLightProps &p)
  {
    p.color = E3DCOLOR(100, 120, 200, 255);
    p.brightness = 1.5;
    p.earthColor = E3DCOLOR(70, 70, 70, 255);
    ;
    p.earthBrightness = 1;
    p.enabled = true;

    const Point3 postMultiply = tonemap.getPostScale();
    Color4 col4 = tonemap.mapColor(::color4(p.color));
    col4 = Color4(col4.r / postMultiply.x, col4.g / postMultiply.y, col4.b / postMultiply.z);
    p.ambCol = color3(col4 * p.brightness / 4);
  }
  void setDefSunProps(SunLightProps &p, float br)
  {
    p.color = E3DCOLOR(255, 255, 242, 255);
    p.brightness = br;
    p.azimuth = 0;
    p.zenith = 40 * DEG_TO_RAD;
    p.angSize = 5 * DEG_TO_RAD;
    p.enabled = true;

    const Point3 postMultiply = tonemap.getPostScale();
    const real initBright = p.brightness * (1 - ::cosf(p.angSize / 2)) * 0.9 / 2;
    Color4 col4 = tonemap.mapColor(::color4(p.color) * initBright);
    col4 = Color4(col4.r / postMultiply.x, col4.g / postMultiply.y, col4.b / postMultiply.z);

    p.ltDir = ::sph_ang_to_dir(Point2(p.azimuth, p.zenith));
    p.ltCol = ::color3(col4);
    p.specPower = 50;
    p.specStrength = 4;
  }

  bool applySunLighting() const
  {
    E3DCOLOR dirCol;
    real dirBright, dirZen, dirAzim;

    if (sun.size() > 0)
    {
      real bright = sun[0].brightness * (1 - ::cosf(sun[0].angSize / 2)) * 0.9 / 2;
      Color4 col4 = ::color4(sun[0].color);

      dirCol = ::normalize_color4(col4, bright);
      dirBright = bright;
      dirZen = sun[0].zenith;
      dirAzim = sun[0].azimuth;
    }
    else
    {
      dirCol = E3DCOLOR(0, 0, 0, 0);
      dirBright = 0;
      dirZen = HALFPI;
      dirAzim = 0;
    }

    E3DCOLOR color;
    real brightness, zenith, azimuth;
    GeomObject::getDirectLight(color, brightness);
    GeomObject::getLightAngles(zenith, azimuth);

    if (color != dirCol || brightness != dirBright || zenith != dirZen || azimuth != dirAzim)
    {
      GeomObject::setDirectLight(dirCol, dirBright);
      GeomObject::setLightAngles(dirZen, dirAzim);

      return true;
    }

    return false;
  }


  bool applySkyLighting() const
  {
    E3DCOLOR ambCol, earthCol;
    real ambBright, earthBright;

    Color4 col4 = ::color4(sky.color);
    real bright = sky.brightness / 4;

    ambCol = ::normalize_color4(col4, bright);
    ambBright = bright;

    col4 = ::color4(sky.earthColor);
    bright = sky.earthBrightness / 4;

    earthCol = ::normalize_color4(col4, bright);
    earthBright = bright;

    E3DCOLOR color, color2;
    real brightness, brightness2;
    GeomObject::getSkyLight(color, brightness);
    GeomObject::getEarthLight(color2, brightness2);

    if (color != ambCol || brightness != ambBright || color2 != earthCol || brightness2 != earthBright)
    {
      GeomObject::setAmbientLight(ambCol, ambBright, earthCol, earthBright);
      return true;
    }

    return false;
  }
};

static GenericLightService srv;

void *get_gen_light_service() { return &srv; }
