import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// Center pane: terminal output placeholder + a command input. Real VT/ANSI
// rendering is future work in the Rust core; this just shows sanitised text.
Rectangle {
    id: root
    property var controller
    property bool showHeader: true
    color: Theme.window

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PanelHeader {
            title: "终端"
            visible: root.showHeader
            Layout.fillWidth: true
            Layout.preferredHeight: root.showHeader ? 42 : 0
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            TextArea {
                id: output
                readOnly: true
                selectByMouse: true
                wrapMode: TextArea.WrapAtWordBoundaryOrAnywhere
                text: root.controller.terminal.text
                color: Theme.textSoft
                font.pixelSize: 15
                font.family: "Cascadia Mono"
                background: Rectangle { color: Theme.terminal }
                padding: 14
                // Keep the latest output in view.
                onTextChanged: output.cursorPosition = output.length
            }
        }

        // Command input row.
        Rectangle {
            Layout.fillWidth: true
            height: 52
            color: Theme.panel
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 9
                spacing: 8

                Text {
                    text: "$"
                    color: Theme.success
                    font.pixelSize: 16
                    font.family: "Cascadia Mono"
                }

                TextField {
                    id: input
                    Layout.fillWidth: true
                    enabled: root.controller.connected
                    placeholderText: root.controller.connected
                                     ? "输入命令后回车…"
                                     : "连接服务器后即可发送命令"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    font.pixelSize: 14
                    font.family: "Cascadia Mono"
                    background: Rectangle {
                        radius: 6
                        color: Theme.panelAlt
                        border.color: input.activeFocus ? Theme.accent : Theme.border
                        border.width: 1
                    }
                    onAccepted: {
                        if (text.length > 0) {
                            root.controller.sendCommand(text)
                            text = ""
                        }
                    }
                }

                StyledButton {
                    text: "发送"
                    primary: true
                    enabled: root.controller.connected && input.text.length > 0
                    onClicked: {
                        root.controller.sendCommand(input.text)
                        input.text = ""
                    }
                }
            }
        }
    }
}
