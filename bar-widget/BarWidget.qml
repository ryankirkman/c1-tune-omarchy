import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons

// A webcam icon in the Omarchy bar that shows the camera's state at a glance, as the macOS
// menu bar icon does: normal when the camera is ready, dimmed when it isn't (unplugged,
// starting, or C1 Tune not running), and the bar's alert colour when something needs doing.
// A click asks c1-tune to open or close its panel, starting the app first if it isn't running.
//
// It runs nothing of its own and polls nothing: c1-tune writes its status to a small file
// whenever it changes, and this watches the file.
BarWidget {
  id: root
  moduleName: "com.ryankirkman.c1-tune"

  readonly property string statePath: Quickshell.env("XDG_RUNTIME_DIR") + "/c1-tune.state"
  // What the file says, and whether the app that wrote it is still there to mean it.
  property string fileStatus: "stopped"
  property bool alive: false
  readonly property string status: alive ? fileStatus : "stopped"
  property string message: ""

  readonly property string tooltip: {
    if (status === "ready") return "Opal C1 ready"
    if (status === "stopped") return "C1 Tune isn't running"
    // The message is C1 Tune's own, but it goes into a tooltip this file doesn't draw, so
    // nothing in it is allowed to read as markup.
    var text = message.replace(/[<>&]/g, "")
    return text !== "" ? "Opal C1: " + text : "Opal C1"
  }

  function parse(text) {
    var line = String(text).split("\n")[0]
    var tab = line.indexOf("\t")
    var kind = tab < 0 ? line : line.slice(0, tab)
    var known = ["ready", "starting", "notConnected", "busy", "failed", "stopped"]
    root.fileStatus = known.indexOf(kind) >= 0 ? kind : "stopped"
    root.message = tab < 0 ? "" : line.slice(tab + 1, tab + 201)
    if (root.fileStatus !== "stopped" && !live.connected) live.connected = true
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  FileView {
    id: stateFile
    path: root.statePath
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.parse(text())
    onLoadFailed: { root.fileStatus = "stopped"; root.message = "" }
  }

  // An app that is killed can't say goodbye in the file. Holding its socket open costs
  // nothing and ends the moment the app does, so a stale "ready" never outlives it.
  Socket {
    id: live
    path: Quickshell.env("XDG_RUNTIME_DIR") + "/c1-tune.sock"
    onConnectionStateChanged: root.alive = connected
  }

  // The file doesn't exist until C1 Tune has run once this session, and a file that isn't
  // there can't be watched: make sure it is, then look again. Once, not on a timer.
  Component.onCompleted: {
    // (Not through the bar: it isn't attached yet while the widget is being built.)
    Util.execDetached("[ -e \"$XDG_RUNTIME_DIR/c1-tune.state\" ] || printf 'stopped\\t\\n' > \"$XDG_RUNTIME_DIR/c1-tune.state\"")
    settle.restart()
  }
  Timer { id: settle; interval: 700; repeat: false; onTriggered: stateFile.reload() }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰖠"
    dimmed: root.status !== "ready" && root.status !== "failed"
    active: root.status === "failed"
    tooltipText: root.tooltip
    onPressed: function(b) {
      root.bar.run("c1-tune --panel")
    }
  }
}
