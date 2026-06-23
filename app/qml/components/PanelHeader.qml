import QtQuick
import ResearchSSH

// A simple section header bar used at the top of each pane.
Rectangle {
    id: root
    property alias title: label.text
    height: 42
    color: Theme.panel

    Text {
        id: label
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: 14
        color: Theme.text
        font.pixelSize: 14
        font.bold: true
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }
}
