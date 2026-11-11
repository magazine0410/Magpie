// The dialog that records the keys of a global shortcut, for a desktop that
// leaves the shortcuts to the programs. Counterpart of ShortcutDialog of the
// Windows user interface. While it is open the scaling service binds no
// keys, so that the ones pressed arrive here.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    // The view model of the Home page
    required property var vm
    // The ShortcutAction the keys are for, and what the dialog is called
    property int action: 0
    property string title

    // What was pressed last, as Qt reports it; 0: nothing yet
    property int key: 0
    property int modifiers: 0
    readonly property string problem: key !== 0 ? vm.shortcutProblem(action, key, modifiers) : ""
    readonly property bool canSave: key !== 0 && problem === ""

    function record(action, title) {
        root.action = action
        root.title = title
        root.key = 0
        root.modifiers = 0
        open()
    }

    function save() {
        if (canSave && vm.setShortcut(action, key, modifiers)) {
            close()
        }
    }

    padding: 24
    modal: true
    anchors.centerIn: Overlay.overlay
    closePolicy: Popup.CloseOnEscape

    onOpened: {
        vm.beginShortcutRecording()
        keys.forceActiveFocus()
    }
    onClosed: vm.endShortcutRecording()

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: root.title
            font.weight: Font.DemiBold
            font.pixelSize: 18
        }

        Label {
            Layout.maximumWidth: 360
            wrapMode: Text.WordWrap
            text: qsTrId("ShortcutDialogContent_Description.Text")
        }

        // Takes the key presses and shows them
        Rectangle {
            id: keys
            objectName: "shortcutRecorderKeys"

            Layout.fillWidth: true
            implicitHeight: 56
            radius: 4
            color: "transparent"
            border.width: activeFocus ? 2 : 1
            border.color: activeFocus ? palette.highlight : palette.mid
            focus: true

            Keys.onPressed: (event) => {
                // A modifier by itself is the beginning of a shortcut
                if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_AltGr, Qt.Key_Meta, Qt.Key_Super_L,
                     Qt.Key_Super_R].indexOf(event.key) >= 0) {
                    return
                }
                if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                    return
                }
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && event.modifiers === Qt.NoModifier) {
                    root.save()
                    event.accepted = true
                    return
                }
                root.key = event.key
                root.modifiers = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier | Qt.MetaModifier)
                event.accepted = true
            }

            Label {
                objectName: "shortcutRecorderText"
                anchors.centerIn: parent
                text: root.key !== 0 ? root.vm.shortcutText(root.key, root.modifiers) : ""
                font.weight: Font.DemiBold
                font.pixelSize: 18
            }
        }

        Label {
            objectName: "shortcutRecorderProblem"
            Layout.maximumWidth: 360
            wrapMode: Text.WordWrap
            // The rule, until keys were pressed that break it
            text: root.problem !== "" ? root.problem : qsTrId("ShortcutDialogContent_Tip.Text")
            color: root.problem !== "" ? "#c42b1c" : palette.windowText
            opacity: root.problem !== "" ? 1 : 0.7
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8

            Button {
                objectName: "shortcutRecorderSave"
                text: qsTrId("ShortcutDialog_Save")
                enabled: root.canSave
                highlighted: true
                onClicked: root.save()
            }

            Button {
                text: qsTrId("ShortcutDialog_Cancel")
                onClicked: root.close()
            }
        }
    }
}
