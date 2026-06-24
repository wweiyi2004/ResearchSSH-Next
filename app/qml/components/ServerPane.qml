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
    property int pendingDeleteIndex: -1
    property string pendingDeleteName: ""
    property string pendingDeleteEndpoint: ""
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
                text: "＋ 添加服务器"
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
                height: 70

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

                contentItem: RowLayout {
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
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
                    StyledButton {
                        text: "删除"
                        implicitWidth: 48
                        enabled: row.hovered || row.ListView.isCurrentItem
                        opacity: enabled ? 1 : 0
                        onClicked: {
                            root.pendingDeleteIndex = row.index
                            root.pendingDeleteName = row.name
                            root.pendingDeleteEndpoint = row.endpoint
                            deleteServerDialog.open()
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 32
                visible: list.count === 0
                text: "暂无服务器\n点击上方“添加服务器”添加 SSH 服务器"
                color: Theme.faint
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.35
            }
        }
    }

    Dialog {
        id: deleteServerDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 360
        padding: 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.panel
            radius: 10
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                text: "删除服务器"
                color: Theme.text
                font.pixelSize: 16
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                text: "将删除左侧服务器配置：\n" + root.pendingDeleteName
                      + "\n" + root.pendingDeleteEndpoint
                color: Theme.textSoft
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                text: "如果这是当前连接，会先断开并清空对应文件树。"
                color: Theme.faint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 8
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "取消"
                    onClicked: deleteServerDialog.close()
                }
                StyledButton {
                    text: "删除"
                    primary: true
                    onClicked: {
                        root.controller.deleteServer(root.pendingDeleteIndex)
                        deleteServerDialog.close()
                    }
                }
            }
        }
    }
}
