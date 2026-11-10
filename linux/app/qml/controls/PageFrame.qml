// A page: its title, buttons next to the title, and the content, which
// scrolls. Counterpart of PageFrame of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string title
    // Buttons to the right of the title
    property alias headerActions: actionsRow.data
    default property alias content: column.data

    readonly property int pageMargin: 32
    readonly property int pageMaxWidth: 1000

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: root.pageMaxWidth + 2 * root.pageMargin
            Layout.alignment: Qt.AlignHCenter
            Layout.leftMargin: root.pageMargin
            Layout.rightMargin: root.pageMargin
            Layout.topMargin: 24
            Layout.bottomMargin: 16
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.title
                elide: Text.ElideRight
                font.pointSize: Application.font.pointSize * 2
                font.weight: Font.DemiBold
            }

            RowLayout {
                id: actionsRow
                spacing: 4
            }
        }

        ScrollView {
            id: scrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            Item {
                width: scrollView.availableWidth
                implicitHeight: column.implicitHeight + 24

                ColumnLayout {
                    id: column
                    width: Math.min(root.pageMaxWidth, parent.width - 2 * root.pageMargin)
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 24
                }
            }
        }
    }
}
