// A setting with further settings below it that can be folded away.
// Counterpart of SettingsExpander of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property string header
    property string description
    property bool expanded: false
    // The control in the header
    property alias headerContent: headerRow.data
    // The settings below: SettingsCard items with "nested: true"
    default property alias items: itemsColumn.data

    SystemPalette {
        id: systemPalette
    }

    readonly property color lineColor: Qt.rgba(systemPalette.text.r, systemPalette.text.g, systemPalette.text.b, 0.15)

    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + 2
    radius: 5
    color: systemPalette.base
    border.width: 1
    border.color: lineColor
    clip: true

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        SettingsCard {
            nested: true
            color: "transparent"
            header: root.header
            description: root.description

            RowLayout {
                id: headerRow
                spacing: 8
            }

            ToolButton {
                objectName: root.objectName + "Toggle"
                text: root.expanded ? "▴" : "▾"
                onClicked: root.expanded = !root.expanded
            }
        }

        ColumnLayout {
            id: itemsColumn
            Layout.fillWidth: true
            visible: root.expanded
            spacing: 0
        }
    }
}
