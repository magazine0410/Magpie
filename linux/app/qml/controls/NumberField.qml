// A field for a number with decimals. Counterpart of the NumberBox of WinUI
// as the Windows user interface uses it.
import QtQuick
import QtQuick.Controls

TextField {
    id: root

    // The number shown while the field is not being edited
    property real value: 0
    // The user entered a number; whoever owns `value` decides whether to
    // take it. Text that is no number is dropped.
    signal edited(real newValue)

    text: app.formatNumber(value)
    selectByMouse: true
    inputMethodHints: Qt.ImhFormattedNumbersOnly

    onEditingFinished: {
        const number = app.parseNumber(text)
        if (!isNaN(number)) {
            root.edited(number)
        }
        // Shows what was taken, or the old number again
        text = Qt.binding(() => app.formatNumber(root.value))
    }
}
