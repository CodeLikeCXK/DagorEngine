//
// Dagor Engine 6.5 - Game Libraries
// Precomputed Radiance Transfer Probe Global Illumination (daPRTGI)
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include "prtgi_debug.h"
#include <debug/dag_debug3d.h>
#include <math/dag_e3dColor.h>
#include <gui/dag_stdGuiRender.h>
#include <gui/dag_stdGuiRenderEx.h>
#include <imgui/imgui.h>
#include <shaders/dag_shaders.h>
#include <debug/dag_debug.h>

namespace daprtgi
{

namespace
{
struct SphereTriangle
{
  Point3 v[3];
  Point3 normal;
};

static void draw_solid_debug_sphere(const Point3 &pos, float radius, E3DCOLOR base_color)
{
  static SphereTriangle unitMesh[32];
  static bool meshInited = false;
  if (!meshInited)
  {
    // 6 vertices of an octahedron
    static const Point3 octVerts[6] = {
      Point3( 1,  0,  0), Point3(-1,  0,  0),
      Point3( 0,  1,  0), Point3( 0, -1,  0),
      Point3( 0,  0,  1), Point3( 0,  0, -1)
    };
    // 8 triangular faces of an octahedron
    static const int octFaces[8][3] = {
      {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
      {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}
    };

    int outIdx = 0;
    for (int f = 0; f < 8; ++f)
    {
      Point3 v0 = octVerts[octFaces[f][0]];
      Point3 v1 = octVerts[octFaces[f][1]];
      Point3 v2 = octVerts[octFaces[f][2]];

      Point3 m01 = normalize(v0 + v1);
      Point3 m12 = normalize(v1 + v2);
      Point3 m20 = normalize(v2 + v0);

      Point3 subtri[4][3] = {
        {v0, m01, m20},
        {v1, m12, m01},
        {v2, m20, m12},
        {m01, m12, m20}
      };

      for (int t = 0; t < 4; ++t)
      {
        unitMesh[outIdx].v[0] = subtri[t][0];
        unitMesh[outIdx].v[1] = subtri[t][1];
        unitMesh[outIdx].v[2] = subtri[t][2];
        unitMesh[outIdx].normal = normalize(subtri[t][0] + subtri[t][1] + subtri[t][2]);
        ++outIdx;
      }
    }
    meshInited = true;
  }

  for (int i = 0; i < 32; ++i)
  {
    Point3 p[3] = {
      pos + unitMesh[i].v[0] * radius,
      pos + unitMesh[i].v[1] * radius,
      pos + unitMesh[i].v[2] * radius
    };
    float light = clamp(0.68f + 0.32f * (unitMesh[i].normal.y * 0.7f + unitMesh[i].normal.x * 0.4f - unitMesh[i].normal.z * 0.3f), 0.0f, 1.0f);
    E3DCOLOR col((int)(base_color.r * light), (int)(base_color.g * light), (int)(base_color.b * light), base_color.a);
    draw_cached_debug_solid_triangle(p, col);
  }
}
} // namespace

void PRTGIDebugRenderer::drawWorldDebug(const PRTAsset &asset, const PRTSectorResidency &residency,
                                       DebugVisualMode mode, const Point3 &camera_pos)
{
  if (mode == DebugVisualMode::None)
    return;

  // Draw Sector bounds
  if (mode == DebugVisualMode::Sectors)
  {
    for (const auto &sec : asset.sectors)
    {
      bool isResident = residency.isSectorResident(sec.sectorIndex);
      E3DCOLOR col = isResident ? E3DCOLOR(0, 255, 120, 255) : E3DCOLOR(100, 100, 100, 100);
      draw_debug_box(sec.bounds, col);
    }
    return;
  }

  // Draw Probes
  if (mode == DebugVisualMode::Probes || mode == DebugVisualMode::ValidityMask)
  {
    G_UNUSED(camera_pos);

    // 1. Draw overall PRTGI grid extent bounding box in vibrant cyan
    BBox3 prtgiBounds;
    for (const auto &sec : asset.sectors)
      prtgiBounds += sec.bounds;
    if (prtgiBounds.isempty() && asset.grid.totalProbes() > 0)
    {
      prtgiBounds += asset.grid.getProbeWorldPos(asset.grid.minCoord);
      prtgiBounds += asset.grid.getProbeWorldPos(asset.grid.minCoord + asset.grid.count - IPoint3(1, 1, 1));
    }
    if (!prtgiBounds.isempty())
      draw_debug_box(prtgiBounds, E3DCOLOR(0, 240, 255, 255));

    float drawRadius = asset.grid.spacing * 0.12f;

    // Count total probes across all sectors
    uint32_t totalProbeCount = 0;
    for (const auto &sec : asset.sectors)
      totalProbeCount += (uint32_t)sec.probeIds.size();

    // Stride uniformly across the grid if probe count is massive, ensuring
    // probes always span and cover the ENTIRE PRTGI coordinate extent from corner to corner.
    constexpr uint32_t MAX_SPHERES_TO_DRAW = 16384;
    uint32_t stride = 1;
    if (totalProbeCount > MAX_SPHERES_TO_DRAW)
      stride = (totalProbeCount + MAX_SPHERES_TO_DRAW - 1) / MAX_SPHERES_TO_DRAW;

    for (const auto &sec : asset.sectors)
    {
      for (size_t i = 0; i < sec.probeIds.size(); i += stride)
      {
        const PRTProbeData &pData = sec.probeData[i];
        uint32_t pIdx = sec.probeIds[i];
        IPoint3 coord = asset.grid.getProbeCoord(pIdx);
        Point3 pos = asset.grid.getProbeWorldPos(coord) + pData.captureOffset;

        E3DCOLOR color = E3DCOLOR(255, 255, 255, 255); // Solid white debug probe
        if (mode == DebugVisualMode::ValidityMask)
        {
          if (pData.validity == 0)
            color = E3DCOLOR(255, 40, 40, 255);       // Red: buried solid / culled
          else if (pData.factorCount == 0)
            color = E3DCOLOR(60, 160, 255, 255);      // Blue/Cyan: pure sky probe
          else
            color = E3DCOLOR(50, 255, 80, 255);       // Green: active surface probe
        }
        else
        {
          if (pData.validity == 0)
            color = E3DCOLOR(170, 170, 170, 180);     // Dimmer white for boundary/buried probes
        }

        // Draw solid 3D sphere
        draw_solid_debug_sphere(pos, drawRadius, color);
        // Draw subtle wireframe outline for crisp edge definition
        draw_cached_debug_sphere(pos, drawRadius, E3DCOLOR(color.r > 128 ? color.r - 40 : color.r + 40,
                                                           color.g > 128 ? color.g - 40 : color.g + 40,
                                                           color.b > 128 ? color.b - 40 : color.b + 40, 255), 8);
      }
    }
  }
}

void PRTGIDebugRenderer::drawOnScreenHUD(const PRTGIStats &stats, const PRTGISettings &settings)
{
  if (!settings.debugOverlay)
    return;

  // Render on-screen debug text via StdGuiRender
  StdGuiRender::ScopeStarterOptional strt;
  StdGuiRender::set_font(0);
  StdGuiRender::reset_draw_str_attr();

  float x = 20.0f;
  float y = 40.0f;
  float lineH = 18.0f;
  E3DCOLOR titleColor(100, 220, 255, 255);
  E3DCOLOR labelColor(200, 200, 200, 255);
  E3DCOLOR valColor(255, 255, 100, 255);
  E3DCOLOR statusColor = (stats.totalProbes > 0) ? E3DCOLOR(100, 255, 100, 255) : E3DCOLOR(255, 120, 80, 255);

  // Background box
  StdGuiRender::set_color(E3DCOLOR(20, 25, 30, 210));
  StdGuiRender::render_rect(x - 10.0f, y - 10.0f, x + 440.0f, y + 210.0f);

  StdGuiRender::set_color(titleColor);
  StdGuiRender::draw_strf_to(x, y, "=== [PRTGI GLOBAL ILLUMINATION MONITOR] ===");
  y += lineH + 4.0f;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Status: ");
  StdGuiRender::set_color(statusColor);
  StdGuiRender::draw_strf_to(x + 55.0f, y, "%s", stats.bakingStatus);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Active GI: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 70.0f, y, "%s", stats.activeSystemName);

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x + 160.0f, y, "Pipe: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 200.0f, y, "%s", stats.pipelineTypeName);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Grid Size: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 70.0f, y, "%dx%dx%d (%u probes, %.1fm spacing)",
    stats.gridCount.x, stats.gridCount.y, stats.gridCount.z, stats.totalProbes, settings.gridSpacing);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Streaming: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 80.0f, y, "%u/%u Sectors GPU (Budget: %d/frame)",
    stats.residentSectors, stats.totalSectors, settings.updateBudget);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Sector Focus: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 95.0f, y, "Near=%d, Background=%d",
    stats.currentNearSector, stats.currentBackgroundSector);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Geometry: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 75.0f, y, "%u Surfels | %u Bricks",
    stats.totalSurfels, stats.totalBricks);
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "VRAM Usage: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 90.0f, y, "%.2f MB GPU | %.2f MB Factors",
    (double)stats.runtimeVramBytes / (1024.0 * 1024.0), (double)stats.factorsMemoryBytes / (1024.0 * 1024.0));
  y += lineH;

  StdGuiRender::set_color(labelColor);
  StdGuiRender::draw_strf_to(x, y, "Relight Time: ");
  StdGuiRender::set_color(valColor);
  StdGuiRender::draw_strf_to(x + 95.0f, y, "%.3f ms", stats.lastRelightDurationMs);
}

void PRTGIDebugRenderer::drawImGuiWindow(const PRTGIStats &stats, PRTGISettings &settings)
{
  if (!settings.debugOverlay)
    return;
  ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("PRTGI Debug Monitor", &settings.debugOverlay))
  {
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Precomputed Radiance Transfer GI");
    ImGui::Separator();

    ImGui::Text("Status: %s", stats.bakingStatus);
    ImGui::Text("Active System: %s", stats.activeSystemName);
    ImGui::Text("Pipeline: %s", stats.pipelineTypeName);
    ImGui::Separator();

    ImGui::Text("Probes Count: %u", stats.totalProbes);
    ImGui::Text("Grid Resolution: %dx%dx%d", stats.gridCount.x, stats.gridCount.y, stats.gridCount.z);
    ImGui::Text("Resident Sectors: %u / %u", stats.residentSectors, stats.totalSectors);
    ImGui::Text("Surfels: %u | Bricks: %u", stats.totalSurfels, stats.totalBricks);
    ImGui::Text("VRAM GPU: %.2f MB | Factors: %.2f MB", (double)stats.runtimeVramBytes / (1024.0 * 1024.0),
      (double)stats.factorsMemoryBytes / (1024.0 * 1024.0));
    ImGui::Text("Relight Time: %.3f ms", stats.lastRelightDurationMs);
    ImGui::Separator();

    ImGui::SliderFloat("Grid Spacing (m)", &settings.gridSpacing, 1.0f, 10.0f);
    ImGui::SliderInt("Update Budget", &settings.updateBudget, 1, 8);
    ImGui::SliderFloat("Sky Intensity", &settings.skyIntensity, 0.0f, 5.0f);
    ImGui::Checkbox("Show Sky Visibility", &settings.debugSkyVisibility);

    const char *visualModes[] = {"None", "Probes", "Cascades", "Sectors", "ValidityMask", "Irradiance"};
    int currentMode = (int)settings.debugDraw;
    if (ImGui::Combo("3D Visual Mode", &currentMode, visualModes, IM_ARRAYSIZE(visualModes)))
      settings.debugDraw = (DebugVisualMode)currentMode;
  }
  ImGui::End();
}

} // namespace daprtgi
