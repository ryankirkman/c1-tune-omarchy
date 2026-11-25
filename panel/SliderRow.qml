import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Title, slider and value for one setting. A null value shows "Default" (leave the camera's
// tuning alone). The value label always follows the drag. With applyOnRelease, `changed`
// fires only when the handle is let go (for settings that restart the camera). With snap,
// values near it stick to it.
RowLayout {
    id: row
    property string title
    property int from: 0
    property int to: 100
    property int step: 1
    property var value: null
    property bool applyOnRelease: false
    property int snap: -1
    property string hint
    property var format: function (v) { return String(v) }
    signal changed(int value)

    // What the user is dragging to, until the app confirms it.
    property var dragged: null
    readonly property var shown: dragged !== null ? dragged : value
    onValueChanged: dragged = null

    Layout.fillWidth: true
    spacing: 6

    function quantize(raw) {
        var v = Math.min(to, from + Math.round((raw - from) / step) * step)
        if (snap >= 0 && Math.abs(v - snap) <= Math.max(step, Math.floor((to - from) * 3 / 100)))
            v = snap
        return v
    }

    function commit(v) {
        if (v !== row.value)
            row.changed(v)
    }

    Label {
        text: row.title
        textFormat: Text.PlainText
        font.pixelSize: 13
        color: row.enabled ? theme.foreground : theme.dim
        elide: Text.ElideRight
        Layout.preferredWidth: 92
    }
    Slider {
        id: slider
        Layout.fillWidth: true
        Layout.preferredHeight: 26
        from: row.from
        to: row.to
        stepSize: row.step
        Accessible.name: row.title
        ToolTip.visible: row.hint !== "" && hovered && !pressed
        ToolTip.delay: 700
        ToolTip.text: row.hint

        // The app's value drives the handle except while the user holds it.
        readonly property real target: row.value === null || row.value === undefined ? (row.from + row.to) / 2 : row.value
        Binding {
            target: slider
            property: "value"
            value: slider.target
            when: !slider.pressed && row.dragged === null
        }
        // A range that arrives after the value (the zoom steps) would leave the handle clamped.
        onToChanged: if (!pressed) value = target
        onFromChanged: if (!pressed) value = target

        onMoved: {
            var v = row.quantize(value)
            if (v !== value && row.snap >= 0 && v === row.snap)
                value = v
            row.dragged = v
            if (!(row.applyOnRelease && pressed))
                row.commit(v)
        }
        onPressedChanged: {
            if (pressed || row.dragged === null)
                return
            if (row.applyOnRelease)
                row.commit(row.dragged)
            if (row.dragged === row.value)
                row.dragged = null
        }
    }
    Label {
        text: row.shown === null || row.shown === undefined ? qsTr("Default") : row.format(row.shown)
        textFormat: Text.PlainText
        font.pixelSize: 12
        color: theme.dim
        horizontalAlignment: Text.AlignRight
        Layout.preferredWidth: 56
    }
}
