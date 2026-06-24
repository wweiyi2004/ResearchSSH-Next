import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

Rectangle {
    id: root
    property var controller

    color: Theme.editor

    function canInstall(manager) {
        var m = String(manager).toLowerCase()
        return m === "pip" || m === "pip3" || m === "uv" || m === "conda" || m === "mamba"
                || m === "apt" || m === "dnf" || m === "yum" || m === "pacman"
                || m === "zypper" || m === "npm"
    }

    function needsConfirm(action, manager) {
        var m = String(manager).toLowerCase()
        return action === "remove" || m === "apt" || m === "dnf" || m === "yum"
                || m === "pacman" || m === "zypper"
    }

    function requestAction(action, manager, name) {
        if (!root.needsConfirm(action, manager)) {
            if (action === "install")
                root.controller.installPackage(manager, name)
            else if (action === "update")
                root.controller.updatePackage(manager, name)
            else if (action === "remove")
                root.controller.removePackage(manager, name)
            return
        }
        packageActionDialog.action = action
        packageActionDialog.manager = manager
        packageActionDialog.packageName = name
        packageActionDialog.open()
    }

    function actionTitle(action) {
        if (action === "install")
            return "确认安装"
        if (action === "update")
            return "确认更新"
        return "确认卸载"
    }

    function actionButtonText(action) {
        if (action === "install")
            return "安装"
        if (action === "update")
            return "更新"
        return "卸载"
    }

    function actionDescription(action, manager, name) {
        var subject = name + "（" + manager + "）"
        if (action === "install")
            return "将通过远端包管理器安装：" + subject
        if (action === "update")
            return "将通过远端包管理器更新：" + subject
        return "将从远端环境卸载：" + subject
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 52
            color: Theme.header
            border.color: Theme.borderSubtle

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                StyledButton {
                    text: root.controller.packageBusy ? "扫描中" : "扫描环境"
                    enabled: root.controller.connected && !root.controller.packageBusy
                    implicitWidth: 92
                    onClicked: root.controller.refreshEnvironment()
                }

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    enabled: root.controller.connected && !root.controller.packageBusy
                    placeholderText: "搜索 pip / conda / module 包"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    font.pixelSize: 13
                    background: Rectangle {
                        radius: 6
                        color: Theme.panelAlt
                        border.color: searchField.activeFocus ? Theme.accent : Theme.border
                    }
                    onAccepted: root.controller.searchPackages(text)
                }

                StyledButton {
                    text: "搜索"
                    primary: true
                    enabled: root.controller.connected && !root.controller.packageBusy
                             && searchField.text.trim().length > 0
                    implicitWidth: 70
                    onClicked: root.controller.searchPackages(searchField.text)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            text: root.controller.packageStatusText
            color: root.controller.packageBusy ? Theme.accent : Theme.muted
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            ScrollView {
                SplitView.preferredWidth: 360
                SplitView.minimumWidth: 280
                clip: true

                ColumnLayout {
                    width: Math.max(parent.width, 280)
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.topMargin: 8
                        text: "检测到的工具"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Repeater {
                        model: root.controller.packageTools
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            height: 48
                            radius: 6
                            color: Theme.panel
                            border.color: Theme.borderSubtle

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                Text {
                                    text: modelData.name
                                    color: Theme.accent
                                    font.pixelSize: 12
                                    font.bold: true
                                    Layout.preferredWidth: 70
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.path
                                    color: Theme.muted
                                    font.pixelSize: 11
                                    font.family: "Cascadia Mono"
                                    elide: Text.ElideMiddle
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        text: "已安装"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Repeater {
                        model: root.controller.installedPackages
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            height: 58
                            radius: 6
                            color: Theme.panel
                            border.color: Theme.borderSubtle

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8
                                Text {
                                    text: modelData.manager
                                    color: Theme.faint
                                    font.pixelSize: 11
                                    Layout.preferredWidth: 46
                                    elide: Text.ElideRight
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: Theme.text
                                        font.pixelSize: 12
                                        font.family: "Cascadia Mono"
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.version
                                        color: Theme.muted
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                StyledButton {
                                    text: "更新"
                                    implicitWidth: 58
                                    enabled: root.controller.connected && !root.controller.packageBusy
                                             && !!modelData.canUpdate
                                    onClicked: root.requestAction("update", modelData.manager,
                                                                  modelData.name)
                                }

                                StyledButton {
                                    text: "卸载"
                                    implicitWidth: 58
                                    accent: Theme.danger
                                    enabled: root.controller.connected && !root.controller.packageBusy
                                             && !!modelData.canRemove
                                    onClicked: root.requestAction("remove", modelData.manager,
                                                                  modelData.name)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.margins: 12
                        visible: root.controller.installedPackages.length === 0
                        text: "尚未扫描已安装包"
                        color: Theme.faint
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            ScrollView {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 340
                clip: true

                ColumnLayout {
                    width: Math.max(parent.width, 340)
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.topMargin: 8
                        text: "搜索结果"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Repeater {
                        model: root.controller.packageSearchResults
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            height: 64
                            radius: 6
                            color: Theme.panel
                            border.color: Theme.borderSubtle

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Text {
                                            text: modelData.manager
                                            color: Theme.accent
                                            font.pixelSize: 11
                                            font.bold: true
                                            Layout.preferredWidth: 52
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            color: Theme.text
                                            font.pixelSize: 13
                                            font.family: "Cascadia Mono"
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: modelData.version
                                            color: Theme.muted
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                            Layout.maximumWidth: 120
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.detail
                                        color: Theme.faint
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                StyledButton {
                                    text: modelData.installed ? "已安装" : "安装"
                                    implicitWidth: 70
                                    enabled: root.controller.connected && !root.controller.packageBusy
                                             && !modelData.installed
                                             && (modelData.canInstall === undefined
                                                 ? root.canInstall(modelData.manager)
                                                 : !!modelData.canInstall)
                                    onClicked: root.requestAction("install", modelData.manager,
                                                                  modelData.name)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.margins: 12
                        visible: root.controller.packageSearchResults.length === 0
                        text: "输入关键词搜索远端可用包"
                        color: Theme.faint
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.bottomMargin: 12
                        visible: root.controller.packageLogText.length > 0
                        height: Math.min(170, packageLogText.implicitHeight + 22)
                        radius: 6
                        color: Theme.terminal
                        border.color: Theme.borderSubtle

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 10
                            clip: true

                            Text {
                                id: packageLogText
                                width: parent.width
                                text: root.controller.packageLogText
                                color: Theme.textSoft
                                font.pixelSize: 11
                                font.family: "Cascadia Mono"
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: packageActionDialog
        property string action: "install"
        property string manager: ""
        property string packageName: ""

        modal: true
        anchors.centerIn: parent
        width: 360
        padding: 0

        background: Rectangle {
            color: Theme.panel
            radius: 6
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 10

            Text {
                Layout.fillWidth: true
                Layout.margins: 14
                text: root.actionTitle(packageActionDialog.action)
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: root.actionDescription(packageActionDialog.action,
                                             packageActionDialog.manager,
                                             packageActionDialog.packageName)
                color: Theme.textSoft
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                visible: packageActionDialog.action === "remove"
                         || root.needsConfirm(packageActionDialog.action,
                                              packageActionDialog.manager)
                text: "系统级包管理器可能修改全局环境；远端需要 root 会话。"
                color: Theme.warning
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "取消"
                    onClicked: packageActionDialog.reject()
                }
                StyledButton {
                    text: root.actionButtonText(packageActionDialog.action)
                    primary: true
                    accent: packageActionDialog.action === "remove" ? Theme.danger : Theme.accent
                    onClicked: packageActionDialog.accept()
                }
            }
        }

        onAccepted: {
            if (action === "install")
                root.controller.installPackage(manager, packageName)
            else if (action === "update")
                root.controller.updatePackage(manager, packageName)
            else if (action === "remove")
                root.controller.removePackage(manager, packageName)
        }
    }
}
