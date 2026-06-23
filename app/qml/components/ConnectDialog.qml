import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// "新建 SSH 连接" dialog. Collects host/port/user/password and asks the controller
// to open a real (russh) session. The password is handed to C++ and never kept in
// QML state beyond this field.
Dialog {
    id: dialog
    property var controller

    modal: true
    anchors.centerIn: parent
    width: 440
    padding: 0
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.panel
        radius: 10
        border.color: Theme.border
    }

    function resetFields() {
        hostField.text = ""
        portField.text = "22"
        userField.text = ""
        passField.text = ""
        hostField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 12
        // Header
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            text: "新建 SSH 连接"
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

            Text { text: "主机"; color: Theme.muted; font.pixelSize: 13 }
            TextField {
                id: hostField
                Layout.fillWidth: true
                placeholderText: "example.edu 或 IP"
                color: Theme.text
                placeholderTextColor: Theme.faint
                background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: hostField.activeFocus ? Theme.accent : Theme.border }
            }

            Text { text: "端口"; color: Theme.muted; font.pixelSize: 13 }
            TextField {
                id: portField
                Layout.fillWidth: true
                text: "22"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 65535 }
                color: Theme.text
                background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: portField.activeFocus ? Theme.accent : Theme.border }
            }

            Text { text: "用户名"; color: Theme.muted; font.pixelSize: 13 }
            TextField {
                id: userField
                Layout.fillWidth: true
                placeholderText: "researcher"
                color: Theme.text
                placeholderTextColor: Theme.faint
                background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: userField.activeFocus ? Theme.accent : Theme.border }
            }

            Text { text: "密码"; color: Theme.muted; font.pixelSize: 13 }
            TextField {
                id: passField
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "••••••••"
                color: Theme.text
                placeholderTextColor: Theme.faint
                background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: passField.activeFocus ? Theme.accent : Theme.border }
                onAccepted: connectButton.clicked()
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            text: "首次连接会要求确认服务器主机密钥。"
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
                onClicked: dialog.close()
            }
            StyledButton {
                id: connectButton
                text: "连接"
                primary: true
                enabled: hostField.text.length > 0 && userField.text.length > 0
                onClicked: {
                    dialog.controller.connectToHost(hostField.text,
                                                    parseInt(portField.text) || 22,
                                                    userField.text,
                                                    passField.text, "")
                    passField.text = ""
                    dialog.close()
                }
            }
        }
    }
}
