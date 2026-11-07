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
#include <debug/dag_debug.h>

namespace daprtgi
{

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
    float drawRadius = asset.grid.spacing * 0.12f;
    float maxDist = 24.0f; // only draw probes near camera (24m)
    float maxDistSq = maxDist * maxDist;
    BBox3 camBox(camera_pos - Point3(maxDist, maxDist, maxDist), camera_pos + Point3(maxDist, maxDist, maxDist));

    int maxSpheresToDraw = 2048; // Increased cap to avoid frame rate collapse while giving full coverage
    int drawnSpheres = 0;

    for (const auto &sec : asset.sectors)
    {
      if (drawnSpheres >= maxSpheresToDraw)
        break;

      // Fast AABB check: skip distant sectors completely
      if (!(sec.bounds & camBox))
        continue;

      for (size_t i = 0; i < sec.probeIds.size(); ++i)
      {
        const PRTProbeData &pData = sec.probeData[i];

        // In normal Probes mode, hide buried solid probes so they don't clutter the scene
        if (mode == DebugVisualMode::Probes && pData.validity == 0)
          continue;

        uint32_t pIdx = sec.probeIds[i];
        IPoint3 coord = asset.grid.getProbeCoord(pIdx);
        Point3 pos = asset.grid.getProbeWorldPos(coord);

        float distSq = lengthSq(pos - camera_pos);
        if (distSq > maxDistSq)
          continue;

        E3DCOLOR color = E3DCOLOR(255, 200, 50, 255); // Default gold probe
        if (mode == DebugVisualMode::ValidityMask)
        {
          if (pData.validity == 0)
            color = E3DCOLOR(255, 40, 40, 255);       // Red: buried solid / culled
          else if (pData.factorCount == 0)
            color = E3DCOLOR(60, 160, 255, 255);      // Blue/Cyan: pure sky probe
          else
            color = E3DCOLOR(50, 255, 80, 255);       // Green: active surface probe
        }

        draw_cached_debug_sphere(pos, drawRadius, color, 8);
        if (++drawnSpheres >= maxSpheresToDraw)
          break;
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
  StdGuiRender::draw_strf_to(x + 90.0f, y, "%.2f MB Factors",
    (double)stats.factorsMemoryBytes / (1024.0 * 1024.0));
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
    ImGui::Text("VRAM Factors: %.2f MB", (double)stats.factorsMemoryBytes / (1024.0 * 1024.0));
    ImGui::Text("Relight Time: %.3f ms", stats.lastRelightDurationMs);
    ImGui::Separator();

    ImGui::SliderFloat("Grid Spacing (m)", &settings.gridSpacing, 1.0f, 10.0f);
    ImGui::SliderInt("Update Budget", &settings.updateBudget, 1, 8);
    ImGui::SliderFloat("Sky Intensity", &settings.skyIntensity, 0.0f, 5.0f);

    const char *visualModes[] = {"None", "Probes", "Cascades", "Sectors", "ValidityMask", "Irradiance"};
    int currentMode = (int)settings.debugDraw;
    if (ImGui::Combo("3D Visual Mode", &currentMode, visualModes, IM_ARRAYSIZE(visualModes)))
      settings.debugDraw = (DebugVisualMode)currentMode;
  }
  ImGui::End();
}

} // namespace daprtgi

