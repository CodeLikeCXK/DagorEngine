// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include "tabPage.h"
#include <propPanel/colors.h>
#include "../scopedImguiBeginDisabled.h"

namespace PropPanel
{

class TabPanelPropertyControl : public ContainerPropertyControl
{
public:
  TabPanelPropertyControl(ControlEventHandler *event_handler, ContainerPropertyControl *parent, int id, int x, int y, hdpi::Px w,
    hdpi::Px h) :
    ContainerPropertyControl(id, event_handler, parent, x, y, w, h)
  {}

  unsigned getTypeMaskForSet() const override { return CONTROL_DATA_TYPE_INT; }
  unsigned getTypeMaskForGet() const override { return CONTROL_DATA_TYPE_INT; }

  int getIntValue() const override { return selectedId; }

  void setIntValue(int value) override
  {
    if (value == selectedId)
      return;

    bool found = false;
    for (PropertyControlBase *control : mControlArray)
    {
      if (control->getID() == value)
      {
        found = true;
        break;
      }
    }

    selectedId = found ? value : -1;
  }

  void onControlAdd(PropertyControlBase *control) override { G_ASSERT_LOG(false, "TabPanel can contain only TabPages!"); }

  void clear() override
  {
    selectedId = -1;

    ContainerPropertyControl::clear();
  }

  ContainerPropertyControl *createTabPage(int id, const char caption[]) override
  {
    TabPagePropertyControl *newControl = new TabPagePropertyControl(mEventHandler, this, id, 0, 0, hdpi::Px(0), hdpi::Px(0), caption);

    mControlsNewLine.push_back(false);
    mControlArray.push_back(newControl);

    if (selectedId < 0)
      selectedId = id;

    return newControl;
  }

  void updateImgui() override
  {
    if (!ImGui::BeginTabBar("TabPanel"))
      return;

    int newSelectedId = selectedId;

    for (PropertyControlBase *control : mControlArray)
    {
      G_ASSERT(control->getImguiControlType() == (int)ControlType::TabPage);
      TabPagePropertyControl *tabPage = static_cast<TabPagePropertyControl *>(control);
      const int pageId = tabPage->getID();
      const bool selected = pageId == selectedId;

      ScopedImguiBeginDisabled scopedDisabled(!tabPage->getEnabled());

      // We do not use BeginTabItem because it does not support setting the selection to nothing, and daEditorX requires
      // that for activating edit modes that are not displayed on tab bar.

      if (selected)
        ImGui::PushStyleColor(ImGuiCol_Tab, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));

      ImGui::PushStyleColor(ImGuiCol_Text, PropPanel::getOverriddenColor(PropPanel::ColorOverride::TAB_BAR_TITLE));
      if (ImGui::TabItemButton(tabPage->getStringCaption()))
        newSelectedId = pageId;
      ImGui::PopStyleColor();

      if (selected)
      {
        ImGui::PopStyleColor();

        tabPage->updateImgui();
      }
    }

    ImGui::EndTabBar();

    if (newSelectedId != selectedId)
    {
      selectedId = newSelectedId;
      onWcChange(nullptr);
    }
  }

private:
  int selectedId = -1;
};

} // namespace PropPanel