import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// Right pane: connection controls, quick commands and remote files.
Rectangle {
    id: root
    property var controller
    property var editorHost
    color: Theme.window

    readonly property var quickCommands: [
        { label: "GPU 状态",     cmd: "nvidia-smi" },
        { label: "作业队列",     cmd: "squeue" },
        { label: "磁盘用量",     cmd: "df -h" },
        { label: "Python 版本",  cmd: "python --version" }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PanelHeader { title: "工作区"; Layout.fillWidth: true }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle { color: Theme.window }

            TabButton {
                id: connectionTab
                text: "连接"
                contentItem: Text {
                    text: connectionTab.text
                    color: connectionTab.checked ? Theme.text : Theme.muted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 13
                }
                background: Rectangle {
                    color: connectionTab.checked ? Theme.panel : Theme.window
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: connectionTab.checked ? Theme.accent : "transparent"
                    }
                }
            }
            TabButton {
                id: filesTab
                text: "文件"
                contentItem: Text {
                    text: filesTab.text
                    color: filesTab.checked ? Theme.text : Theme.muted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 13
                }
                background: Rectangle {
                    color: filesTab.checked ? Theme.panel : Theme.window
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: filesTab.checked ? Theme.accent : "transparent"
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    // Status card.
                    Rectangle {
                        Layout.fillWidth: true
                        radius: 8
                        color: Theme.panel
                        border.color: Theme.border
                        implicitHeight: statusCol.implicitHeight + 24

                        ColumnLayout {
                            id: statusCol
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                spacing: 8
                                StatusDot { state: root.controller.connectionState }
                                Text {
                                    text: root.controller.connectionStateText
                                    color: Theme.text
                                    font.pixelSize: 15
                                    font.bold: true
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.controller.currentEndpoint.length > 0
                                      ? root.controller.currentEndpoint
                                      : "未选择服务器"
                                color: Theme.muted
                                font.pixelSize: 12
                                font.family: "Cascadia Mono"
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // Primary actions.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        StyledButton {
                            text: "连接"
                            primary: true
                            Layout.fillWidth: true
                            enabled: !root.controller.connected && !root.controller.busy
                                     && root.controller.selectedIndex >= 0
                            onClicked: root.controller.connectToServer(root.controller.selectedIndex)
                        }
                        StyledButton {
                            text: "断开"
                            Layout.fillWidth: true
                            enabled: root.controller.connected
                            onClicked: root.controller.disconnectCurrent()
                        }
                    }
                    StyledButton {
                        text: "取消操作"
                        Layout.fillWidth: true
                        enabled: root.controller.busy
                        onClicked: root.controller.cancel()
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    // Research quick commands.
                    Text {
                        text: "科研快捷命令"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 8
                        columnSpacing: 8
                        Repeater {
                            model: root.quickCommands
                            delegate: StyledButton {
                                required property var modelData
                                Layout.fillWidth: true
                                text: modelData.label
                                enabled: root.controller.connected
                                onClicked: root.controller.runQuickCommand(modelData.cmd)
                            }
                        }
                    }

                    Item { Layout.fillHeight: true } // spacer

                    StyledButton {
                        text: "清空终端"
                        Layout.fillWidth: true
                        onClicked: root.controller.clearTerminal()
                    }

                    // Footer: core + backend info.
                    Text {
                        Layout.fillWidth: true
                        text: "核心 v" + root.controller.coreVersion
                              + "  ·  凭据：" + root.controller.credentialBackend
                        color: Theme.faint
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            RemoteFilesPane {
                controller: root.controller
                editorHost: root.editorHost
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
}
