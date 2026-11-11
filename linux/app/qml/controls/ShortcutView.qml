// The key of a global shortcut and a button for changing it. Where the
// desktop owns the keys, the button opens its dialog; where it has no such
// dialog, the keys are those of config.json and there is no button.
// Counterpart of ShortcutControl of the Windows user interface, which records
// the keys itself.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    // As the desktop writes the key, for example "Alt+Shift+A"; empty if the
    // shortcut has none
    property string key
    // The desktop registered the shortcuts
    property bool available: true
    // The desktop has a dialog for the keys
    property bool canChange: true
    signal changeRequested

    spacing: 8

    Label {
        text: !root.available ? qsTrId("Linux_Shortcut_Unavailable")
            : root.key !== "" ? root.key : qsTrId("Linux_Shortcut_None")
        opacity: root.available && root.key !== "" ? 1 : 0.7
        font.weight: root.available && root.key !== "" ? Font.DemiBold : Font.Normal
    }

    Button {
        text: qsTrId("Linux_Shortcut_Change")
        visible: root.canChange
        enabled: root.available
        onClicked: root.changeRequested()

        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: qsTrId("Linux_Shortcut_ChangeToolTip")
    }
}
