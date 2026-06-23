import QtQuick
import QtQuick.Controls.Basic
import ResearchSSH

// Dark-themed button used throughout the UI.
Button {
    id: control
    property color accent: Theme.accent
    property bool primary: false

    implicitHeight: 34
    implicitWidth: Math.max(96, contentText.implicitWidth + 24)
    padding: 8
    font.pixelSize: 13

    contentItem: Text {
        id: contentText
        text: control.text
        color: control.enabled
               ? (control.primary ? "#ffffff" : Theme.text)
               : Theme.faint
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 6
        color: control.primary
               ? (control.down ? Qt.darker(control.accent, 1.25) : control.accent)
               : (control.down ? Theme.buttonDown : Theme.button)
        border.color: control.primary ? control.accent : Theme.border
        border.width: 1
        opacity: control.enabled ? 1.0 : 0.5
    }
}
