// The page of the default profile or of the profile of a program.
// Counterpart of ProfilePage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../controls"

PageFrame {
    id: page

    // The place of the profile; negative for the default profile
    property int profileIndex: -1
    readonly property var vm: app.profile(profileIndex)

    function toolTipText(name) {
        return qsTrId(name + ".[using:Windows.UI.Xaml.Controls]ToolTipService.ToolTip")
    }

    title: vm.name

    headerActions: [
        ToolButton {
            objectName: "profileLaunch"
            visible: !page.vm.isDefaultProfile
            enabled: page.vm.canLaunch
            icon.name: "media-playback-start"
            text: icon.name === "" ? page.toolTipText("Profile_Launch") : ""
            onClicked: page.vm.launch()

            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: page.toolTipText("Profile_Launch")
        },
        ToolButton {
            id: moreButton
            objectName: "profileMore"
            visible: !page.vm.isDefaultProfile
            icon.name: "application-menu"
            text: icon.name === "" ? "⋮" : ""
            onClicked: moreMenu.popup(this, 0, height)

            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: page.toolTipText("Profile_MoreOptions")

            Menu {
                id: moreMenu

                MenuItem {
                    text: qsTrId("Profile_MoreOptions_OpenProgramLocation.Text")
                    icon.name: "document-open-folder"
                    enabled: page.vm.canOpenProgramLocation
                    onTriggered: page.vm.openProgramLocation()
                }

                MenuItem {
                    text: qsTrId("Profile_MoreOptions_ChangeExecutableForLaunching.Text")
                    icon.name: "document-open"
                    onTriggered: launcherDialog.open()
                }

                MenuSeparator {
                }

                MenuItem {
                    text: qsTrId("Profile_MoreOptions_Rename.Text")
                    icon.name: "edit-rename"
                    onTriggered: {
                        renameField.text = page.vm.name
                        renamePopup.open()
                        renameField.selectAll()
                        renameField.forceActiveFocus()
                    }
                }

                MenuItem {
                    text: qsTrId("Profile_MoreOptions_Reorder.Text")
                    icon.name: "view-sort"
                    onTriggered: reorderPopup.open()
                }

                MenuItem {
                    text: qsTrId("Profile_MoreOptions_Delete.Text")
                    icon.name: "edit-delete"
                    onTriggered: deletePopup.open()
                }
            }

            Popup {
                id: renamePopup
                y: parent.height + 4
                x: parent.width - width
                padding: 16

                function accept() {
                    if (renameOkButton.enabled) {
                        app.root.renameProfile(page.profileIndex, renameField.text)
                        renamePopup.close()
                    }
                }

                contentItem: ColumnLayout {
                    spacing: 8

                    Label {
                        text: qsTrId("Profile_MoreOptions_RenameFlyout_Title.Text")
                    }

                    TextField {
                        id: renameField
                        Layout.preferredWidth: 280
                        selectByMouse: true
                        onAccepted: renamePopup.accept()
                    }

                    Button {
                        id: renameOkButton
                        Layout.alignment: Qt.AlignRight
                        Layout.topMargin: 8
                        highlighted: true
                        enabled: app.root.canRenameProfile(page.profileIndex, renameField.text)
                        text: qsTrId("Profile_MoreOptions_RenameFlyout_OK.Content")
                        onClicked: renamePopup.accept()
                    }
                }
            }

            Popup {
                id: reorderPopup
                y: parent.height + 4
                x: parent.width - width
                padding: 16

                contentItem: ColumnLayout {
                    spacing: 8

                    Label {
                        text: qsTrId("Profile_MoreOptions_ReorderFlyout_Title.Text")
                    }

                    Button {
                        Layout.fillWidth: true
                        icon.name: "go-up"
                        text: qsTrId("Profile_MoreOptions_ReorderFlyout_MoveUp.Text")
                        enabled: page.profileIndex > 0
                        onClicked: app.root.moveProfile(page.profileIndex, true)
                    }

                    Button {
                        Layout.fillWidth: true
                        icon.name: "go-down"
                        text: qsTrId("Profile_MoreOptions_ReorderFlyout_MoveDown.Text")
                        enabled: page.profileIndex + 1 < app.root.profiles.length
                        onClicked: app.root.moveProfile(page.profileIndex, false)
                    }
                }
            }

            Popup {
                id: deletePopup
                y: parent.height + 4
                x: parent.width - width
                padding: 16

                contentItem: ColumnLayout {
                    spacing: 12

                    Label {
                        Layout.maximumWidth: 280
                        text: qsTrId("Profile_MoreOptions_DeleteFlyout.Text")
                        wrapMode: Text.WordWrap
                    }

                    Button {
                        objectName: "profileDeleteConfirm"
                        Layout.alignment: Qt.AlignRight
                        icon.name: "edit-delete"
                        text: qsTrId("Profile_MoreOptions_Delete.Text")
                        onClicked: {
                            deletePopup.close()
                            app.root.removeProfile(page.profileIndex)
                        }
                    }
                }
            }
        }
    ]

    FileDialog {
        id: launcherDialog
        title: qsTrId("Dialog_SelectLauncher_Title")
        fileMode: FileDialog.OpenFile
        currentFolder: page.vm.launcherFolder
        onAccepted: page.vm.changeLauncher(selectedFile)
    }

    FolderDialog {
        id: screenshotDirectoryDialog
        title: qsTrId("Dialog_SelectScreenshotSaveDirectory_Title")
        onAccepted: page.vm.changeScreenshotSaveDirectory(selectedFolder)
    }

    SettingsGroup {
        header: qsTrId("Profile_General.Header")

        SettingsCard {
            header: qsTrId("Profile_General_ScalingMode.Header")

            ComboBox {
                objectName: "profileScalingMode"
                implicitContentWidthPolicy: ComboBox.WidestText
                model: page.vm.scalingModes
                currentIndex: page.vm.scalingMode
                onActivated: (index) => page.vm.scalingMode = index
            }
        }

        SettingsCard {
            visible: !page.vm.isDefaultProfile
            header: qsTrId("Profile_General_AutoScale.Header")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [
                    qsTrId("Profile_General_AutoScale_Disabled.Content"),
                    qsTrId("Profile_General_AutoScale_Fullscreen.Content"),
                    qsTrId("Profile_General_AutoScale_Windowed.Content")
                ]
                currentIndex: page.vm.autoScale
                onActivated: (index) => page.vm.autoScale = index
            }
        }

        SettingsCard {
            header: qsTrId("Profile_General_3DGameMode.Header")
            description: qsTrId("Profile_General_3DGameMode.Description")

            Switch {
                objectName: "profile3DGameMode"
                checked: page.vm.is3DGameMode
                onToggled: page.vm.is3DGameMode = checked
            }
        }

        SettingsCard {
            visible: page.vm.hasMultipleMonitors
            header: qsTrId("Profile_General_Multimonitor.Header")
            description: qsTrId("Profile_General_Multimonitor.Description")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [
                    qsTrId("Profile_General_Multimonitor_Closest.Content"),
                    qsTrId("Profile_General_Multimonitor_Intersected.Content"),
                    qsTrId("Profile_General_Multimonitor_All.Content")
                ]
                currentIndex: page.vm.multiMonitorUsage
                onActivated: (index) => page.vm.multiMonitorUsage = index
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_WindowedScaling.Header")

        SettingsCard {
            header: qsTrId("Profile_WindowedScaling_InitialScaleFactor.Header")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [
                    qsTrId("Profile_WindowedScaling_InitialScaleFactor_Auto.Content"),
                    "1.25x", "1.5x", "1.75x", "2x", "3x",
                    qsTrId("Profile_WindowedScaling_InitialScaleFactor_Custom.Content")
                ]
                currentIndex: page.vm.initialWindowedScaleFactor
                onActivated: (index) => page.vm.initialWindowedScaleFactor = index
            }

            NumberField {
                Layout.preferredWidth: 70
                visible: page.vm.initialWindowedScaleFactor === 6
                value: page.vm.customInitialWindowedScaleFactor
                onEdited: (newValue) => page.vm.customInitialWindowedScaleFactor = newValue
            }

            Label {
                visible: page.vm.initialWindowedScaleFactor === 6
                text: "x"
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_Toolbar.Header")

        SettingsExpander {
            header: qsTrId("Profile_Toolbar_InitialState.Header")

            headerContent: Label {
                text: page.vm.initialToolbarStateDescription
                opacity: 0.7
            }

            ExpanderItem {
                header: qsTrId("Profile_Toolbar_InitialState_Fullscreen.Header")

                ComboBox {
                    implicitContentWidthPolicy: ComboBox.WidestText
                    model: [
                        qsTrId("Profile_Toolbar_InitialState_Off.Content"),
                        qsTrId("Profile_Toolbar_InitialState_AlwaysShow.Content"),
                        qsTrId("Profile_Toolbar_InitialState_AutoHide.Content")
                    ]
                    currentIndex: page.vm.fullscreenInitialToolbarState
                    onActivated: (index) => page.vm.fullscreenInitialToolbarState = index
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_Toolbar_InitialState_Windowed.Header")

                ComboBox {
                    implicitContentWidthPolicy: ComboBox.WidestText
                    model: [
                        qsTrId("Profile_Toolbar_InitialState_Off.Content"),
                        qsTrId("Profile_Toolbar_InitialState_AlwaysShow.Content"),
                        qsTrId("Profile_Toolbar_InitialState_AutoHide.Content")
                    ]
                    currentIndex: page.vm.windowedInitialToolbarState
                    onActivated: (index) => page.vm.windowedInitialToolbarState = index
                }
            }
        }

        SettingsCard {
            header: qsTrId("Profile_Toolbar_ScreenshotSaveDirectory.Header")
            description: page.vm.screenshotSaveDirectory

            Button {
                icon.name: "document-open-folder"
                text: icon.name === "" ? "↗" : ""
                onClicked: page.vm.openScreenshotSaveDirectory()
            }

            Button {
                text: qsTrId("Profile_Toolbar_ScreenshotSaveDirectory_Change.Content")
                onClicked: {
                    screenshotDirectoryDialog.currentFolder = "file://" + page.vm.screenshotSaveDirectory
                    screenshotDirectoryDialog.open()
                }
            }
        }

        SettingsCard {
            header: qsTrId("Profile_Toolbar_ScreenshotFilenameTemplate.Header")
            description: qsTrId("Profile_Toolbar_ScreenshotFilenameTemplate.Description")

            TextField {
                id: templateField
                objectName: "profileScreenshotTemplate"
                Layout.preferredWidth: 240
                selectByMouse: true
                text: page.vm.screenshotFilenameTemplate
                onEditingFinished: page.vm.screenshotFilenameTemplate = text
            }

            ToolButton {
                icon.name: "help-about"
                text: icon.name === "" ? "?" : ""
                onClicked: templateTipPopup.open()

                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: page.toolTipText("Profile_Toolbar_ScreenshotFilenameTemplate_TipButton")

                Popup {
                    id: templateTipPopup
                    y: parent.height + 4
                    x: parent.width - width
                    padding: 16

                    contentItem: ColumnLayout {
                        spacing: 4

                        Label {
                            Layout.bottomMargin: 8
                            text: qsTrId("Profile_Toolbar_ScreenshotFilenameTemplate_Tip_Header.Text")
                            font.weight: Font.Bold
                        }

                        Repeater {
                            model: [
                                ["%WT%", "WindowTitle"], ["%WT:n%", "WindowTitle2"],
                                ["%PN%", "ProcessName"], ["%PN:n%", "ProcessName2"],
                                ["%Y%", "Year"], ["%y%", "ShortYear"], ["%m%", "Month"], ["%D%", "Day"],
                                ["%H%", "Hour"], ["%M%", "Minute"], ["%S%", "Second"], ["%MS%", "Millisecond"],
                                ["\\%", "Escape"]
                            ]

                            RowLayout {
                                id: tipRow

                                required property var modelData

                                spacing: 6

                                Label {
                                    text: tipRow.modelData[0]
                                    font.family: "monospace"
                                    font.weight: Font.Bold
                                }

                                Label {
                                    text: "- " + qsTrId("Profile_Toolbar_ScreenshotFilenameTemplate_Tip_" + tipRow.modelData[1] + ".Text")
                                }
                            }
                        }
                    }
                }
            }
        }

        InfoBar {
            visible: !page.vm.isScreenshotFilenameTemplateValid
            severity: InfoBar.Severity.Error
            title: qsTrId("Profile_Toolbar_ScreenshotFilenameTemplateErrorInfoBar.Title")
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_Performance.Header")

        SettingsCard {
            visible: page.vm.isShowGraphicsCardSettingsCard
            header: qsTrId("Profile_Performance_GraphicsCard.Header")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: page.vm.graphicsCards
                currentIndex: page.vm.graphicsCard
                onActivated: (index) => page.vm.graphicsCard = index
            }
        }

        InfoBar {
            visible: page.vm.isNoGraphicsCard
            severity: InfoBar.Severity.Warning
            title: qsTrId("Profile_Performance_NoGraphicsCard.Title")
        }

        SettingsExpander {
            id: frameRateExpander
            header: qsTrId("Profile_Performance_FrameRateLimiter.Header")
            expanded: page.vm.isFrameRateLimiterEnabled

            headerContent: Switch {
                objectName: "profileFrameRateLimiter"
                checked: page.vm.isFrameRateLimiterEnabled
                onToggled: {
                    page.vm.isFrameRateLimiterEnabled = checked
                    frameRateExpander.expanded = checked
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_Performance_FrameRateLimiter_MaxFrameRate.Header")

                NumberField {
                    objectName: "profileMaxFrameRate"
                    Layout.preferredWidth: 90
                    value: page.vm.maxFrameRate
                    onEdited: (newValue) => page.vm.maxFrameRate = newValue
                }

                Label {
                    text: "FPS"
                }
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_SourceWindow.Header")

        SettingsCard {
            header: qsTrId("Profile_SourceWindow_CaptureTitleBar.Header")

            Switch {
                checked: page.vm.isCaptureTitleBar
                onToggled: page.vm.isCaptureTitleBar = checked
            }
        }

        SettingsExpander {
            id: croppingExpander
            header: qsTrId("Profile_SourceWindow_CustomCropping.Header")
            expanded: page.vm.isCroppingEnabled

            headerContent: Switch {
                checked: page.vm.isCroppingEnabled
                onToggled: {
                    page.vm.isCroppingEnabled = checked
                    croppingExpander.expanded = checked
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_SourceWindow_CustomCropping_Left.Header")

                NumberField {
                    Layout.preferredWidth: 90
                    value: page.vm.croppingLeft
                    onEdited: (newValue) => page.vm.croppingLeft = newValue
                }

                Label {
                    text: qsTrId("Profile_SourceWindow_CustomCropping_Pixels.Text")
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_SourceWindow_CustomCropping_Right.Header")

                NumberField {
                    Layout.preferredWidth: 90
                    value: page.vm.croppingRight
                    onEdited: (newValue) => page.vm.croppingRight = newValue
                }

                Label {
                    text: qsTrId("Profile_SourceWindow_CustomCropping_Pixels.Text")
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_SourceWindow_CustomCropping_Top.Header")

                NumberField {
                    Layout.preferredWidth: 90
                    value: page.vm.croppingTop
                    onEdited: (newValue) => page.vm.croppingTop = newValue
                }

                Label {
                    text: qsTrId("Profile_SourceWindow_CustomCropping_Pixels.Text")
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_SourceWindow_CustomCropping_Bottom.Header")

                NumberField {
                    Layout.preferredWidth: 90
                    value: page.vm.croppingBottom
                    onEdited: (newValue) => page.vm.croppingBottom = newValue
                }

                Label {
                    text: qsTrId("Profile_SourceWindow_CustomCropping_Pixels.Text")
                }
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_Cursor.Header")

        SettingsExpander {
            header: qsTrId("Profile_Cursor_DrawCursor_ScaleFactor.Header")

            headerContent: [
                ComboBox {
                    implicitContentWidthPolicy: ComboBox.WidestText
                    model: [
                        "0.5x", "0.75x",
                        qsTrId("Profile_Cursor_DrawCursor_ScaleFactor_NoScaling.Content"),
                        "1.25x", "1.5x", "2x",
                        qsTrId("Profile_Cursor_DrawCursor_ScaleFactor_SameAsSourceWindow.Content"),
                        qsTrId("Profile_Cursor_DrawCursor_ScaleFactor_Custom.Content")
                    ]
                    currentIndex: page.vm.cursorScaling
                    onActivated: (index) => page.vm.cursorScaling = index
                },
                NumberField {
                    Layout.preferredWidth: 70
                    visible: page.vm.cursorScaling === 7
                    value: page.vm.customCursorScaleFactor
                    onEdited: (newValue) => page.vm.customCursorScaleFactor = newValue
                },
                Label {
                    visible: page.vm.cursorScaling === 7
                    text: "x"
                }
            ]

            ExpanderItem {
                header: qsTrId("Profile_Cursor_DrawCursor_Interpolation.Header")

                ComboBox {
                    implicitContentWidthPolicy: ComboBox.WidestText
                    model: [
                        qsTrId("Profile_Cursor_DrawCursor_Interpolation_NearestNeighbor.Content"),
                        qsTrId("Profile_Cursor_DrawCursor_Interpolation_Bilinear.Content")
                    ]
                    currentIndex: page.vm.cursorInterpolationMode
                    onActivated: (index) => page.vm.cursorInterpolationMode = index
                }
            }
        }

        SettingsExpander {
            id: autoHideExpander
            header: qsTrId("Profile_Cursor_AutoHide.Header")
            expanded: page.vm.isAutoHideCursorEnabled

            headerContent: Switch {
                checked: page.vm.isAutoHideCursorEnabled
                onToggled: {
                    page.vm.isAutoHideCursorEnabled = checked
                    autoHideExpander.expanded = checked
                }
            }

            ExpanderItem {
                header: qsTrId("Profile_Cursor_AutoHide_Delay.Header")

                Slider {
                    Layout.preferredWidth: 200
                    from: 0.1
                    to: 5
                    stepSize: 0.1
                    value: page.vm.autoHideCursorDelay
                    onMoved: page.vm.autoHideCursorDelay = value
                }

                Label {
                    Layout.minimumWidth: 24
                    horizontalAlignment: Text.AlignRight
                    text: app.formatNumber(page.vm.autoHideCursorDelay)
                }
            }
        }

        SettingsCard {
            header: qsTrId("Profile_Cursor_DrawCursor_AdjustCursorSpeed.Header")

            Switch {
                checked: page.vm.isAdjustCursorSpeed
                onToggled: page.vm.isAdjustCursorSpeed = checked
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Profile_Advanced.Header")

        SettingsCard {
            visible: !page.vm.isDefaultProfile
            header: qsTrId("Profile_Advanced_LaunchParameters.Header")

            TextField {
                Layout.preferredWidth: 300
                selectByMouse: true
                text: page.vm.launchParameters
                onEditingFinished: page.vm.launchParameters = text
            }
        }

        SettingsExpander {
            visible: !page.vm.isDefaultProfile
            header: qsTrId("Linux_Profile_LaunchNested.Header")
            description: page.vm.isNestedOnly ? qsTrId("Linux_Profile_LaunchNested.OnlyWay")
                : qsTrId("Linux_Profile_LaunchNested.Description")
            expanded: page.vm.isLaunchedNested || page.vm.isNestedOnly

            headerContent: Switch {
                objectName: "profileLaunchNested"
                // Where the desktop has no back end, this is how every
                // program is started
                enabled: !page.vm.isNestedOnly
                checked: page.vm.isLaunchedNested || page.vm.isNestedOnly
                onToggled: page.vm.isLaunchedNested = checked
            }

            ExpanderItem {
                header: qsTrId("Linux_Profile_NestedSize.Header")
                description: qsTrId("Linux_Profile_NestedSize.Description")

                RowLayout {
                    spacing: 8

                    NumberField {
                        objectName: "profileNestedWidth"
                        Layout.preferredWidth: 90
                        value: page.vm.nestedWidth
                        onEdited: (newValue) => page.vm.nestedWidth = newValue
                    }

                    Label {
                        text: "×"
                    }

                    NumberField {
                        objectName: "profileNestedHeight"
                        Layout.preferredWidth: 90
                        value: page.vm.nestedHeight
                        onEdited: (newValue) => page.vm.nestedHeight = newValue
                    }
                }
            }
        }

        SettingsCard {
            header: qsTrId("Profile_Advanced_OutputAlignment.Header")
            description: qsTrId("Profile_Advanced_OutputAlignment.Description")

            ComboBox {
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [
                    qsTrId("Profile_Advanced_OutputAlignment_TopLeft.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_TopCenter.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_TopRight.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_Left.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_Center.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_Right.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_BottomLeft.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_BottomCenter.Content"),
                    qsTrId("Profile_Advanced_OutputAlignment_BottomRight.Content")
                ]
                currentIndex: page.vm.outputAlignment
                onActivated: (index) => page.vm.outputAlignment = index
            }
        }
    }
}
