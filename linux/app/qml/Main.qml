// The main window: the list of pages on the left, the chosen page on the
// right. Counterpart of RootPage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"

ApplicationWindow {
    id: window

    // "home", "scalingModes", "settings", "about", or "profile:" and the
    // place of a profile, -1 for the defaults
    property string currentPage: ""

    onCurrentPageChanged: {
        if (currentPage.startsWith("profile:")) {
            pageLoader.setSource("pages/ProfilePage.qml", { profileIndex: parseInt(currentPage.substring(8)) })
        } else if (currentPage === "scalingModes") {
            pageLoader.setSource("pages/ScalingModesPage.qml")
        } else if (currentPage === "settings") {
            pageLoader.setSource("pages/SettingsPage.qml")
        } else if (currentPage === "about") {
            pageLoader.setSource("pages/AboutPage.qml")
        } else {
            pageLoader.setSource("pages/HomePage.qml")
        }
    }

    Component.onCompleted: currentPage = "home"

    title: "Magpie"
    width: 1000
    height: 720
    minimumWidth: 640
    minimumHeight: 420
    visible: false

    onClosing: app.onClosing()

    SystemPalette {
        id: systemPalette
    }

    // The profiles change places in the list; the page follows its profile
    Connections {
        target: app.root

        function onProfileAdded(profile) {
            window.currentPage = "profile:" + profile
        }

        function onProfileRemoved(profile) {
            if (window.currentPage.startsWith("profile:")) {
                // To the defaults, also for a page of another profile,
                // whose place may have changed. Set anew in any case, so
                // that the page does not keep the profile it showed.
                window.currentPage = ""
                window.currentPage = "profile:-1"
            }
        }

        function onProfileMoved(from, to) {
            if (window.currentPage === "profile:" + from) {
                window.currentPage = "profile:" + to
            } else if (window.currentPage === "profile:" + to) {
                window.currentPage = "profile:" + from
            }
        }
    }

    component NavigationItem: ItemDelegate {
        property string page

        Layout.fillWidth: true
        highlighted: page !== "" && window.currentPage === page
        onClicked: {
            if (page !== "") {
                window.currentPage = page
            }
        }
    }

    NewProfilePopup {
        id: newProfilePopup
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillHeight: true
            Layout.preferredWidth: 230
            padding: 6

            ColumnLayout {
                anchors.fill: parent
                spacing: 2

                NavigationItem {
                    objectName: "navHome"
                    page: "home"
                    icon.name: "go-home"
                    text: qsTrId("Root_Home.Content")
                }

                NavigationItem {
                    objectName: "navScalingModes"
                    page: "scalingModes"
                    icon.name: "transform-scale"
                    text: qsTrId("Root_ScalingModes.Content")
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.leftMargin: 8
                    Layout.bottomMargin: 2
                    text: qsTrId("Root_Profiles.Content")
                    font.weight: Font.DemiBold
                    opacity: 0.7
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 2

                        NavigationItem {
                            objectName: "navDefaults"
                            page: "profile:-1"
                            icon.name: "preferences-other"
                            text: qsTrId("Root_Defaults.Content")
                        }

                        Repeater {
                            model: app.root.profiles

                            delegate: NavigationItem {
                                id: profileItem

                                required property var modelData
                                required property int index

                                objectName: "navProfile" + index
                                page: "profile:" + index
                                // A .desktop file names an icon of the
                                // theme or a file
                                icon.name: modelData.icon.startsWith("/") ? "" : modelData.icon
                                icon.source: modelData.icon.startsWith("/") ? "file://" + modelData.icon : ""
                                // An icon of a program keeps its colours
                                icon.color: "transparent"
                                text: modelData.name

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: profileMenu.popup()
                                }

                                Menu {
                                    id: profileMenu

                                    MenuItem {
                                        text: qsTrId("Root_ProfileFlyout_Launch.Text")
                                        icon.name: "media-playback-start"
                                        enabled: profileItem.modelData.canLaunch
                                        onTriggered: app.root.launch(profileItem.index)
                                    }

                                    MenuItem {
                                        text: qsTrId("Profile_MoreOptions_OpenProgramLocation.Text")
                                        icon.name: "document-open-folder"
                                        enabled: app.root.canOpenProgramLocation(profileItem.index)
                                        onTriggered: app.root.openProgramLocation(profileItem.index)
                                    }

                                    MenuItem {
                                        text: qsTrId("Profile_MoreOptions_Delete.Text")
                                        icon.name: "edit-delete"
                                        onTriggered: deleteProfilePopup.open()
                                    }
                                }

                                Popup {
                                    id: deleteProfilePopup
                                    x: parent.width
                                    padding: 16

                                    contentItem: ColumnLayout {
                                        spacing: 12

                                        Label {
                                            Layout.maximumWidth: 280
                                            text: qsTrId("Root_ProfileFlyout_DeleteConfirmationText").replace("{}", profileItem.modelData.name)
                                            wrapMode: Text.WordWrap
                                        }

                                        Button {
                                            Layout.alignment: Qt.AlignRight
                                            icon.name: "edit-delete"
                                            text: qsTrId("Profile_MoreOptions_Delete.Text")
                                            onClicked: {
                                                deleteProfilePopup.close()
                                                app.root.removeProfile(profileItem.index)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        NavigationItem {
                            objectName: "navNewProfile"
                            icon.name: "list-add"
                            text: qsTrId("Root_NewProfile.Content")
                            onClicked: newProfilePopup.open()
                        }
                    }
                }

                NavigationItem {
                    objectName: "navSettings"
                    page: "settings"
                    icon.name: "configure"
                    text: qsTrId("Linux_Settings.Title")
                }

                NavigationItem {
                    objectName: "navAbout"
                    page: "about"
                    icon.name: "help-about"
                    text: qsTrId("Root_About.Content")
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 1
            color: Qt.rgba(systemPalette.text.r, systemPalette.text.g, systemPalette.text.b, 0.15)
        }

        Loader {
            id: pageLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
