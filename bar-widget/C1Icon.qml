import QtQuick
import QtQuick.Shapes

// The Opal C1 seen from the front, as the macOS menu bar icon draws it: a flat slab with the
// lens at the left and the 4x3 vent grid at the right. One path filled odd-even, so the lens
// and the vents are holes and the glass in the middle of the lens is solid again.
Shape {
  id: icon

  property real iconSize: 18          // its width; the artwork is 24 by 18
  property color color: "white"

  width: iconSize
  height: iconSize * 18 / 24
  preferredRendererType: Shape.CurveRenderer

  ShapePath {
    strokeColor: "transparent"
    fillColor: icon.color
    fillRule: ShapePath.OddEvenFill
    scale: Qt.size(icon.width / 24, icon.height / 18)
    PathSvg { path: "M1.7,3.25 H22.3 A1.2,1.2 0 0 1 23.5,4.45 V13.55 A1.2,1.2 0 0 1 22.3,14.75 H1.7 A1.2,1.2 0 0 1 0.5,13.55 V4.45 A1.2,1.2 0 0 1 1.7,3.25 Z M1.65,9 a4.6,4.6 0 1,0 9.2,0 a4.6,4.6 0 1,0 -9.2,0 Z M4.226,9 a2.024,2.024 0 1,0 4.048,0 a2.024,2.024 0 1,0 -4.048,0 Z M12.075,5.35 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M12.075,9 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M12.075,12.65 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M14.925,5.35 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M14.925,9 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M14.925,12.65 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M17.775,5.35 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M17.775,9 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M17.775,12.65 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M20.625,5.35 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M20.625,9 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z M20.625,12.65 a0.775,0.775 0 1,0 1.55,0 a0.775,0.775 0 1,0 -1.55,0 Z" }
  }
}
