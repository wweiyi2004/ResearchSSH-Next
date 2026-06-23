import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

Rectangle {
    id: root
    property var controller
    property bool modified: false
    property bool loadingText: false
    readonly property int maxEditorBytes: 2 * 1024 * 1024

    color: Theme.editor

    function requestOpen(path, size) {
        if (!root.controller.fileAvailable)
            return
        if (!path || path.length === 0)
            return
        if (Number(size) >= root.maxEditorBytes) {
            largeFileDialog.path = path
            largeFileDialog.open()
            return
        }
        if (root.modified && root.controller.editor.path !== path) {
            unsavedDialog.path = path
            unsavedDialog.size = Number(size)
            unsavedDialog.open()
            return
        }
        root.controller.openPath(path)
    }

    function saveCurrent() {
        if (root.controller.connected && root.controller.fileAvailable
                && root.controller.editor.isOpen && root.modified
                && !root.controller.editor.saving) {
            root.controller.saveEditor(editorText.text)
        }
    }

    function shortPath(path) {
        var text = String(path)
        if (text.length <= 78)
            return text
        return "..." + text.slice(text.length - 75)
    }

    function lineNumbers(count) {
        var limit = Math.min(200, count)
        var values = []
        for (var i = 1; i <= limit; ++i)
            values.push(i)
        return values.join("\n")
    }

    Shortcut {
        sequence: StandardKey.Save
        enabled: root.controller.connected
                 && root.controller.fileAvailable
                 && root.controller.editor.isOpen
                 && root.modified
                 && !root.controller.editor.saving
        onActivated: root.saveCurrent()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 42
            color: Theme.editor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                spacing: 10

                Rectangle {
                    width: 26
                    height: 22
                    radius: 4
                    color: root.controller.editor.isOpen
                           ? (Theme.dark ? "#2b4d3a" : "#e6fffb")
                           : Theme.panelAlt
                    border.color: root.controller.editor.isOpen ? "#4ec9b0" : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: "TXT"
                        color: root.controller.editor.isOpen ? "#12805c" : Theme.muted
                        font.pixelSize: 10
                        font.bold: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: root.controller.editor.isOpen
                              ? root.controller.editor.fileName + (root.modified ? " *" : "")
                              : "未打开文件"
                        color: Theme.textSoft
                        font.pixelSize: 14
                        font.bold: true
                        elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.controller.editor.isOpen
                        text: root.shortPath(root.controller.editor.path)
                        color: Theme.muted
                        font.pixelSize: 11
                        font.family: "Cascadia Mono"
                        elide: Text.ElideMiddle
                    }
                }

                Text {
                    visible: root.controller.editor.busy
                    text: "读取中"
                    color: Theme.accent
                    font.pixelSize: 12
                }

                Text {
                    visible: root.controller.editor.saving || root.modified
                    text: root.controller.editor.saving ? "保存中" : "未保存"
                    color: root.controller.editor.saving ? Theme.accent : Theme.warning
                    font.pixelSize: 12
                }

                StyledButton {
                    text: "保存"
                    primary: true
                    enabled: root.controller.connected
                             && root.controller.fileAvailable
                             && root.controller.editor.isOpen
                             && root.modified
                             && !root.controller.editor.saving
                    onClicked: root.saveCurrent()
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderSubtle
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.editor

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 52
                color: Theme.editorGutter
                border.color: Theme.borderSubtle

                Text {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 12
                    text: root.controller.editor.isOpen
                          ? root.lineNumbers(editorText.lineCount)
                          : ""
                    color: Theme.faint
                    font.pixelSize: 13
                    font.family: "Cascadia Mono"
                    lineHeight: 1.25
                    horizontalAlignment: Text.AlignRight
                }
            }

            TextArea {
                id: editorText
                anchors.fill: parent
                anchors.leftMargin: 52
                enabled: root.controller.editor.isOpen && !root.controller.editor.busy
                         && !root.controller.editor.saving
                selectByMouse: true
                wrapMode: TextArea.NoWrap
                color: Theme.textSoft
                selectionColor: Theme.dark ? "#264f78" : "#cfe8ff"
                selectedTextColor: Theme.selectionText
                font.pixelSize: 14
                font.family: "Cascadia Mono"
                leftPadding: 14
                rightPadding: 16
                topPadding: 12
                bottomPadding: 12
                placeholderText: root.controller.editor.busy ? "正在读取..." : ""
                placeholderTextColor: Theme.faint
                background: Rectangle { color: Theme.editor }
                onTextChanged: {
                    if (!root.loadingText && root.controller.editor.isOpen)
                        root.modified = true
                }
            }

            Text {
                anchors.centerIn: parent
                visible: !root.controller.editor.isOpen && !root.controller.editor.busy
                text: "未打开文件"
                color: Theme.faint
                font.pixelSize: 18
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 24
            color: Theme.accentStrong

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 14

                Text {
                    text: root.controller.editor.isOpen ? "UTF-8" : ""
                    color: "#ffffff"
                    font.pixelSize: 11
                }
                Text {
                    text: root.controller.editor.isOpen ? "CRLF/LF" : ""
                    color: "#ffffff"
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: root.controller.editor.isOpen
                          ? (root.modified ? "已修改" : "已保存")
                          : ""
                    color: "#ffffff"
                    font.pixelSize: 11
                }
            }
        }
    }

    Connections {
        target: root.controller.editor
        function onChanged() {
            if (!root.controller.editor.isOpen && !root.controller.editor.busy) {
                root.loadingText = true
                editorText.text = ""
                root.loadingText = false
                root.modified = false
            }
        }
        function onContentLoaded(text) {
            root.loadingText = true
            editorText.text = text
            root.loadingText = false
            root.modified = false
        }
        function onSaveSucceeded() {
            root.modified = false
        }
        function onSaveFailed(message) {
            root.modified = true
        }
    }

    Dialog {
        id: unsavedDialog
        property string path: ""
        property var size: 0

        modal: true
        anchors.centerIn: parent
        width: 330
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
                text: "当前文件有未保存修改"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: "继续打开新文件会丢弃这些修改。"
                color: Theme.textSoft
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "取消"
                    onClicked: unsavedDialog.reject()
                }
                StyledButton {
                    text: "丢弃并打开"
                    primary: true
                    onClicked: unsavedDialog.accept()
                }
            }
        }

        onAccepted: {
            root.modified = false
            root.requestOpen(path, size)
        }
    }

    Dialog {
        id: largeFileDialog
        property string path: ""

        modal: true
        anchors.centerIn: parent
        width: 330
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
                text: "文件过大"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: "轻量编辑器当前限制为 " + (root.maxEditorBytes / 1024 / 1024) + " MB。"
                color: Theme.textSoft
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "确定"
                    primary: true
                    onClicked: largeFileDialog.accept()
                }
            }
        }
    }
}
