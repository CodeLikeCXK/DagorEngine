from "%darg/ui_imports.nut" import *

function dmModule(count_total_state, count_broken_state) {
  return function () {
    let image = {
      rendObj = ROBJ_SOLID
      color = Color(0, 0, 255)
      //image = icon
      size = sh(5)
    }

    let dotAlive = @(_totalDotsCount) {
      rendObj = ROBJ_SOLID
      //image = images.dotFilled
      color = Color(255, 255, 255)
      size = sh(1)
      margin = sh(0.3)
    }
    let dotDead = @(_totalDotsCount) {
      rendObj = ROBJ_SOLID
      //image = images.dotHole
      color = Color(255, 50, 0)
      size = sh(1)
      margin = sh(0.3)
    }

    function dots () {
      let aliveCount = count_total_state.get() - count_broken_state.get()
      let children = []
      children.resize(aliveCount, dotAlive(count_total_state.get()))
      children.resize(count_total_state.get(), dotDead(count_total_state.get()))

      return {
        size = FLEX_H
        flow = FLOW_HORIZONTAL
        halign = ALIGN_CENTER
        children = children
      }
    }

    let children = [image]
    if (count_total_state.get() > 1)
      children.append(dots)

    return {
      size = SIZE_TO_CONTENT
      flow = FLOW_VERTICAL
      margin = static [sh(0.7), 0]
      watch = [
        count_total_state
        count_broken_state
      ]

      children = children
    }
  }
}

return dmModule
