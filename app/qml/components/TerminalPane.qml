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
    property var completionSuggestions: []
    property string completionPrefix: ""
    color: Theme.window

    readonly property var commandCompletions: [
        { label: "nvidia-smi", insert: "nvidia-smi", detail: "GPU 状态" },
        { label: "watch nvidia-smi", insert: "watch -n 1 nvidia-smi", detail: "刷新 GPU" },
        { label: "squeue", insert: "squeue -u $USER", detail: "作业队列" },
        { label: "df", insert: "df -h", detail: "磁盘用量" },
        { label: "ls", insert: "ls -lah", detail: "列表" },
        { label: "cd", insert: "cd ", detail: "进入目录" },
        { label: "python", insert: "python ", detail: "运行脚本" },
        { label: "CUDA_VISIBLE_DEVICES", insert: "CUDA_VISIBLE_DEVICES=0 python ", detail: "指定 GPU" },
        { label: "pip install", insert: "pip install ", detail: "安装包" },
        { label: "conda activate", insert: "conda activate ", detail: "环境" },
        { label: "tail", insert: "tail -f ", detail: "日志" },
        { label: "grep", insert: "grep -R \"\" .", detail: "搜索" },
        { label: "tar", insert: "tar -czf archive.tar.gz ", detail: "打包" }
    ]

    function tokenPrefix() {
        var pos = input.cursorPosition
        var before = input.text.slice(0, pos)
        var match = before.match(/[A-Za-z0-9_.-]+$/)
        return match ? match[0] : ""
    }

    function updateCompletion(force) {
        if (!root.controller.connected) {
            terminalCompletion.close()
            return
        }
        var prefix = tokenPrefix()
        root.completionPrefix = prefix
        if (!force && prefix.length < 1) {
            terminalCompletion.close()
            return
        }
        var lower = prefix.toLowerCase()
        var out = []
        for (var i = 0; i < root.commandCompletions.length; ++i) {
            var item = root.commandCompletions[i]
            if (force || item.label.toLowerCase().indexOf(lower) === 0
                    || item.insert.toLowerCase().indexOf(lower) === 0)
                out.push(item)
        }
        root.completionSuggestions = out.slice(0, 8)
        if (root.completionSuggestions.length > 0) {
            terminalCompletionList.currentIndex = 0
            terminalCompletion.open()
        } else {
            terminalCompletion.close()
        }
    }

    function acceptCompletion(index) {
        if (index < 0 || index >= root.completionSuggestions.length)
            return
        var item = root.completionSuggestions[index]
        var prefix = root.completionPrefix
        var start = input.cursorPosition - prefix.length
        input.remove(start, input.cursorPosition)
        input.insert(start, item.insert)
        input.cursorPosition = start + item.insert.length
        terminalCompletion.close()
    }

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
                    onTextEdited: root.updateCompletion(false)
                    onCursorPositionChanged: {
                        if (terminalCompletion.opened)
                            root.updateCompletion(false)
                    }
                    onAccepted: {
                        if (terminalCompletion.opened) {
                            root.acceptCompletion(terminalCompletionList.currentIndex)
                        } else if (text.length > 0) {
                            root.controller.sendCommand(text)
                            text = ""
                        }
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Space
                                && (event.modifiers & Qt.ControlModifier)) {
                            root.updateCompletion(true)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Tab && terminalCompletion.opened) {
                            root.acceptCompletion(terminalCompletionList.currentIndex)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape && terminalCompletion.opened) {
                            terminalCompletion.close()
                            event.accepted = true
                        }
                    }
                }

                StyledButton {
                    text: "中止"
                    enabled: root.controller.connected
                    implicitWidth: 58
                    onClicked: root.controller.sendInterrupt()
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

            Popup {
                id: terminalCompletion
                x: 34
                y: -height - 6
                width: Math.min(input.width, 320)
                height: Math.min(238, terminalCompletionList.contentHeight + 8)
                padding: 4
                modal: false
                focus: false
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    radius: 6
                    color: Theme.panel
                    border.color: Theme.border
                }

                ListView {
                    id: terminalCompletionList
                    anchors.fill: parent
                    clip: true
                    model: root.completionSuggestions
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: terminalCompletionList.width
                        height: 32
                        radius: 4
                        color: index === terminalCompletionList.currentIndex ? Theme.treeSelected : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: modelData.insert
                                color: Theme.text
                                font.pixelSize: 12
                                font.family: "Cascadia Mono"
                                elide: Text.ElideRight
                            }
                            Text {
                                text: modelData.detail
                                color: Theme.muted
                                font.pixelSize: 11
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: terminalCompletionList.currentIndex = index
                            onClicked: root.acceptCompletion(index)
                        }
                    }
                }
            }
        }
    }
}
