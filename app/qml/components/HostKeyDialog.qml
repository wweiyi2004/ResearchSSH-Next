import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// Shown the first time a server presents an unknown host key. The user confirms
// the fingerprint before the connection is trusted.
Dialog {
    id: dialog
    property var controller
    property string fingerprint: ""

    modal: true
    anchors.centerIn: parent
    width: 460
    padding: 0
    closePolicy: Popup.NoAutoClose

    background: Rectangle {
        color: Theme.panel
        radius: 10
        border.color: Theme.border
    }

    function show(fp) {
        fingerprint = fp
        open()
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            text: "确认主机密钥"
            color: Theme.text
            font.pixelSize: 16
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            text: "这是首次连接该服务器。请核对其主机密钥指纹是否与服务器管理员提供的一致:"
            color: Theme.textSoft
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            radius: 6
            color: Theme.panelAlt
            border.color: Theme.border
            implicitHeight: fpText.implicitHeight + 18
            Text {
                id: fpText
                anchors.fill: parent
                anchors.margins: 9
                text: dialog.fingerprint
                color: Theme.warning
                font.pixelSize: 13
                font.family: "Cascadia Mono"
                wrapMode: Text.WrapAnywhere
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            text: "接受后该密钥会写入 known_hosts,下次不再询问。"
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
                text: "拒绝"
                onClicked: {
                    dialog.controller.confirmHostKey(false)
                    dialog.close()
                }
            }
            StyledButton {
                text: "接受"
                primary: true
                onClicked: {
                    dialog.controller.confirmHostKey(true)
                    dialog.close()
                }
            }
        }
    }
}
