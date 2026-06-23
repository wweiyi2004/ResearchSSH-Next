import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// Left pane: the list of configured servers. Click to select, double-click (or
// the Connect button in the status pane) to connect.
Rectangle {
    id: root
    property var controller
    signal newConnectionRequested()
    color: Theme.window

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PanelHeader { title: "服务器"; Layout.fillWidth: true }

        // New-connection bar.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            color: Theme.window
            StyledButton {
                anchors.centerIn: parent
                width: parent.width - 20
                text: "＋ 新建连接"
                primary: true
                onClicked: root.newConnectionRequested()
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller.serverModel
            currentIndex: root.controller.selectedIndex
            boundsBehavior: Flickable.StopAtBounds

            delegate: ItemDelegate {
                id: row
                required property int index
                required property string name
                required property string endpoint
                required property int status
                required property string statusText

                width: ListView.view.width
                height: 66

                onClicked: root.controller.selectServer(row.index)
                onDoubleClicked: root.controller.connectToServer(row.index)

                background: Rectangle {
                    color: row.ListView.isCurrentItem ? Theme.treeSelected
                                                       : (row.hovered ? Theme.treeHover : "transparent")
                    Rectangle {
                        anchors.left: parent.left
                        width: 3
                        height: parent.height
                        color: Theme.accent
                        visible: row.ListView.isCurrentItem
                    }
                }

                contentItem: ColumnLayout {
                    spacing: 3
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: row.name
                            color: Theme.text
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        StatusDot { state: row.status }
                    }
                    Text {
                        text: row.endpoint
                        color: Theme.muted
                        font.pixelSize: 12
                        font.family: "Cascadia Mono"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        text: row.statusText
                        color: Theme.faint
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
