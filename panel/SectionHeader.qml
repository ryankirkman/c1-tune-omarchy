import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A section title, with an "Auto" switch for the sections that have one.
RowLayout {
    id: header
    property string title
    property bool hasToggle: false
    property bool checked: false
    signal toggled(bool on)

    Layout.fillWidth: true
    Layout.topMargin: 8
    spacing: 6

    Label {
        text: header.title
        textFormat: Text.PlainText
        font.pixelSize: 10
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.6
        color: theme.dim
        Layout.fillWidth: true
    }
    Label {
        visible: header.hasToggle
        text: qsTr("Auto")
        font.pixelSize: 11
        color: theme.dim
    }
    Switch {
        visible: header.hasToggle
        checked: header.checked
        Layout.preferredHeight: 24
        Accessible.name: qsTr("%1 auto").arg(header.title)
        onToggled: header.toggled(checked)
    }
}
