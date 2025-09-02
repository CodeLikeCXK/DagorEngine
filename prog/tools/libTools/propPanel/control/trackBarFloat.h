// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <propPanel/control/propertyControlBase.h>
#include <propPanel/constants.h>
#include <propPanel/imguiHelper.h>
#include <propPanel/immediateFocusLossHandler.h>
#include "spinEditStandalone.h"
#include "../scopedImguiBeginDisabled.h"

namespace PropPanel
{

class TrackBarFloatPropertyControl : public PropertyControlBase, public IImmediateFocusLossHandler
{
public:
  TrackBarFloatPropertyControl(ControlEventHandler *event_handler, ContainerPropertyControl *parent, int id, int x, int y, hdpi::Px w,
    const char caption[], float min, float max, float step, float power) :
    PropertyControlBase(id, event_handler, parent, x, y, w, hdpi::Px(0)),
    controlCaption(caption),
    controlPower(power),
    spinEdit(min, max, step, 0)
  {
    spinEdit.setValue(min);
  }

  ~TrackBarFloatPropertyControl() override
  {
    if (get_focused_immediate_focus_loss_handler() == this)
      set_focused_immediate_focus_loss_handler(nullptr);
  }

  unsigned getTypeMaskForSet() const override { return CONTROL_DATA_TYPE_FLOAT | CONTROL_DATA_MIN_MAX_STEP | CONTROL_CAPTION; }
  unsigned getTypeMaskForGet() const override { return CONTROL_DATA_TYPE_FLOAT; }

  float getFloatValue() const override { return spinEdit.getValue(); }

  void setFloatValue(float value) override
  {
    const float step = spinEdit.getStepValue();
    const float min = spinEdit.getMinValue();
    const float max = spinEdit.getMaxValue();

    value = clamp(value, min, max);
    if (step != 0.0f)
      value = min + (floorf(0.5f + ((value - min) / step)) * step);

    spinEdit.setValue(value);
  }

  void setMinMaxStepValue(float min, float max, float step) override { spinEdit.setMinMaxStepValue(min, max, step); }

  void setCaptionValue(const char value[]) override { controlCaption = value; }

  void reset() override
  {
    setFloatValue(spinEdit.getMinValue());

    PropertyControlBase::reset();
  }

  void setEnabled(bool enabled) override { controlEnabled = enabled; }

  void updateImgui() override
  {
    ScopedImguiBeginDisabled scopedDisabled(!controlEnabled);

    ImguiHelper::separateLineLabel(controlCaption);

    const float remainingWidthAfterTheLabel = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x;
    const float spinEditWidth = ImguiHelper::getDefaultRightSideEditWidth();
    const float sliderWidth = remainingWidthAfterTheLabel - spinEditWidth;
    if (sliderWidth >= hdpi::_pxS(Constants::MINIMUM_TRACK_BAR_SLIDER_WIDTH_TO_SHOW))
    {
      ImGui::SetNextItemWidth(sliderWidth);

      float value = spinEdit.getValue();
      const bool changed = controlPower == TRACKBAR_DEFAULT_POWER ? sliderFloat(value) : sliderFloatPower(value);

      setPreviousImguiControlTooltip();

      if (changed)
      {
        // This is here to add stepping. (ImGui currently does not support that. See: https://github.com/ocornut/imgui/issues/1183.)
        setFloatValue(value);

        onWcChange(nullptr);
      }

      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      ImGui::SetNextItemWidth(spinEditWidth);
    }
    else
    {
      ImGui::SetNextItemWidth(-FLT_MIN);
    }

    setFocusToNextImGuiControlIfRequested();
    spinEdit.updateImgui(*this, &controlTooltip, this);

    if (spinEdit.isTextInputFocused())
      set_focused_immediate_focus_loss_handler(this);
    else if (get_focused_immediate_focus_loss_handler() == this)
      set_focused_immediate_focus_loss_handler(nullptr);
  }

private:
  void onImmediateFocusLoss() override { spinEdit.sendWcChangeIfVarChanged(*this); }

  bool sliderFloat(float &value)
  {
    pushTrackBarColorOverrides();
    const bool changed =
      ImGui::SliderFloat("##s", &value, spinEdit.getMinValue(), spinEdit.getMaxValue(), "", ImGuiSliderFlags_NoInput);
    popTrackBarColorOverrides();
    return changed;
  }

  bool sliderFloatPower(float &value)
  {
    // As the power parameter no longer can be given to ImGui's SliderFloat, we map our value between 0 and 1 respecting
    // the power parameter, let the slider work normally within the 0..1 range, and then convert the value back.

    const float min = spinEdit.getMinValue();
    const float max = spinEdit.getMaxValue();
    float ratio = min == max ? 0.0f : pow((spinEdit.getValue() - min) / (max - min), 1.0f / controlPower);

    pushTrackBarColorOverrides();
    const bool changed = ImGui::SliderFloat("##s", &ratio, 0.0f, 1.0f, "", ImGuiSliderFlags_NoInput);
    popTrackBarColorOverrides();
    if (!changed)
      return false;

    const float oldValue = value;
    value = min + (pow(ratio, controlPower) * (max - min));
    return value != oldValue;
  }

  String controlCaption;
  bool controlEnabled = true;
  const float controlPower;
  SpinEditControlStandalone spinEdit;
};

} // namespace PropPanel