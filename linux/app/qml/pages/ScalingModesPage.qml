// Counterpart of ScalingModesPage of the Windows user interface.
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../controls"

PageFrame {
    id: page

    readonly property var vm: app.scalingModes

    // The effects for the menu: folders, and the effects outside folders
    readonly property var effectMenu: vm.effectMenu()
    readonly property var effectFolders: effectMenu.filter(entry => entry.folder !== undefined)
    readonly property var rootEffects: effectMenu.filter(entry => entry.folder === undefined)

    // The texts of tool tips have long names in the files of the Windows
    // user interface
    function toolTipText(name) {
        return qsTrId(name + ".[using:Windows.UI.Xaml.Controls]ToolTipService.ToolTip")
    }

    title: qsTrId("ScalingModes_PageFrame.Title")

    headerActions: ToolButton {
        icon.name: "application-menu"
        text: icon.name === "" ? "⋮" : ""
        onClicked: generalMenu.popup(this, 0, height)

        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: page.toolTipText("ScalingModes_General_MoreOptions")

        Menu {
            id: generalMenu

            MenuItem {
                text: qsTrId("ScalingModes_General_Export.Text")
                icon.name: "document-export"
                onTriggered: exportDialog.open()
            }

            MenuItem {
                text: qsTrId("ScalingModes_General_Import.Text")
                icon.name: "document-import"
                onTriggered: importDialog.open()
            }
        }
    }

    FileDialog {
        id: exportDialog
        title: qsTrId("Dialog_Export_Title")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTrId("Dialog_JsonFile") + " (*.json)"]
        defaultSuffix: "json"
        selectedFile: currentFolder + "/ScalingModes.json"
        onAccepted: {
            messageBar.title = qsTrId("Message_ExportScalingModesFailed")
            messageBar.visible = !page.vm.exportTo(selectedFile)
        }
    }

    FileDialog {
        id: importDialog
        title: qsTrId("Dialog_Import_Title")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTrId("Dialog_JsonFile") + " (*.json)"]
        onAccepted: {
            messageBar.title = qsTrId("Message_ImportScalingModesFailed")
            messageBar.visible = !page.vm.importFrom(selectedFile)
        }
    }

    // One menu for all scaling modes; it adds to the one it was opened for
    Menu {
        id: addEffectMenu

        property int modeIndex: -1

        Instantiator {
            model: page.effectFolders
            delegate: Menu {
                id: folderMenu

                required property var modelData

                title: modelData.folder

                Instantiator {
                    model: folderMenu.modelData.items
                    delegate: MenuItem {
                        required property var modelData

                        text: modelData.name
                        onTriggered: page.vm.addEffect(addEffectMenu.modeIndex, modelData.fullName)
                    }
                    onObjectAdded: (index, object) => folderMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => folderMenu.removeItem(object)
                }
            }
            onObjectAdded: (index, object) => addEffectMenu.insertMenu(index, object)
            onObjectRemoved: (index, object) => addEffectMenu.removeMenu(object)
        }

        Instantiator {
            model: page.rootEffects
            delegate: MenuItem {
                required property var modelData

                text: modelData.name
                onTriggered: page.vm.addEffect(addEffectMenu.modeIndex, modelData.fullName)
            }
            onObjectAdded: (index, object) => addEffectMenu.insertItem(page.effectFolders.length + index, object)
            onObjectRemoved: (index, object) => addEffectMenu.removeItem(object)
        }
    }

    InfoBar {
        id: messageBar
        visible: false
        severity: InfoBar.Severity.Error
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 4

        Repeater {
            model: page.vm.count

            delegate: SettingsExpander {
                id: modeItem

                required property int index
                readonly property int effectCount: {
                    page.vm.revision
                    return page.vm.effectCount(index)
                }

                objectName: "scalingMode" + index
                header: {
                    page.vm.revision
                    return page.vm.modeName(index)
                }
                description: {
                    page.vm.revision
                    return page.vm.modeDescription(index)
                }

                Component.onCompleted: expanded = page.vm.isExpanded(index)
                onExpandedChanged: page.vm.setExpanded(index, expanded)

                Connections {
                    target: page.vm

                    function onModesReordered() {
                        modeItem.expanded = page.vm.isExpanded(modeItem.index)
                    }
                }

                headerContent: [
                    Button {
                        id: renameButton
                        objectName: "rename" + modeItem.index
                        icon.name: "edit-rename"
                        text: icon.name === "" ? qsTrId("ScalingModes_RenameFlyout_Title.Text") : ""
                        onClicked: {
                            renameField.text = page.vm.modeName(modeItem.index)
                            renamePopup.open()
                            renameField.selectAll()
                            renameField.forceActiveFocus()
                        }

                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: page.toolTipText("ScalingModes_Rename")

                        Popup {
                            id: renamePopup
                            y: parent.height + 4
                            x: parent.width - width
                            padding: 16

                            function accept() {
                                if (renameOkButton.enabled) {
                                    page.vm.renameMode(modeItem.index, renameField.text)
                                    renamePopup.close()
                                }
                            }

                            contentItem: ColumnLayout {
                                spacing: 8

                                Label {
                                    text: qsTrId("ScalingModes_RenameFlyout_Title.Text")
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
                                    enabled: page.vm.canRenameMode(modeItem.index, renameField.text)
                                    text: qsTrId("ScalingModes_RenameFlyout_OK.Content")
                                    onClicked: renamePopup.accept()
                                }
                            }
                        }
                    },
                    Button {
                        id: moreButton
                        objectName: "more" + modeItem.index
                        icon.name: "overflow-menu"
                        text: icon.name === "" ? "⋯" : ""
                        onClicked: modeMenu.popup(this, 0, height)

                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: page.toolTipText("ScalingModes_MoreOptions")

                        Menu {
                            id: modeMenu

                            MenuItem {
                                text: qsTrId("ScalingModes_MoreOptionsFlyout_MoveUp.Text")
                                icon.name: "go-up"
                                enabled: modeItem.index > 0
                                onTriggered: page.vm.moveMode(modeItem.index, true)
                            }

                            MenuItem {
                                text: qsTrId("ScalingModes_MoreOptionsFlyout_MoveDown.Text")
                                icon.name: "go-down"
                                enabled: modeItem.index + 1 < page.vm.count
                                onTriggered: page.vm.moveMode(modeItem.index, false)
                            }

                            MenuSeparator {
                            }

                            MenuItem {
                                text: qsTrId("ScalingModes_MoreOptionsFlyout_Delete.Text")
                                icon.name: "edit-delete"
                                onTriggered: {
                                    // A scaling mode that profiles use is
                                    // asked about first
                                    deletePopup.profiles = page.vm.linkedProfiles(modeItem.index)
                                    if (deletePopup.profiles.length > 0) {
                                        deletePopup.open()
                                    } else {
                                        page.vm.removeMode(modeItem.index)
                                    }
                                }
                            }
                        }

                        Popup {
                            id: deletePopup

                            property var profiles: []

                            y: parent.height + 4
                            x: parent.width - width
                            padding: 16

                            contentItem: ColumnLayout {
                                spacing: 8

                                Label {
                                    Layout.maximumWidth: 280
                                    text: qsTrId("ScalingModes_DeleteFlyout_Title.Text")
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.WordWrap
                                }

                                Label {
                                    Layout.maximumWidth: 280
                                    Layout.topMargin: 8
                                    text: qsTrId("ScalingModes_DeleteFlyout_Description.Text")
                                    wrapMode: Text.WordWrap
                                }

                                Repeater {
                                    model: deletePopup.profiles

                                    Label {
                                        required property string modelData

                                        Layout.maximumWidth: 280
                                        text: modelData
                                        font.italic: true
                                        elide: Text.ElideRight
                                    }
                                }

                                Button {
                                    Layout.alignment: Qt.AlignRight
                                    Layout.topMargin: 8
                                    icon.name: "edit-delete"
                                    text: qsTrId("ScalingModes_DeleteFlyout_Delete.Text")
                                    onClicked: {
                                        deletePopup.close()
                                        page.vm.removeMode(modeItem.index)
                                    }
                                }
                            }
                        }
                    }
                ]

                InfoBar {
                    visible: {
                        page.vm.revision
                        return page.vm.hasUnknownEffects(modeItem.index)
                    }
                    radius: 0
                    border.width: 0
                    severity: InfoBar.Severity.Warning
                    title: qsTrId("ScalingModes_HasUnkownEffects.Title")
                }

                Repeater {
                    model: modeItem.effectCount

                    delegate: ExpanderItem {
                        id: effectItem

                        required property int index
                        // How far the effect has been dragged
                        property real dragOffset: 0

                        objectName: "effect" + modeItem.index + "_" + index
                        header: {
                            page.vm.revision
                            return page.vm.effectName(modeItem.index, index)
                        }
                        z: dragHandler.active ? 1 : 0
                        opacity: dragHandler.active ? 0.8 : 1
                        transform: Translate {
                            y: effectItem.dragOffset
                        }

                        // Dragging an effect up or down gives it another
                        // place among the effects
                        handlers: DragHandler {
                            id: dragHandler
                            target: null
                            enabled: modeItem.effectCount > 1
                            xAxis.enabled: false
                            onActiveTranslationChanged: {
                                if (active) {
                                    effectItem.dragOffset = activeTranslation.y
                                }
                            }
                            onActiveChanged: {
                                if (!active) {
                                    const places = Math.round(effectItem.dragOffset / effectItem.height)
                                    const target = Math.max(0, Math.min(modeItem.effectCount - 1, effectItem.index + places))
                                    effectItem.dragOffset = 0
                                    page.vm.moveEffect(modeItem.index, effectItem.index, target)
                                }
                            }
                        }

                        Button {
                            id: scaleButton
                            objectName: "scale" + modeItem.index + "_" + effectItem.index
                            visible: {
                                page.vm.revision
                                return page.vm.effectCanScale(modeItem.index, effectItem.index)
                            }
                            icon.name: "transform-scale"
                            text: icon.name === "" ? page.toolTipText("ScalingModes_Scale") : ""
                            onClicked: scalePopup.open()

                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: page.toolTipText("ScalingModes_Scale")

                            Popup {
                                id: scalePopup

                                readonly property int type: {
                                    page.vm.revision
                                    return page.vm.scalingType(modeItem.index, effectItem.index)
                                }
                                readonly property real scaleX: {
                                    page.vm.revision
                                    return page.vm.scaleX(modeItem.index, effectItem.index)
                                }
                                readonly property real scaleY: {
                                    page.vm.revision
                                    return page.vm.scaleY(modeItem.index, effectItem.index)
                                }

                                y: parent.height + 4
                                x: parent.width - width
                                padding: 16

                                contentItem: ColumnLayout {
                                    spacing: 8

                                    Label {
                                        text: qsTrId("ScalingModes_ScaleFlyout_Type.Text")
                                    }

                                    ComboBox {
                                        id: typeBox
                                        Layout.preferredWidth: 280
                                        textRole: "name"
                                        model: [
                                            {
                                                name: qsTrId("ScalingModes_ScaleFlyout_Type_Factor"),
                                                description: qsTrId("ScalingModes_ScaleFlyout_Type_Factor_Description")
                                            },
                                            {
                                                name: qsTrId("ScalingModes_ScaleFlyout_Type_Fit"),
                                                description: qsTrId("ScalingModes_ScaleFlyout_Type_Fit_Description")
                                            },
                                            {
                                                name: qsTrId("ScalingModes_ScaleFlyout_Type_Absolute"),
                                                description: qsTrId("ScalingModes_ScaleFlyout_Type_Absolute_Description")
                                            },
                                            {
                                                name: qsTrId("ScalingModes_ScaleFlyout_Type_Fill"),
                                                description: qsTrId("ScalingModes_ScaleFlyout_Type_Fill_Description")
                                            }
                                        ]
                                        currentIndex: scalePopup.type
                                        onActivated: (index) => page.vm.setScalingType(modeItem.index, effectItem.index, index)

                                        delegate: ItemDelegate {
                                            id: typeDelegate

                                            required property var modelData
                                            required property int index

                                            width: typeBox.width
                                            highlighted: typeBox.highlightedIndex === index

                                            contentItem: ColumnLayout {
                                                spacing: 0

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: typeDelegate.modelData.name
                                                }

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: typeDelegate.modelData.description
                                                    opacity: 0.7
                                                    font.pointSize: Application.font.pointSize * 0.9
                                                    wrapMode: Text.WordWrap
                                                }
                                            }
                                        }
                                    }

                                    // Factors, or a size in pixels
                                    Label {
                                        Layout.topMargin: 8
                                        visible: scalePopup.type !== 3
                                        text: scalePopup.type === 2 ? qsTrId("ScalingModes_ScaleFlyout_WidthPixels.Text")
                                            : qsTrId("ScalingModes_ScaleFlyout_WidthFactor.Text")
                                    }

                                    NumberField {
                                        Layout.fillWidth: true
                                        visible: scalePopup.type !== 3
                                        value: scalePopup.scaleX
                                        onEdited: (newValue) => page.vm.setScaleX(modeItem.index, effectItem.index, newValue)
                                    }

                                    Label {
                                        Layout.topMargin: 8
                                        visible: scalePopup.type !== 3
                                        text: scalePopup.type === 2 ? qsTrId("ScalingModes_ScaleFlyout_HeightPixels.Text")
                                            : qsTrId("ScalingModes_ScaleFlyout_HeightFactor.Text")
                                    }

                                    NumberField {
                                        Layout.fillWidth: true
                                        visible: scalePopup.type !== 3
                                        value: scalePopup.scaleY
                                        onEdited: (newValue) => page.vm.setScaleY(modeItem.index, effectItem.index, newValue)
                                    }
                                }
                            }
                        }

                        Button {
                            id: parametersButton
                            objectName: "parameters" + modeItem.index + "_" + effectItem.index
                            visible: {
                                page.vm.revision
                                return page.vm.effectHasParameters(modeItem.index, effectItem.index)
                            }
                            icon.name: "configure"
                            text: icon.name === "" ? page.toolTipText("ScalingModes_Parameters") : ""
                            onClicked: parametersPopup.open()

                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: page.toolTipText("ScalingModes_Parameters")

                            Popup {
                                id: parametersPopup

                                // Read anew after every change; the controls
                                // below stay, so that a slider can be dragged
                                readonly property var parameters: {
                                    page.vm.revision
                                    return page.vm.parameters(modeItem.index, effectItem.index)
                                }

                                y: parent.height + 4
                                x: parent.width - width
                                padding: 16

                                contentItem: ColumnLayout {
                                    spacing: 8

                                    Repeater {
                                        model: parametersPopup.parameters.length

                                        delegate: ColumnLayout {
                                            id: parameterItem

                                            required property int index
                                            readonly property var parameter: parametersPopup.parameters[index]

                                            Layout.preferredWidth: 280
                                            spacing: 2

                                            CheckBox {
                                                visible: parameterItem.parameter.isBool
                                                text: parameterItem.parameter.label
                                                checked: parameterItem.parameter.value !== 0
                                                onToggled: page.vm.setParameter(modeItem.index, effectItem.index,
                                                    parameterItem.index, checked ? 1 : 0)
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                visible: !parameterItem.parameter.isBool

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: parameterItem.parameter.label
                                                    wrapMode: Text.WordWrap
                                                }

                                                Label {
                                                    text: app.formatNumber(parameterItem.parameter.value)
                                                }
                                            }

                                            Slider {
                                                Layout.fillWidth: true
                                                visible: !parameterItem.parameter.isBool
                                                from: parameterItem.parameter.minimum
                                                to: parameterItem.parameter.maximum
                                                stepSize: parameterItem.parameter.step
                                                snapMode: Slider.SnapAlways
                                                value: parameterItem.parameter.value
                                                onMoved: page.vm.setParameter(modeItem.index, effectItem.index,
                                                    parameterItem.index, value)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Button {
                            objectName: "remove" + modeItem.index + "_" + effectItem.index
                            icon.name: "edit-delete"
                            text: icon.name === "" ? page.toolTipText("ScalingModes_Delete") : ""
                            onClicked: page.vm.removeEffect(modeItem.index, effectItem.index)

                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: page.toolTipText("ScalingModes_Delete")
                        }
                    }
                }

                ExpanderItem {
                    description: modeItem.effectCount > 1 ? qsTrId("ScalingModes_DragToReorder.Text") : ""

                    Button {
                        objectName: "addEffect" + modeItem.index
                        flat: true
                        icon.name: "list-add"
                        text: qsTrId("ScalingModes_AddEffect.Text")
                        onClicked: {
                            addEffectMenu.modeIndex = modeItem.index
                            addEffectMenu.popup(this, 0, height)
                        }
                    }
                }
            }
        }
    }

    Button {
        objectName: "newScalingMode"
        Layout.alignment: Qt.AlignRight
        highlighted: true
        icon.name: "list-add"
        text: qsTrId("ScalingModes_NewScalingMode.Text")
        onClicked: {
            newModeName.text = ""
            copyFromBox.model = page.vm.copyFromList()
            copyFromBox.currentIndex = 0
            newModePopup.open()
            newModeName.forceActiveFocus()
        }

        Popup {
            id: newModePopup
            y: -height - 4
            x: parent.width - width
            padding: 16

            function accept() {
                if (createButton.enabled) {
                    page.vm.addMode(newModeName.text, copyFromBox.currentIndex)
                    newModePopup.close()
                }
            }

            contentItem: ColumnLayout {
                spacing: 8

                Label {
                    Layout.bottomMargin: 8
                    text: qsTrId("ScalingModes_NewScalingModeFlyout_Title.Text")
                    font.pointSize: Application.font.pointSize * 1.3
                    font.weight: Font.DemiBold
                }

                Label {
                    text: qsTrId("ScalingModes_NewScalingModeFlyout_Name.Text")
                }

                TextField {
                    id: newModeName
                    Layout.preferredWidth: 260
                    selectByMouse: true
                    onAccepted: newModePopup.accept()
                }

                Label {
                    Layout.topMargin: 8
                    text: qsTrId("ScalingModes_NewScalingModeFlyout_CopyFrom.Text")
                }

                ComboBox {
                    id: copyFromBox
                    Layout.fillWidth: true
                }

                Button {
                    id: createButton
                    Layout.alignment: Qt.AlignRight
                    Layout.topMargin: 12
                    highlighted: true
                    enabled: page.vm.canAddMode(newModeName.text)
                    text: qsTrId("ScalingModes_NewScalingModeFlyout_Create.Content")
                    onClicked: newModePopup.accept()
                }
            }
        }
    }
}
