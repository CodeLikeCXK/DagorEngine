from "%darg/ui_imports.nut" import *

let cursors = require("samples_prog/_cursors.nut")

let sliderVal = Watched(150)

function slider() {
  let fValue = sliderVal.get()
  let minVal = 60
  let maxVal = 160

  let knob = {
    size  = static [50, flex()]
    rendObj = ROBJ_SOLID
    pos = [-10,0]
  }

  return {
    rendObj = ROBJ_SOLID
    valign = ALIGN_CENTER
    behavior = Behaviors.Slider

    watch = sliderVal
    fValue = fValue

    knob = knob
    min = minVal
    max = maxVal
    unit = 5
    orientation = O_HORIZONTAL
    size = static [flex(), sh(5)]
    color = Color(0, 10, 20)
    flow = FLOW_HORIZONTAL

    children = [
      {size=[flex(fValue-minVal), sh(5)] color = Color((fValue-minVal+50)/(maxVal-minVal)*255/2, 0, 0) rendObj = ROBJ_SOLID}
      knob
      {size=[flex(maxVal-fValue), 0]}
    ]

    onChange = function(val) {
      sliderVal.set(val)
    }
  }
}
return {
  rendObj = ROBJ_SOLID
  color = Color(40,40,40)
  cursor = cursors.normal

  children = [
    @(){rendObj = ROBJ_TEXT text = $"This is demo of Slider Behavior. Value = {sliderVal.get()}" watch=sliderVal}
    slider
  ]
  size =flex()
  halign = ALIGN_CENTER
  valign = ALIGN_CENTER
  flow = FLOW_VERTICAL
}
