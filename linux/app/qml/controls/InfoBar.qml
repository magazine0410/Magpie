// A message inside a page. Counterpart of the InfoBar of WinUI.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    enum Severity {
        Informational,
        Success,
        Warning,
        Error
    }

    property string title
    property string message
    property int severity: InfoBar.Severity.Informational
    // A button on the right
    default property alias action: actionRow.data

    readonly property color severityColor: severity === InfoBar.Severity.Error ? "#d32f2f"
        : severity === InfoBar.Severity.Warning ? "#f9a825"
        : severity === InfoBar.Severity.Success ? "#2e7d32" : "#1976d2"

    Layout.fillWidth: true
    implicitHeight: row.implicitHeight + 20
    radius: 5
    color: Qt.rgba(severityColor.r, severityColor.g, severityColor.b, 0.15)
    border.width: 1
    border.color: Qt.rgba(severityColor.r, severityColor.g, severityColor.b, 0.5)

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 12

        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 3
            implicitWidth: 12
            implicitHeight: 12
            radius: 6
            color: root.severityColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.title
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.message
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            id: actionRow
            spacing: 8
        }
    }
}
