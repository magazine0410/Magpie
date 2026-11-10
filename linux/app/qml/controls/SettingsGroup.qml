// A titled group of settings. Counterpart of SettingsGroup of the Windows
// user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    property string header
    property string description
    default property alias content: column.data

    Layout.fillWidth: true
    spacing: 6

    Label {
        Layout.fillWidth: true
        visible: text !== ""
        text: root.header
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Label {
        Layout.fillWidth: true
        Layout.bottomMargin: 2
        visible: text !== ""
        text: root.description
        opacity: 0.7
        wrapMode: Text.WordWrap
    }

    ColumnLayout {
        id: column
        Layout.fillWidth: true
        spacing: 4
    }
}
