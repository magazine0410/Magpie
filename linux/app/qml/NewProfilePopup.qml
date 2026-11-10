// The dialog for a new profile: the window of the program, the name of the
// profile, and the profile to copy the settings from. Counterpart of the
// "New profile" flyout of RootPage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    readonly property var windows: app.root.candidateWindows

    function accept() {
        if (createButton.enabled) {
            app.root.addProfile(windowBox.currentIndex, nameField.text, copyFromBox.currentIndex)
            root.close()
        }
    }

    padding: 16
    modal: true

    onAboutToShow: {
        // The list arrives a moment later
        app.root.prepareNewProfile()
        nameField.text = ""
        copyFromBox.model = app.root.copyFromList()
        copyFromBox.currentIndex = 0
    }

    // With one window there is nothing to choose
    onWindowsChanged: {
        windowBox.currentIndex = windows.length === 1 ? 0 : -1
        nameField.text = app.root.defaultProfileName(windowBox.currentIndex)
    }

    contentItem: ColumnLayout {
        spacing: 8

        // No window a profile could be made for
        ColumnLayout {
            Layout.minimumWidth: 280
            Layout.margins: 24
            visible: root.windows.length === 0
            spacing: 16

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "ℹ"
                font.pointSize: Application.font.pointSize * 2.5
                opacity: 0.7
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTrId("Root_NewProfileFlyout_NoCandidateWindow.Text")
            }
        }

        Label {
            Layout.bottomMargin: 8
            visible: root.windows.length > 0
            text: qsTrId("Root_NewProfileFlyout_Title.Text")
            font.pointSize: Application.font.pointSize * 1.3
            font.weight: Font.DemiBold
        }

        ComboBox {
            id: windowBox
            objectName: "newProfileWindow"
            Layout.fillWidth: true
            Layout.minimumWidth: 320
            Layout.maximumWidth: 480
            visible: root.windows.length > 0
            model: root.windows
            textRole: "title"
            displayText: currentIndex < 0 ? qsTrId("Root_NewProfileFlyout_ComboBox.PlaceholderText") : currentText
            onActivated: (index) => nameField.text = app.root.defaultProfileName(index)

            delegate: ItemDelegate {
                required property var modelData
                required property int index

                width: windowBox.width
                highlighted: windowBox.highlightedIndex === index
                icon.name: modelData.icon.startsWith("/") ? "" : modelData.icon
                icon.source: modelData.icon.startsWith("/") ? "file://" + modelData.icon : ""
                text: modelData.title
            }
        }

        Label {
            Layout.topMargin: 8
            visible: root.windows.length > 0
            text: qsTrId("Root_NewProfileFlyout_Name.Text")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.windows.length > 0
            spacing: 4

            TextField {
                id: nameField
                objectName: "newProfileName"
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: root.accept()
            }

            // What the name can be filled with
            ToolButton {
                text: "▾"
                enabled: windowBox.currentIndex >= 0
                onClicked: fillMenu.popup(this, 0, height)

                Menu {
                    id: fillMenu

                    MenuItem {
                        text: qsTrId("Root_NewProfileFlyout_NameContextFlyout_AppName")
                        onTriggered: nameField.text = app.root.defaultProfileName(windowBox.currentIndex)
                    }

                    MenuItem {
                        text: qsTrId("Root_NewProfileFlyout_NameContextFlyout_ProcessName")
                        onTriggered: nameField.text = app.root.processName(windowBox.currentIndex)
                    }

                    MenuItem {
                        text: qsTrId("Root_NewProfileFlyout_NameContextFlyout_WindowTitle")
                        onTriggered: nameField.text = app.root.windowTitle(windowBox.currentIndex)
                    }
                }
            }
        }

        Label {
            Layout.topMargin: 8
            visible: root.windows.length > 0
            text: qsTrId("Root_NewProfileFlyout_CopyFrom.Text")
        }

        ComboBox {
            id: copyFromBox
            Layout.fillWidth: true
            visible: root.windows.length > 0
        }

        Button {
            id: createButton
            objectName: "newProfileCreate"
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 12
            visible: root.windows.length > 0
            highlighted: true
            enabled: app.root.canAddProfile(windowBox.currentIndex, nameField.text)
            text: qsTrId("Root_NewProfileFlyout_Create.Content")
            onClicked: root.accept()
        }
    }
}
