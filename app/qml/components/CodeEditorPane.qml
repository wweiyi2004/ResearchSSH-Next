import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

Rectangle {
    id: root
    property var controller
    property bool modified: false
    property bool loadingText: false
    property var openDocuments: []
    property int activeDocumentIndex: -1
    property string pendingOpenPath: ""
    property var ignoredOpenPaths: []
    readonly property int maxEditorBytes: 2 * 1024 * 1024
    readonly property string activePath: root.activeDocumentIndex >= 0
                                         && root.activeDocumentIndex < root.openDocuments.length
                                         ? root.openDocuments[root.activeDocumentIndex].path
                                         : ""
    readonly property bool activeDocumentLoading: root.activeDocumentIndex >= 0
                                                  && root.activeDocumentIndex < root.openDocuments.length
                                                  && !!root.openDocuments[root.activeDocumentIndex].loading
    readonly property string currentLanguage: languageForPath(root.activePath)
    readonly property bool pythonOpen: currentLanguage === "python"
    property var completionSuggestions: []
    property string completionPrefix: ""

    color: Theme.editor

    function requestOpen(path, size) {
        if (!root.controller.fileAvailable)
            return
        if (!path || path.length === 0)
            return
        var existing = root.findDocumentIndex(path)
        if (existing >= 0) {
            root.setActiveDocument(existing)
            return
        }
        if (Number(size) >= root.maxEditorBytes) {
            largeFileDialog.path = path
            largeFileDialog.open()
            return
        }
        root.unignoreOpenPath(path)
        var doc = {
            path: path,
            name: root.fileNameOf(path),
            text: "",
            modified: false,
            loading: true
        }
        var docs = root.openDocuments.slice()
        docs.push(doc)
        root.openDocuments = docs
        root.pendingOpenPath = path
        root.activateDocument(docs.length - 1, true, true)
        root.controller.openPath(path)
    }

    function saveCurrent() {
        if (root.controller.connected && root.controller.fileAvailable
                && root.activeDocumentIndex >= 0 && !root.activeDocumentLoading
                && root.modified && !root.controller.editor.saving) {
            if (root.activePath.length > 0)
                root.controller.activateEditorPath(root.activePath)
            root.controller.saveEditor(editorText.text)
        }
    }

    function fileNameOf(path) {
        var text = String(path)
        var slash = text.lastIndexOf("/")
        return slash >= 0 ? text.slice(slash + 1) : text
    }

    function findDocumentIndex(path) {
        for (var i = 0; i < root.openDocuments.length; ++i) {
            if (root.openDocuments[i].path === path)
                return i
        }
        return -1
    }

    function replaceDocument(index, doc) {
        var docs = root.openDocuments.slice()
        docs[index] = doc
        root.openDocuments = docs
    }

    function isIgnoredOpenPath(path) {
        return root.ignoredOpenPaths.indexOf(path) >= 0
    }

    function ignoreOpenPath(path) {
        if (!path || path.length === 0 || root.isIgnoredOpenPath(path))
            return
        var paths = root.ignoredOpenPaths.slice()
        paths.push(path)
        root.ignoredOpenPaths = paths
    }

    function unignoreOpenPath(path) {
        var index = root.ignoredOpenPaths.indexOf(path)
        if (index < 0)
            return
        var paths = root.ignoredOpenPaths.slice()
        paths.splice(index, 1)
        root.ignoredOpenPaths = paths
    }

    function makeDocument(path, text, modified, loading) {
        return {
            path: path,
            name: root.fileNameOf(path),
            text: text,
            modified: modified,
            loading: loading
        }
    }

    function persistActiveText() {
        if (root.loadingText || root.activeDocumentIndex < 0
                || root.activeDocumentIndex >= root.openDocuments.length)
            return
        var doc = Object.assign({}, root.openDocuments[root.activeDocumentIndex])
        doc.text = editorText.text
        root.replaceDocument(root.activeDocumentIndex, doc)
    }

    function activateDocument(index, persistCurrent, focusEditor) {
        if (index < 0 || index >= root.openDocuments.length)
            return
        if (persistCurrent)
            root.persistActiveText()
        root.activeDocumentIndex = index
        var doc = root.openDocuments[index]
        root.loadingText = true
        editorText.text = doc.text || ""
        editorText.cursorPosition = 0
        root.loadingText = false
        root.modified = !!doc.modified
        root.controller.activateEditorPath(doc.path)
        completionPopup.close()
        if (focusEditor)
            editorText.forceActiveFocus()
    }

    function setActiveDocument(index) {
        if (index < 0 || index >= root.openDocuments.length)
            return
        root.pendingOpenPath = ""
        root.activateDocument(index, true, true)
    }

    function setActiveModified(value) {
        if (root.activeDocumentIndex < 0
                || root.activeDocumentIndex >= root.openDocuments.length) {
            root.modified = false
            return
        }
        var doc = Object.assign({}, root.openDocuments[root.activeDocumentIndex])
        doc.modified = value
        doc.text = editorText.text
        doc.loading = false
        root.replaceDocument(root.activeDocumentIndex, doc)
        root.modified = value
    }

    function upsertLoadedDocument(path, text) {
        if (root.isIgnoredOpenPath(path)) {
            root.unignoreOpenPath(path)
            if (root.activePath.length > 0)
                root.controller.activateEditorPath(root.activePath)
            return
        }
        var index = root.findDocumentIndex(path)
        var wasActive = index === root.activeDocumentIndex
        var doc = root.makeDocument(path, text, false, false)
        if (index >= 0) {
            root.replaceDocument(index, doc)
        } else {
            var docs = root.openDocuments.slice()
            docs.push(doc)
            root.openDocuments = docs
            index = docs.length - 1
        }
        var shouldActivate = root.pendingOpenPath === path || root.activeDocumentIndex < 0
        if (root.pendingOpenPath === path)
            root.pendingOpenPath = ""
        if (shouldActivate) {
            root.activateDocument(index, false, true)
        } else if (wasActive) {
            root.activateDocument(index, false, false)
        } else if (root.activePath.length > 0) {
            root.controller.activateEditorPath(root.activePath)
        }
    }

    function handleOpenFailed(path, message) {
        if (root.isIgnoredOpenPath(path)) {
            root.unignoreOpenPath(path)
            return
        }
        var index = root.findDocumentIndex(path)
        if (index < 0)
            return
        var wasActive = index === root.activeDocumentIndex
        var docs = root.openDocuments.slice()
        docs.splice(index, 1)
        root.openDocuments = docs
        if (root.pendingOpenPath === path)
            root.pendingOpenPath = ""
        if (docs.length === 0) {
            root.activeDocumentIndex = -1
            root.modified = false
            root.loadingText = true
            editorText.text = ""
            root.loadingText = false
            root.controller.closeEditor()
            return
        }
        if (wasActive) {
            root.activateDocument(Math.min(index, docs.length - 1), false, true)
        } else if (index < root.activeDocumentIndex) {
            root.activeDocumentIndex -= 1
        }
    }

    function requestCloseDocument(index) {
        if (index < 0 || index >= root.openDocuments.length)
            return
        if (root.openDocuments[index].modified) {
            closeFileDialog.index = index
            closeFileDialog.fileName = root.openDocuments[index].name
            closeFileDialog.open()
            return
        }
        root.closeDocument(index)
    }

    function closeDocument(index) {
        if (index < 0 || index >= root.openDocuments.length)
            return
        var wasActive = index === root.activeDocumentIndex
        var closingDoc = root.openDocuments[index]
        if (closingDoc.loading)
            root.ignoreOpenPath(closingDoc.path)
        if (root.pendingOpenPath === closingDoc.path)
            root.pendingOpenPath = ""
        var docs = root.openDocuments.slice()
        docs.splice(index, 1)
        root.openDocuments = docs
        if (docs.length === 0) {
            root.activeDocumentIndex = -1
            root.modified = false
            root.loadingText = true
            editorText.text = ""
            root.loadingText = false
            root.controller.closeEditor()
            root.pendingOpenPath = ""
            return
        }
        if (wasActive) {
            root.activateDocument(Math.min(index, docs.length - 1), false, true)
        } else if (index < root.activeDocumentIndex) {
            root.activeDocumentIndex -= 1
        }
    }

    function shortPath(path) {
        var text = String(path)
        if (text.length <= 78)
            return text
        return "..." + text.slice(text.length - 75)
    }

    function lineNumbers(count) {
        var limit = Math.min(9999, count)
        var values = []
        for (var i = 1; i <= limit; ++i)
            values.push(i)
        return values.join("\n")
    }

    function languageForPath(path) {
        var p = String(path).toLowerCase()
        if (p.endsWith(".py") || p.endsWith(".pyw"))
            return "python"
        if (p.endsWith(".cpp") || p.endsWith(".cc") || p.endsWith(".cxx")
                || p.endsWith(".c") || p.endsWith(".h") || p.endsWith(".hpp"))
            return "cpp"
        if (p.endsWith(".sh") || p.endsWith(".bash") || p.endsWith(".zsh"))
            return "shell"
        if (p.endsWith(".md") || p.endsWith(".markdown"))
            return "markdown"
        return "text"
    }

    function languageLabel(lang) {
        if (lang === "python")
            return "PY"
        if (lang === "cpp")
            return "C++"
        if (lang === "shell")
            return "SH"
        if (lang === "markdown")
            return "MD"
        return "TXT"
    }

    function wordPrefix() {
        var pos = editorText.cursorPosition
        var before = editorText.text.slice(0, pos)
        var match = before.match(/[A-Za-z_][A-Za-z0-9_]*$/)
        return match ? match[0] : ""
    }

    function completionPool() {
        if (root.currentLanguage === "python") {
            return [
                { label: "import", insert: "import ", detail: "module import" },
                { label: "from ... import ...", insert: "from  import ", detail: "selective import" },
                { label: "def", insert: "def main():\n    ", detail: "function" },
                { label: "class", insert: "class Model:\n    def __init__(self):\n        ", detail: "class" },
                { label: "for", insert: "for item in items:\n    ", detail: "loop" },
                { label: "if", insert: "if condition:\n    ", detail: "branch" },
                { label: "with open", insert: "with open(path, \"r\", encoding=\"utf-8\") as f:\n    ", detail: "file" },
                { label: "torch.cuda.is_available", insert: "torch.cuda.is_available()", detail: "gpu check" },
                { label: "torch.device", insert: "torch.device(\"cuda\" if torch.cuda.is_available() else \"cpu\")", detail: "device" },
                { label: "print", insert: "print()", detail: "builtin" },
                { label: "range", insert: "range()", detail: "builtin" },
                { label: "enumerate", insert: "enumerate()", detail: "builtin" }
            ]
        }
        if (root.currentLanguage === "cpp") {
            return [
                { label: "#include", insert: "#include <", detail: "preprocessor" },
                { label: "int main", insert: "int main(int argc, char **argv) {\n    return 0;\n}", detail: "entry" },
                { label: "for", insert: "for (int i = 0; i < n; ++i) {\n    \n}", detail: "loop" },
                { label: "if", insert: "if () {\n    \n}", detail: "branch" },
                { label: "std::vector", insert: "std::vector<>", detail: "container" },
                { label: "QString", insert: "QString", detail: "Qt string" }
            ]
        }
        if (root.currentLanguage === "shell") {
            return [
                { label: "python", insert: "python ", detail: "run Python" },
                { label: "CUDA_VISIBLE_DEVICES", insert: "CUDA_VISIBLE_DEVICES=0 python ", detail: "select GPU" },
                { label: "nvidia-smi", insert: "nvidia-smi", detail: "GPU status" },
                { label: "squeue", insert: "squeue -u $USER", detail: "jobs" },
                { label: "for", insert: "for f in *; do\n    echo \"$f\"\ndone", detail: "loop" },
                { label: "if", insert: "if [ -f file ]; then\n    \nfi", detail: "branch" }
            ]
        }
        return [
            { label: "TODO", insert: "TODO: ", detail: "note" },
            { label: "python", insert: "python ", detail: "command" },
            { label: "nvidia-smi", insert: "nvidia-smi", detail: "command" }
        ]
    }

    function updateCompletion(force) {
        if (root.activeDocumentIndex < 0 || root.currentLanguage === "text"
                || root.activeDocumentLoading) {
            completionPopup.close()
            return
        }
        var prefix = wordPrefix()
        root.completionPrefix = prefix
        if (!force && prefix.length < 2) {
            completionPopup.close()
            return
        }
        var lower = prefix.toLowerCase()
        var pool = completionPool()
        var out = []
        for (var i = 0; i < pool.length; ++i) {
            if (force || pool[i].label.toLowerCase().indexOf(lower) === 0
                    || pool[i].insert.toLowerCase().indexOf(lower) === 0)
                out.push(pool[i])
        }
        root.completionSuggestions = out.slice(0, 8)
        if (root.completionSuggestions.length > 0) {
            completionList.currentIndex = 0
            completionPopup.x = Math.min(editorText.x + editorText.cursorRectangle.x + 12,
                                         root.width - completionPopup.width - 12)
            completionPopup.y = Math.min(editorText.y + editorText.cursorRectangle.y + 30,
                                         root.height - completionPopup.height - 32)
            completionPopup.open()
        } else {
            completionPopup.close()
        }
    }

    function acceptCompletion(index) {
        if (index < 0 || index >= root.completionSuggestions.length)
            return
        var item = root.completionSuggestions[index]
        var prefix = root.completionPrefix
        var start = editorText.cursorPosition - prefix.length
        editorText.remove(start, editorText.cursorPosition)
        editorText.insert(start, item.insert)
        editorText.cursorPosition = start + item.insert.length
        completionPopup.close()
        root.setActiveModified(true)
    }

    function leadingWhitespace(text) {
        var match = String(text).match(/^\s*/)
        return match ? match[0] : ""
    }

    function currentLineBeforeCursor() {
        var before = editorText.text.slice(0, editorText.cursorPosition)
        var lineStart = before.lastIndexOf("\n") + 1
        return before.slice(lineStart)
    }

    function pairFor(ch) {
        if (ch === "(")
            return ")"
        if (ch === "[")
            return "]"
        if (ch === "{")
            return "}"
        if (ch === "\"")
            return "\""
        if (ch === "'")
            return "'"
        return ""
    }

    function handlePairInsert(ch) {
        var close = root.pairFor(ch)
        if (close.length === 0)
            return false
        var start = editorText.selectionStart
        var end = editorText.selectionEnd
        if (start !== end) {
            var selected = editorText.selectedText
            editorText.remove(start, end)
            editorText.insert(start, ch + selected + close)
            editorText.cursorPosition = start + selected.length + 2
        } else {
            var pos = editorText.cursorPosition
            editorText.insert(pos, ch + close)
            editorText.cursorPosition = pos + 1
        }
        root.setActiveModified(true)
        return true
    }

    function handleClosePair(ch) {
        var pos = editorText.cursorPosition
        if (editorText.text.charAt(pos) === ch) {
            editorText.cursorPosition = pos + 1
            return true
        }
        return false
    }

    function handleBackspacePair() {
        if (editorText.selectionStart !== editorText.selectionEnd)
            return false
        var pos = editorText.cursorPosition
        if (pos <= 0)
            return false
        var prev = editorText.text.charAt(pos - 1)
        var next = editorText.text.charAt(pos)
        if (root.pairFor(prev) === next) {
            editorText.remove(pos - 1, pos + 1)
            editorText.cursorPosition = pos - 1
            root.setActiveModified(true)
            return true
        }
        return false
    }

    function handleSmartEnter() {
        var pos = editorText.cursorPosition
        var prev = pos > 0 ? editorText.text.charAt(pos - 1) : ""
        var next = editorText.text.charAt(pos)
        var line = root.currentLineBeforeCursor()
        var indent = root.leadingWhitespace(line)
        var extra = (line.trim().endsWith(":") || line.trim().endsWith("{")
                     || line.trim().endsWith("[") || line.trim().endsWith("("))
                    ? "    " : ""
        if (root.pairFor(prev) === next && (prev === "{" || prev === "[" || prev === "(")) {
            editorText.insert(pos, "\n" + indent + "    \n" + indent)
            editorText.cursorPosition = pos + indent.length + 5
        } else {
            editorText.insert(pos, "\n" + indent + extra)
            editorText.cursorPosition = pos + 1 + indent.length + extra.length
        }
        root.setActiveModified(true)
        return true
    }

    Shortcut {
        sequence: StandardKey.Save
        enabled: root.controller.connected
                 && root.controller.fileAvailable
                 && root.activeDocumentIndex >= 0
                 && root.modified
                 && !root.activeDocumentLoading
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
                    color: root.activeDocumentIndex >= 0
                           ? (Theme.dark ? "#2b4d3a" : "#e6fffb")
                           : Theme.panelAlt
                    border.color: root.activeDocumentIndex >= 0 ? "#4ec9b0" : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: root.languageLabel(root.currentLanguage)
                        color: root.activeDocumentIndex >= 0 ? "#12805c" : Theme.muted
                        font.pixelSize: 10
                        font.bold: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: root.activeDocumentIndex >= 0
                              ? root.openDocuments[root.activeDocumentIndex].name + (root.modified ? " *" : "")
                              : "未打开文件"
                        color: Theme.textSoft
                        font.pixelSize: 14
                        font.bold: true
                        elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.activeDocumentIndex >= 0
                        text: root.shortPath(root.activePath)
                        color: Theme.muted
                        font.pixelSize: 11
                        font.family: "Cascadia Mono"
                        elide: Text.ElideMiddle
                    }
                }

                Text {
                    visible: root.activeDocumentLoading
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

                ComboBox {
                    id: runDeviceBox
                    visible: root.pythonOpen
                    enabled: root.controller.connected
                    model: ["CPU", "GPU 0", "GPU 1", "GPU 2", "GPU 3"]
                    implicitWidth: 94
                    implicitHeight: 32
                    font.pixelSize: 12
                    contentItem: Text {
                        text: runDeviceBox.displayText
                        color: runDeviceBox.enabled ? Theme.text : Theme.faint
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 10
                        rightPadding: 24
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        radius: 6
                        color: Theme.panelAlt
                        border.color: runDeviceBox.activeFocus ? Theme.accent : Theme.border
                    }
                }

                StyledButton {
                    text: "运行"
                    visible: root.pythonOpen
                    enabled: root.controller.connected && root.activeDocumentIndex >= 0
                             && !root.activeDocumentLoading
                    implicitWidth: 66
                    onClicked: {
                        var target = runDeviceBox.currentIndex === 0
                                ? "cpu"
                                : String(runDeviceBox.currentIndex - 1)
                        root.controller.runPythonFile(root.activePath, target)
                    }
                }

                StyledButton {
                    text: "保存"
                    primary: true
                    enabled: root.controller.connected
                             && root.controller.fileAvailable
                             && root.activeDocumentIndex >= 0
                             && root.modified
                             && !root.activeDocumentLoading
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
            height: root.openDocuments.length > 0 ? 34 : 0
            visible: root.openDocuments.length > 0
            color: Theme.tabBar
            clip: true

            Flickable {
                anchors.fill: parent
                contentWidth: fileTabsRow.implicitWidth
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick
                clip: true

                RowLayout {
                    id: fileTabsRow
                    height: parent.height
                    spacing: 0

                    Repeater {
                        model: root.openDocuments
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            Layout.preferredWidth: Math.min(210, Math.max(122, tabName.implicitWidth + 54))
                            height: fileTabsRow.height
                            color: index === root.activeDocumentIndex ? Theme.tabActive : Theme.tabInactive
                            border.color: Theme.borderSubtle

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 6
                                spacing: 6
                                Text {
                                    id: tabName
                                    Layout.fillWidth: true
                                    text: modelData.name
                                          + (modelData.loading ? " ..." : (modelData.modified ? " *" : ""))
                                    color: index === root.activeDocumentIndex ? Theme.text : Theme.muted
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }
                                Rectangle {
                                    width: 18
                                    height: 18
                                    radius: 4
                                    color: closeMouse.containsMouse ? Theme.buttonDown : "transparent"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "x"
                                        color: Theme.muted
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: closeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: root.requestCloseDocument(index)
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                anchors.rightMargin: 24
                                onClicked: root.setActiveDocument(index)
                            }
                        }
                    }
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
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.topMargin: 12
                    width: parent.width - 12
                    text: root.activeDocumentIndex >= 0
                          ? root.lineNumbers(editorText.lineCount)
                          : ""
                    color: Theme.faint
                    font.pixelSize: 14
                    font.family: "Cascadia Mono"
                    horizontalAlignment: Text.AlignRight
                }
            }

            TextArea {
                id: editorText
                anchors.fill: parent
                anchors.leftMargin: 52
                enabled: root.activeDocumentIndex >= 0 && !root.activeDocumentLoading
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
                placeholderText: root.activeDocumentLoading ? "正在读取..." : ""
                placeholderTextColor: Theme.faint
                background: Rectangle { color: Theme.editor }
                onTextChanged: {
                    if (!root.loadingText && root.activeDocumentIndex >= 0)
                        root.setActiveModified(true)
                    if (!root.loadingText)
                        root.updateCompletion(false)
                }
                onCursorPositionChanged: {
                    if (completionPopup.opened)
                        root.updateCompletion(false)
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Space
                            && (event.modifiers & Qt.ControlModifier)) {
                        root.updateCompletion(true)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Tab && completionPopup.opened) {
                        root.acceptCompletion(completionList.currentIndex)
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                               && completionPopup.opened) {
                        root.acceptCompletion(completionList.currentIndex)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Escape && completionPopup.opened) {
                        completionPopup.close()
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                        event.accepted = root.handleSmartEnter()
                    } else if (event.key === Qt.Key_Backspace) {
                        event.accepted = root.handleBackspacePair()
                    } else if (event.text === "(" || event.text === "[" || event.text === "{"
                               || event.text === "\"" || event.text === "'") {
                        event.accepted = root.handlePairInsert(event.text)
                    } else if (event.text === ")" || event.text === "]" || event.text === "}") {
                        event.accepted = root.handleClosePair(event.text)
                    }
                }
            }

            CodeHighlighter {
                textDocument: editorText.textDocument
                language: root.currentLanguage
                darkTheme: Theme.dark
            }

            Popup {
                id: completionPopup
                width: 292
                height: Math.min(238, completionList.contentHeight + 8)
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
                    id: completionList
                    anchors.fill: parent
                    clip: true
                    model: root.completionSuggestions
                    currentIndex: 0
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: completionList.width
                        height: 32
                        radius: 4
                        color: index === completionList.currentIndex ? Theme.treeSelected : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: Theme.text
                                font.pixelSize: 12
                                font.family: "Cascadia Mono"
                                elide: Text.ElideRight
                            }
                            Text {
                                text: modelData.detail
                                color: Theme.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: completionList.currentIndex = index
                            onClicked: root.acceptCompletion(index)
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: root.activeDocumentIndex < 0
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
                    text: root.activeDocumentIndex >= 0 ? "UTF-8" : ""
                    color: "#ffffff"
                    font.pixelSize: 11
                }
                Text {
                    text: root.activeDocumentIndex >= 0 ? "CRLF/LF" : ""
                    color: "#ffffff"
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: root.activeDocumentIndex >= 0
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
                root.openDocuments = []
                root.activeDocumentIndex = -1
                root.pendingOpenPath = ""
                root.loadingText = true
                editorText.text = ""
                root.loadingText = false
                root.modified = false
            }
        }
        function onContentLoaded(path, text) {
            root.upsertLoadedDocument(path, text)
        }
        function onOpenFailed(path, message) {
            root.handleOpenFailed(path, message)
        }
        function onSaveSucceeded(path) {
            var index = root.findDocumentIndex(path)
            if (index < 0)
                return
            var doc = Object.assign({}, root.openDocuments[index])
            doc.modified = false
            if (index === root.activeDocumentIndex)
                doc.text = editorText.text
            root.replaceDocument(index, doc)
            if (index === root.activeDocumentIndex)
                root.modified = false
        }
        function onSaveFailed(path, message) {
            var index = root.findDocumentIndex(path)
            if (index < 0)
                return
            var doc = Object.assign({}, root.openDocuments[index])
            doc.modified = true
            root.replaceDocument(index, doc)
            if (index === root.activeDocumentIndex)
                root.modified = true
        }
    }

    Dialog {
        id: closeFileDialog
        property int index: -1
        property string fileName: ""

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
                text: "关闭未保存文件"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: "“" + closeFileDialog.fileName + "” 有未保存修改，关闭会丢弃这些修改。"
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
                    onClicked: closeFileDialog.reject()
                }
                StyledButton {
                    text: "丢弃并关闭"
                    primary: true
                    onClicked: closeFileDialog.accept()
                }
            }
        }

        onAccepted: {
            root.closeDocument(index)
            index = -1
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
