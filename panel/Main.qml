import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

// The control panel: exposure, focus, white balance, framing, image, and an optional preview.
// It only shows what the running app reports and sends back what the user changes.
ApplicationWindow {
    id: window

    readonly property var s: backend.settings
    readonly property bool ready: backend.connected && backend.status === "ready"
    readonly property bool failed: backend.connected && backend.status === "failed"
    readonly property int margin: 14
    // A window that can't be resized is one Hyprland floats, which is what a panel wants.
    readonly property int panelHeight: Math.min(content.implicitHeight + 2 * margin, Screen.desktopAvailableHeight - 96)

    function setting(key, fallback) {
        var value = s[key]
        return value === undefined ? fallback : value
    }
    function signed(v) { return v > 0 ? "+" + v : String(v) }
    function zoomLabel(index) {
        var p = backend.zoomStops[index]
        if (p === undefined)
            return ""
        return p === 100 ? "1×" : p === 200 ? "2×" : (p / 100).toLocaleString(Qt.locale(), "f", 2) + "×"
    }

    visible: true
    title: qsTr("C1 Tune")
    width: 340
    height: panelHeight
    minimumWidth: 340
    maximumWidth: 340
    minimumHeight: panelHeight
    maximumHeight: panelHeight
    color: theme.background

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.primary: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground
    // Set once here; every control inherits it.
    Material.roundedScale: theme.radius

    Shortcut { sequences: ["Escape", "Q"]; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; onActivated: backend.preview = !backend.preview }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; onActivated: if (window.failed) backend.retry() }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 2 * window.margin
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            x: window.margin
            y: window.margin
            width: window.width - 2 * window.margin
            spacing: 3

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: "Opal C1"
                    font.pixelSize: 14
                    font.weight: Font.Bold
                    color: theme.bright
                }
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: window.ready ? theme.accent : window.failed || !backend.connected ? theme.red : theme.muted
                }
                Label {
                    text: !backend.connected ? qsTr("C1 Tune isn't running")
                        : window.failed ? qsTr("Failed")
                        : backend.restartPending ? qsTr("Restarting camera…") : backend.message
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: theme.dim
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    Layout.fillWidth: true
                }
                Button {
                    visible: window.failed || !backend.connected
                    text: backend.connected ? qsTr("Retry") : qsTr("Start")
                    flat: true
                    font.pixelSize: 12
                    Layout.preferredHeight: 28
                    onClicked: backend.connected ? backend.retry() : backend.startApp()
                }
            }
            // What went wrong, in full: a missing udev rule or firmware needs a sentence.
            Label {
                visible: window.failed
                text: backend.message
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: theme.red
                Layout.fillWidth: true
            }

            Loader {
                id: previewLoader
                Layout.fillWidth: true
                Layout.preferredHeight: backend.preview ? Math.round(content.width * 9 / 16) : 0
                Layout.topMargin: backend.preview ? 6 : 0
                visible: backend.preview
                // The webcam re-enumerates whenever the camera restarts, so the preview is
                // torn down with it and rebuilt once the camera is ready again.
                active: backend.preview && window.ready
                source: "CameraPreview.qml"
                Rectangle { anchors.fill: parent; z: -1; color: "black"; visible: backend.preview }
            }

            SectionHeader {
                title: qsTr("Exposure")
                hasToggle: true
                checked: setting("autoExposure", true)
                onToggled: (on) => backend.set("autoExposure", on)
            }
            SliderRow {
                title: qsTr("Shutter"); from: 100; to: 33000; step: 100
                value: setting("exposureUs", 16667); enabled: !setting("autoExposure", true)
                format: (v) => qsTr("%1 ms").arg((v / 1000).toLocaleString(Qt.locale(), "f", 1))
                onChanged: (v) => backend.set("exposureUs", v)
            }
            SliderRow {
                title: qsTr("ISO"); from: 100; to: 1600; step: 50
                value: setting("iso", 400); enabled: !setting("autoExposure", true)
                onChanged: (v) => backend.set("iso", v)
            }
            SliderRow {
                title: qsTr("Bias"); from: -9; to: 9
                value: setting("exposureCompensation", -1); enabled: setting("autoExposure", true)
                format: signed
                onChanged: (v) => backend.set("exposureCompensation", v)
            }

            SectionHeader {
                title: qsTr("Focus")
                hasToggle: true
                checked: setting("autoFocus", true)
                onToggled: (on) => backend.set("autoFocus", on)
            }
            SliderRow {
                title: qsTr("Distance"); from: 0; to: 255
                value: setting("lensPosition", 130); enabled: !setting("autoFocus", true)
                onChanged: (v) => backend.set("lensPosition", v)
            }

            SectionHeader {
                title: qsTr("White balance")
                hasToggle: true
                checked: setting("autoWhiteBalance", true)
                onToggled: (on) => backend.set("autoWhiteBalance", on)
            }
            SliderRow {
                title: qsTr("Temperature"); from: 1000; to: 12000; step: 100
                value: setting("whiteBalanceK", 4500); enabled: !setting("autoWhiteBalance", true)
                format: (v) => qsTr("%1 K").arg(v)
                onChanged: (v) => backend.set("whiteBalanceK", v)
            }

            // Zoom and framing restart the camera, so they apply on release (the label tracks the drag).
            SectionHeader { title: qsTr("Framing") }
            SliderRow {
                title: qsTr("Zoom"); from: 0; to: Math.max(1, backend.zoomStops.length - 1)
                value: Math.max(0, backend.zoomStops.indexOf(setting("zoom", 100)))
                applyOnRelease: true
                hint: qsTr("Zooming restarts the camera (about 5 seconds).")
                format: zoomLabel
                onChanged: (index) => backend.set("zoom", backend.zoomStops[index])
            }
            SliderRow {
                title: qsTr("Left – right"); from: 0; to: 100; snap: 50
                value: setting("frameX", 50); enabled: setting("zoom", 100) !== 100
                applyOnRelease: true
                format: (v) => qsTr("%1%").arg(v)
                onChanged: (v) => backend.set("frameX", v)
            }
            SliderRow {
                title: qsTr("Up – down"); from: 0; to: 100; snap: 50
                value: setting("frameY", 50); enabled: setting("zoom", 100) !== 100
                applyOnRelease: true
                format: (v) => qsTr("%1%").arg(v)
                onChanged: (v) => backend.set("frameY", v)
            }

            SectionHeader { title: qsTr("Image") }
            SliderRow {
                title: qsTr("Brightness"); from: -10; to: 10
                value: setting("brightness", 0); format: signed
                onChanged: (v) => backend.set("brightness", v)
            }
            SliderRow {
                title: qsTr("Contrast"); from: -10; to: 10
                value: setting("contrast", 0); format: signed
                onChanged: (v) => backend.set("contrast", v)
            }
            SliderRow {
                title: qsTr("Saturation"); from: -10; to: 10
                value: setting("saturation", 1); format: signed
                onChanged: (v) => backend.set("saturation", v)
            }
            SliderRow {
                title: qsTr("Sharpness"); from: 0; to: 4
                value: setting("sharpness", null)
                onChanged: (v) => backend.set("sharpness", v)
            }
            SliderRow {
                title: qsTr("Denoise"); from: 0; to: 4
                value: setting("noiseReduction", null)
                onChanged: (v) => backend.set("noiseReduction", v)
            }
            SliderRow {
                title: qsTr("Color denoise"); from: 0; to: 4
                value: setting("colorNoiseReduction", null)
                onChanged: (v) => backend.set("colorNoiseReduction", v)
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Label {
                    text: qsTr("Anti-flicker")
                    font.pixelSize: 13
                    Layout.fillWidth: true
                }
                ComboBox {
                    model: [qsTr("Off"), qsTr("50 Hz"), qsTr("60 Hz"), qsTr("Auto")]
                    currentIndex: Math.min(Math.max(setting("antiFlicker", 3), 0), 3)
                    font.pixelSize: 13
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 32
                    Accessible.name: qsTr("Anti-flicker")
                    onActivated: (index) => backend.set("antiFlicker", index)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CheckBox {
                    text: qsTr("Flip image")
                    checked: setting("flipped", false)
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Rotates the picture 180°. Restarts the camera (about 5 seconds).")
                    onToggled: backend.set("flipped", checked)
                }
                CheckBox {
                    text: qsTr("Microphone")
                    checked: setting("microphone", false)
                    font.pixelSize: 13
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Turns on the camera's built-in microphone. When off, it still appears but records silence. Restarts the camera (about 5 seconds).")
                    onToggled: backend.set("microphone", checked)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CheckBox {
                    text: qsTr("Preview")
                    checked: backend.preview
                    enabled: window.ready || backend.preview
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    onToggled: backend.preview = checked
                }
                CheckBox {
                    text: qsTr("Open at login")
                    checked: backend.openAtLogin
                    font.pixelSize: 13
                    onToggled: backend.openAtLogin = checked
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Label {
                    text: qsTr("C1 Tune %1").arg(backend.version)
                    textFormat: Text.PlainText
                    font.pixelSize: 10
                    color: theme.muted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Button {
                    text: qsTr("Reset")
                    flat: true
                    font.pixelSize: 12
                    enabled: backend.connected
                    Layout.preferredHeight: 30
                    onClicked: backend.reset()
                }
                Button {
                    text: qsTr("Quit")
                    flat: true
                    font.pixelSize: 12
                    Layout.preferredHeight: 30
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Stops C1 Tune. The camera returns to its factory webcam mode.")
                    onClicked: { backend.quitApp(); Qt.quit() }
                }
            }
        }
    }
}
