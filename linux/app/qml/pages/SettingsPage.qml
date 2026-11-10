// Counterpart of SettingsPage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

PageFrame {
    id: page

    readonly property var vm: app.settings

    title: qsTrId("Linux_Settings.Title")

    SettingsGroup {
        header: qsTrId("Settings_General.Header")

        SettingsCard {
            header: qsTrId("Settings_General_Language.Header")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: page.vm.languages
                currentIndex: page.vm.language
                onActivated: (index) => page.vm.language = index
            }
        }

        InfoBar {
            visible: page.vm.requireRestart
            severity: InfoBar.Severity.Warning
            title: qsTrId("Settings_General_RequireRestart.Title")

            Button {
                text: qsTrId("Settings_General_RequireRestart_ActionButton.Content")
                onClicked: page.vm.restart()
            }
        }

        SettingsCard {
            visible: page.vm.canChooseTheme
            header: qsTrId("Settings_General_Theme.Header")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [
                    qsTrId("Linux_SystemDefault"),
                    qsTrId("Settings_General_Theme_Light.Content"),
                    qsTrId("Settings_General_Theme_Dark.Content")
                ]
                currentIndex: page.vm.theme
                onActivated: (index) => page.vm.theme = index
            }
        }

        SettingsCard {
            header: qsTrId("Linux_Settings_ConfigLocation.Header")
            description: page.vm.configPath

            Button {
                text: qsTrId("Settings_General_PortableMode_Locate.Content")
                onClicked: page.vm.openConfigLocation()
            }
        }

        SettingsCard {
            header: qsTrId("Settings_General_ShowNotifyIcon.Header")
            description: qsTrId("Settings_General_ShowNotifyIcon.Description")

            Switch {
                checked: page.vm.isShowNotifyIcon
                onToggled: page.vm.isShowNotifyIcon = checked
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Settings_Launch.Header")

        SettingsCard {
            header: qsTrId("Settings_Launch_RunAtStartup.Header")
            description: qsTrId("Settings_Launch_RunAtStartup.Description")

            Switch {
                checked: page.vm.isRunAtStartup
                onToggled: page.vm.isRunAtStartup = checked
            }
        }
    }
}
