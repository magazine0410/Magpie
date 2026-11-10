// Counterpart of AboutPage of the Windows user interface, without the
// update check.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

PageFrame {
    id: page

    readonly property var vm: app.about

    title: qsTrId("About_PageFrame.Title")

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 20

        Image {
            source: "qrc:/icons/magpie.svg"
            sourceSize.width: 64
            sourceSize.height: 64
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                text: "Magpie"
                font.pointSize: Application.font.pointSize * 1.8
            }

            Label {
                id: versionLabel
                text: page.vm.version
                opacity: 0.7

                // A double click with the Alt key held enables developer
                // mode, as on Windows
                TapHandler {
                    acceptedModifiers: Qt.AltModifier
                    onDoubleTapped: {
                        if (!page.vm.isDeveloperMode) {
                            page.vm.enableDeveloperMode()
                            developerModeBar.visible = true
                        }
                    }
                }
            }
        }
    }

    InfoBar {
        id: developerModeBar
        visible: false
        severity: InfoBar.Severity.Success
        title: qsTrId("About_DeveloperModeEnabled")
    }

    SettingsGroup {
        header: qsTrId("About_Feedback.Header")

        SettingsCard {
            header: qsTrId("About_Feedback_ReportBug.Header")

            Button {
                icon.name: "document-open-remote"
                text: "GitHub"
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl + "/issues/new")
            }
        }

        SettingsCard {
            header: qsTrId("About_Feedback_RequestFeature.Header")

            Button {
                icon.name: "document-open-remote"
                text: "GitHub"
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl + "/issues/new")
            }
        }

        SettingsCard {
            header: qsTrId("About_Feedback_Discussion.Header")

            Button {
                icon.name: "document-open-remote"
                text: "GitHub"
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl + "/discussions")
            }
        }
    }

    SettingsGroup {
        header: qsTrId("About_OtherLinks.Header")

        Flow {
            Layout.fillWidth: true
            spacing: 4

            Button {
                flat: true
                text: qsTrId("About_OtherLinks_Repository.Text")
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl)
            }

            Button {
                flat: true
                text: qsTrId("About_OtherLinks_FAQ.Text")
                onClicked: Qt.openUrlExternally("https://github.com/Blinue/Magpie/wiki/FAQ")
            }

            Button {
                flat: true
                text: qsTrId("About_OtherLinks_ContributionGuidelines.Text")
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl + "/blob/HEAD/CONTRIBUTING.md")
            }

            Button {
                flat: true
                text: qsTrId("About_OtherLinks_License.Text")
                onClicked: Qt.openUrlExternally(page.vm.repositoryUrl + "/blob/HEAD/LICENSE")
            }
        }
    }
}
