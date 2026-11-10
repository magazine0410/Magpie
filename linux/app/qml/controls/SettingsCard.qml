// One setting: its name and description on the left, its control on the
// right. Counterpart of SettingsCard of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property string header
    property string description
    // The control of the setting
    default property alias content: contentRow.data
    // Below the description, for a link
    property alias descriptionContent: descriptionColumn.data
    // Inside an expander: no frame of its own
    property bool nested: false
    // A line above, between the settings of an expander
    property bool separator: false
    // Pointer handlers for the whole card, for example one for dragging it
    property alias handlers: handlerItem.data

    SystemPalette {
        id: systemPalette
    }

    Layout.fillWidth: true
    implicitHeight: Math.max(52, row.implicitHeight + 20)
    radius: nested ? 0 : 5
    color: systemPalette.base
    border.width: nested ? 0 : 1
    border.color: Qt.rgba(systemPalette.text.r, systemPalette.text.g, systemPalette.text.b, 0.15)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        visible: root.separator
        color: root.border.color
    }

    Item {
        id: handlerItem
        anchors.fill: parent
    }

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.alignment: Qt.AlignVCenter
            spacing: 2
            visible: root.header !== "" || root.description !== "" || descriptionColumn.children.length > 0

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.header
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.description
                opacity: 0.7
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                id: descriptionColumn
                Layout.fillWidth: true
                visible: children.length > 0
                spacing: 2
            }
        }

        RowLayout {
            id: contentRow
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            spacing: 8
        }
    }
}
