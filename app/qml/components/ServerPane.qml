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
    signal settingsRequested()
    property int pendingDeleteIndex: -1
    property string pendingDeleteName: ""
    property string pendingDeleteEndpoint: ""
    property int pendingEditIndex: -1
    color: Theme.window

    function openEditDialog(index, name, host, port, username, keyPath) {
        root.pendingEditIndex = index
        editNameField.text = name
        editHostField.text = host
        editPortField.text = String(port || 22)
        editUserField.text = username
        editPasswordField.text = ""
        editKeyPathField.text = keyPath || ""
        editKeyPassField.text = ""
        editServerDialog.open()
        editNameField.forceActiveFocus()
    }

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
                required property string host
                required property int port
                required property string username
                required property string endpoint
                required property string keyPath
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
                        text: "编辑"
                        implicitWidth: 48
                        enabled: row.hovered || row.ListView.isCurrentItem
                        opacity: enabled ? 1 : 0
                        onClicked: root.openEditDialog(row.index, row.name, row.host, row.port,
                                                       row.username, row.keyPath)
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

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 46
            color: Theme.window
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

            ToolButton {
                id: settingsButton
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 34
                height: 34
                text: "⚙"
                font.pixelSize: 18
                hoverEnabled: true
                ToolTip.visible: hovered
                ToolTip.delay: 450
                ToolTip.text: "设置"
                onClicked: root.settingsRequested()

                contentItem: Text {
                    text: settingsButton.text
                    color: settingsButton.enabled ? Theme.text : Theme.faint
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: settingsButton.font.pixelSize
                }

                background: Rectangle {
                    radius: 6
                    color: settingsButton.down ? Theme.buttonDown
                                                : (settingsButton.hovered ? Theme.panelAlt
                                                                          : Theme.button)
                    border.color: Theme.border
                }
            }

            Text {
                anchors.left: settingsButton.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: 12
                text: "v" + root.controller.appVersion
                color: Theme.faint
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }

    Dialog {
        id: editServerDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 430
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
                text: "编辑服务器"
                color: Theme.text
                font.pixelSize: 16
                font.bold: true
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                columns: 2
                rowSpacing: 10
                columnSpacing: 12

                Text { text: "别名"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editNameField
                    Layout.fillWidth: true
                    placeholderText: "显示名称"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editNameField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "主机/IP"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editHostField
                    Layout.fillWidth: true
                    placeholderText: "example.edu 或 IP"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editHostField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "端口"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editPortField
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: 65535 }
                    color: Theme.text
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editPortField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "用户名"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editUserField
                    Layout.fillWidth: true
                    placeholderText: "researcher"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editUserField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "新密码"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editPasswordField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: "留空保留原密码"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editPasswordField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "私钥文件"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editKeyPathField
                    Layout.fillWidth: true
                    placeholderText: "留空则自动发现 ~/.ssh"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editKeyPathField.activeFocus ? Theme.accent : Theme.border }
                }

                Text { text: "密钥口令"; color: Theme.muted; font.pixelSize: 13 }
                TextField {
                    id: editKeyPassField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: "留空保留原口令"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: editKeyPassField.activeFocus ? Theme.accent : Theme.border }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                text: "修改当前连接的服务器会断开现有会话；留空的密码字段不会覆盖已保存凭据。"
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
                    onClicked: editServerDialog.close()
                }
                StyledButton {
                    text: "保存"
                    primary: true
                    enabled: editHostField.text.trim().length > 0
                             && editUserField.text.trim().length > 0
                             && editPortField.acceptableInput
                    onClicked: {
                        root.controller.editServer(root.pendingEditIndex,
                                                   editHostField.text,
                                                   parseInt(editPortField.text) || 22,
                                                   editUserField.text,
                                                   editPasswordField.text,
                                                   editNameField.text,
                                                   editKeyPathField.text,
                                                   editKeyPassField.text)
                        editPasswordField.text = ""
                        editKeyPassField.text = ""
                        editServerDialog.close()
                    }
                }
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
