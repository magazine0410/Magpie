// Counterpart of HomePage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

PageFrame {
    id: page

    readonly property var vm: app.home

    title: qsTrId("Home_PageFrame.Title")

    InfoBar {
        visible: !page.vm.isEngineAvailable
        severity: InfoBar.Severity.Error
        title: qsTrId("Linux_Engine_Failed.Title")
        message: page.vm.engineError
    }

    InfoBar {
        visible: page.vm.isEngineAvailable && !page.vm.hasDesktopBackEnd
        severity: InfoBar.Severity.Warning
        title: qsTrId("Linux_NoBackEnd.Title")
        message: qsTrId("Linux_NoBackEnd.Message")
    }

    SettingsGroup {
        header: qsTrId("Home_Activation.Header")
        description: qsTrId("Home_Activation.Description")

        SettingsCard {
            header: qsTrId("Home_Activation_FullscreenScaling.Header")

            ShortcutView {
                key: page.vm.scaleShortcut
                available: page.vm.hasShortcuts
                canChange: page.vm.canConfigureShortcuts
                onChangeRequested: page.vm.configureShortcuts()
            }
        }

        SettingsCard {
            header: qsTrId("Home_Activation_WindowedScaling.Header")

            ShortcutView {
                key: page.vm.windowedScaleShortcut
                available: page.vm.hasShortcuts
                canChange: page.vm.canConfigureShortcuts
                onChangeRequested: page.vm.configureShortcuts()
            }
        }

        SettingsExpander {
            header: qsTrId("Home_Activation_Timer.Header")
            description: page.vm.timerDescription

            headerContent: [
                Label {
                    visible: page.vm.isTimerOn
                    text: page.vm.timerLabelText
                    font.weight: Font.DemiBold
                },
                ProgressBar {
                    visible: page.vm.isTimerOn
                    Layout.preferredWidth: 60
                    from: 0
                    to: 1
                    value: page.vm.timerProgress
                },
                Button {
                    objectName: "fullscreenTimerButton"
                    enabled: page.vm.isNotRunning && page.vm.isEngineAvailable
                    icon.name: "view-fullscreen"
                    text: page.vm.isFullscreenTimerOn ? qsTrId("Home_Activation_Timer_Cancel")
                        : qsTrId("Home_Activation_Timer_Start")
                    onClicked: page.vm.toggleTimer(false)

                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTrId("Profile_General_AutoScale_Fullscreen.Content")
                },
                Button {
                    objectName: "windowedTimerButton"
                    enabled: page.vm.isNotRunning && page.vm.isEngineAvailable
                    icon.name: "window"
                    text: page.vm.isWindowedTimerOn ? qsTrId("Home_Activation_Timer_Cancel")
                        : qsTrId("Home_Activation_Timer_Start")
                    onClicked: page.vm.toggleTimer(true)

                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTrId("Profile_General_AutoScale_Windowed.Content")
                }
            ]

            ExpanderItem {
                header: qsTrId("Home_Activation_Timer_Delay.Header")

                Slider {
                    Layout.preferredWidth: 200
                    from: 1
                    to: 5
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: page.vm.delay
                    onMoved: page.vm.delay = value
                }

                Label {
                    Layout.minimumWidth: 16
                    horizontalAlignment: Text.AlignRight
                    text: page.vm.delay
                }
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Home_Toolbar.Header")
        // The Windows text also says that the toolbar moves the scaled
        // window, which the title bar of the desktop does here
        description: qsTrId("Linux_Home_Toolbar.Description")

        SettingsCard {
            header: qsTrId("Home_Toolbar_StateToggle.Header")

            ShortcutView {
                key: page.vm.toolbarShortcut
                available: page.vm.hasShortcuts
                canChange: page.vm.canConfigureShortcuts
                onChangeRequested: page.vm.configureShortcuts()
            }
        }

        SettingsCard {
            header: qsTrId("Home_Toolbar_TakeScreenshot.Header")

            ShortcutView {
                key: page.vm.screenshotShortcut
                available: page.vm.hasShortcuts
                canChange: page.vm.canConfigureShortcuts
                onChangeRequested: page.vm.configureShortcuts()
            }
        }
    }

    SettingsGroup {
        header: qsTrId("Home_Advanced.Header")

        SettingsCard {
            header: qsTrId("Home_Advanced_AllowScalingMaximized.Header")
            description: qsTrId("Home_Advanced_AllowScalingMaximized.Description")

            Switch {
                checked: page.vm.isAllowScalingMaximized
                onToggled: page.vm.isAllowScalingMaximized = checked
            }
        }

        SettingsCard {
            header: qsTrId("Home_Advanced_KeepScreenOn.Header")

            Switch {
                checked: page.vm.isKeepScreenOn
                onToggled: page.vm.isKeepScreenOn = checked
            }
        }

        SettingsCard {
            header: qsTrId("Home_Advanced_SimulateExclusiveFullscreen.Header")
            description: qsTrId("Home_Advanced_SimulateExclusiveFullscreen.Description")

            Switch {
                checked: page.vm.isSimulateExclusiveFullscreen
                onToggled: page.vm.isSimulateExclusiveFullscreen = checked
            }
        }

        SettingsCard {
            header: qsTrId("Home_Advanced_InlineParams.Header")
            description: qsTrId("Home_Advanced_InlineParams.Description")

            Switch {
                checked: page.vm.isInlineParams
                onToggled: page.vm.isInlineParams = checked
            }
        }

        SettingsCard {
            header: qsTrId("Home_Advanced_MinFrameRate.Header")
            description: qsTrId("Home_Advanced_MinFrameRate.Description")

            ComboBox {
                model: page.vm.minFrameRateOptions
                currentIndex: page.vm.minFrameRateIndex
                onActivated: (index) => page.vm.minFrameRateIndex = index
            }

            Label {
                text: "FPS"
            }
        }

        SettingsExpander {
            visible: page.vm.isDeveloperMode
            expanded: true
            header: qsTrId("Home_Advanced_DeveloperOptions.Header")
            description: qsTrId("Home_Advanced_DeveloperOptions.Description")

            headerContent: Switch {
                checked: page.vm.isDeveloperMode
                onToggled: page.vm.isDeveloperMode = checked
            }

            ExpanderItem {
                header: qsTrId("Home_Advanced_DeveloperOptions_LocateLogs.Header")

                Button {
                    text: "Magpie"
                    onClicked: page.vm.locateLogs()
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_DebugMode.Content")
                    checked: page.vm.isDebugMode
                    onToggled: page.vm.isDebugMode = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_BenchmarkMode.Content")
                    checked: page.vm.isBenchmarkMode
                    onToggled: page.vm.isBenchmarkMode = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_DisableTopmost.Content")
                    checked: page.vm.isTopmostDisabled
                    onToggled: page.vm.isTopmostDisabled = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_DisableEffectCache.Content")
                    checked: page.vm.isEffectCacheDisabled
                    onToggled: page.vm.isEffectCacheDisabled = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_SaveEffectSources.Content")
                    checked: page.vm.isSaveEffectSources
                    onToggled: page.vm.isSaveEffectSources = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_WarningsAreErrors.Content")
                    checked: page.vm.isWarningsAreErrors
                    onToggled: page.vm.isWarningsAreErrors = checked
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    text: qsTrId("Home_Advanced_DeveloperOptions_DisableFP16.Content")
                    checked: page.vm.isFP16Disabled
                    onToggled: page.vm.isFP16Disabled = checked
                }
            }

            ExpanderItem {
                header: qsTrId("Home_Advanced_DeveloperOptions_DuplicateFrameDetection.Header")

                ComboBox {
                    model: [
                        qsTrId("Home_Advanced_DeveloperOptions_DuplicateFrameDetection_Always.Content"),
                        qsTrId("Home_Advanced_DeveloperOptions_DuplicateFrameDetection_Dynamic.Content"),
                        qsTrId("Home_Advanced_DeveloperOptions_DuplicateFrameDetection_Never.Content")
                    ]
                    currentIndex: page.vm.duplicateFrameDetectionMode
                    onActivated: (index) => page.vm.duplicateFrameDetectionMode = index
                }
            }

            ExpanderItem {
                CheckBox {
                    Layout.fillWidth: true
                    enabled: page.vm.isDynamicDetection
                    text: qsTrId("Home_Advanced_DeveloperOptions_EnableStatisticsForDynamicDetection.Content")
                    checked: page.vm.isStatisticsForDynamicDetectionEnabled
                    onToggled: page.vm.isStatisticsForDynamicDetectionEnabled = checked
                }
            }
        }
    }
}
