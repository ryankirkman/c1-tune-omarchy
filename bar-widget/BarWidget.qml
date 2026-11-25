import QtQuick
import Quickshell
import qs.Ui
import qs.Commons

// A webcam icon in the Omarchy bar. It runs nothing of its own: a click asks c1-tune to open
// or close its panel, starting the app first if it isn't running.
BarWidget {
  id: root
  moduleName: "com.ryankirkman.c1-tune"

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰖠"
    tooltipText: "C1 Tune"
    onPressed: function(b) {
      root.bar.run("c1-tune --panel")
    }
  }
}
