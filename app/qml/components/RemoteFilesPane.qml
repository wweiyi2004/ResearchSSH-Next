import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import ResearchSSH

// Remote file explorer. Opening a text file is handled by the center workspace.
Rectangle {
    id: root
    property var controller
    property var editorHost
    property string selectedPath: ""
    property string selectedName: ""

    color: Theme.window

    function formatSize(bytes) {
        if (bytes < 1024)
            return bytes + " B"
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(1) + " KB"
        return (bytes / (1024 * 1024)).toFixed(1) + " MB"
    }

    function validRemoteName(name) {
        return name.trim().length > 0 && name.indexOf("/") < 0 && name.indexOf("\\") < 0
    }

    function baseName(path) {
        var s = String(path)
        while (s.length > 1 && s.endsWith("/"))
            s = s.slice(0, -1)
        var slash = s.lastIndexOf("/")
        return slash >= 0 ? s.slice(slash + 1) : s
    }

    function parentDir(path) {
        var s = String(path)
        while (s.length > 1 && s.endsWith("/"))
            s = s.slice(0, -1)
        var slash = s.lastIndexOf("/")
        if (slash < 0)
            return ""
        if (slash === 0)
            return "/"
        return s.slice(0, slash)
    }

    function fileNameFromUrl(url) {
        var text = decodeURIComponent(String(url))
        var slash = text.lastIndexOf("/")
        return slash >= 0 ? text.slice(slash + 1) : text
    }

    function childExists(dir, name) {
        return root.controller.fileTree.childExists(dir, name)
    }

    function requestOpen(path, size) {
        if (!root.controller.fileAvailable)
            return
        if (root.editorHost && root.editorHost.requestOpen)
            root.editorHost.requestOpen(path, size)
        else
            root.controller.openPath(path)
    }

    function requestPaste(destDir) {
        if (!root.controller.fileAvailable)
            return
        var name = root.controller.clipboardName
        if (!name || name.length === 0) {
            root.controller.paste(destDir)
            return
        }
        if (root.childExists(destDir, name)) {
            overwriteDialog.mode = "paste"
            overwriteDialog.path = ""
            overwriteDialog.destDir = destDir
            overwriteDialog.localFile = ""
            overwriteDialog.targetName = name
            overwriteDialog.open()
            return
        }
        root.controller.paste(destDir)
    }

    function requestRename(path, newName) {
        if (!root.controller.fileAvailable)
            return
        var clean = newName.trim()
        var dir = root.parentDir(path)
        if (!root.validRemoteName(clean))
            return
        if (clean !== root.baseName(path) && root.childExists(dir, clean)) {
            overwriteDialog.mode = "rename"
            overwriteDialog.path = path
            overwriteDialog.destDir = dir
            overwriteDialog.localFile = ""
            overwriteDialog.targetName = clean
            overwriteDialog.open()
            return
        }
        root.controller.renamePath(path, clean)
    }

    function requestMakeDir(dir, name) {
        if (!root.controller.fileAvailable)
            return
        var clean = name.trim()
        if (!root.validRemoteName(clean))
            return
        if (root.childExists(dir, clean)) {
            nameExistsDialog.name = clean
            nameExistsDialog.open()
            return
        }
        root.controller.makeDir(dir, clean)
    }

    function requestMakeFile(dir, name) {
        if (!root.controller.fileAvailable)
            return
        var clean = name.trim()
        if (!root.validRemoteName(clean))
            return
        if (root.childExists(dir, clean)) {
            nameExistsDialog.name = clean
            nameExistsDialog.open()
            return
        }
        root.controller.makeFile(dir, clean)
    }

    function targetDirForMenu() {
        return fileMenu.isDir ? fileMenu.path : root.parentDir(fileMenu.path)
    }

    function requestUpload(destDir, localFile) {
        if (!root.controller.fileAvailable)
            return
        var name = root.fileNameFromUrl(localFile)
        if (root.childExists(destDir, name)) {
            overwriteDialog.mode = "upload"
            overwriteDialog.path = ""
            overwriteDialog.destDir = destDir
            overwriteDialog.localFile = localFile
            overwriteDialog.targetName = name
            overwriteDialog.open()
            return
        }
        root.controller.uploadFile(destDir, localFile)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            height: 30
            spacing: 6

            Text {
                Layout.fillWidth: true
                text: "远端文件"
                color: Theme.text
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }

            StyledButton {
                text: "刷新"
                implicitWidth: 58
                implicitHeight: 28
                enabled: root.controller.connected
                onClicked: root.controller.reloadFiles()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            StyledButton {
                text: "新建"
                Layout.fillWidth: true
                implicitHeight: 30
                enabled: root.controller.connected && root.controller.fileAvailable
                onClicked: newRootMenu.popup()

                Menu {
                    id: newRootMenu
                    MenuItem {
                        text: "文件"
                        onTriggered: {
                            nameDialog.mode = "mkfile"
                            nameDialog.parentDir = ""
                            nameField.text = ""
                            nameDialog.open()
                        }
                    }
                    MenuItem {
                        text: "目录"
                        onTriggered: {
                            nameDialog.mode = "mkdir"
                            nameDialog.parentDir = ""
                            nameField.text = ""
                            nameDialog.open()
                        }
                    }
                }
            }
            StyledButton {
                text: "上传"
                Layout.fillWidth: true
                implicitHeight: 30
                enabled: root.controller.connected && root.controller.fileAvailable
                onClicked: {
                    uploadDialog.destDir = ""
                    uploadDialog.open()
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.controller.fileStatusText.length > 0
            text: root.controller.fileStatusText
            color: root.controller.fileAvailable ? Theme.muted : Theme.warning
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        Text {
            Layout.fillWidth: true
            visible: root.controller.fileBusy
            text: "文件操作进行中 (" + root.controller.pendingFileOperations + ")"
            color: Theme.accent
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.tree
            border.color: Theme.borderSubtle
            radius: 3

            TreeView {
                id: tree
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.controller.fileTree
                columnWidthProvider: function(column) { return tree.width }
                boundsBehavior: Flickable.StopAtBounds

                onWidthChanged: forceLayout()

                delegate: Rectangle {
                    id: rowDelegate
                    required property int row
                    required property int depth
                    required property bool expanded
                    required property string fileName
                    required property string path
                    required property bool isDir
                    required property bool editable
                    required property var size

                    readonly property bool selected: root.selectedPath === rowDelegate.path
                    readonly property string displayName: rowDelegate.fileName && rowDelegate.fileName.length > 0 ? rowDelegate.fileName : root.baseName(rowDelegate.path)

                    width: tree.width
                    height: 30
                    implicitHeight: 30
                    color: selected ? Theme.treeSelected
                                     : (rowMouse.containsMouse ? Theme.treeHover : "transparent")

                    Rectangle {
                        width: 3
                        height: parent.height
                        color: rowDelegate.selected ? Theme.accentStrong : "transparent"
                    }

                    ToolTip.visible: rowMouse.containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: rowDelegate.path

                    Item {
                        anchors.fill: parent

                        Text {
                            id: chevron
                            x: 6 + rowDelegate.depth * 16
                            width: 12
                            height: parent.height
                            text: rowDelegate.isDir
                                  ? (rowDelegate.expanded ? "v" : ">")
                                  : ""
                            color: rowDelegate.selected ? Theme.text : Theme.muted
                            font.pixelSize: 11
                            font.family: "Cascadia Mono"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        Item {
                            id: fileIcon
                            x: chevron.x + chevron.width + 6
                            y: 5
                            width: 20
                            height: 20

                            Item {
                                anchors.fill: parent
                                visible: rowDelegate.isDir
                                Rectangle {
                                    x: 3
                                    y: 4
                                    width: 8
                                    height: 4
                                    radius: 1
                                    color: Theme.dark ? "#e8b84e" : "#f5c04f"
                                }
                                Rectangle {
                                    x: 2
                                    y: 7
                                    width: 16
                                    height: 10
                                    radius: 2
                                    color: rowDelegate.expanded ? "#f3c766" : "#f0c04f"
                                    border.color: "#c7972e"
                                }
                            }

                            Item {
                                anchors.fill: parent
                                visible: !rowDelegate.isDir
                                Rectangle {
                                    x: 5
                                    y: 2
                                    width: 12
                                    height: 16
                                    radius: 1
                                    color: rowDelegate.editable
                                           ? (Theme.dark ? "#f3f6fb" : "#ffffff")
                                           : (Theme.dark ? "#d7d7d7" : "#f6f8fa")
                                    border.color: Theme.muted
                                }
                                Rectangle {
                                    x: 13
                                    y: 2
                                    width: 4
                                    height: 4
                                    color: rowDelegate.editable ? "#dbeafe" : "#c8c8c8"
                                    border.color: Theme.muted
                                }
                            }
                        }

                        Text {
                            id: nameLabel
                            x: fileIcon.x + fileIcon.width + 7
                            y: 0
                            width: Math.max(20, parent.width - x - sizeLabel.width - 12)
                            height: parent.height
                            text: rowDelegate.displayName
                            color: rowDelegate.selected ? Theme.text : Theme.textSoft
                            font.pixelSize: 13
                            elide: Text.ElideMiddle
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            id: sizeLabel
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            width: (!rowDelegate.isDir && tree.width > 340) ? 58 : 0
                            height: parent.height
                            visible: !rowDelegate.isDir && tree.width > 300
                            text: root.formatSize(Number(rowDelegate.size))
                            color: rowDelegate.selected ? Theme.accent : Theme.faint
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        function selectRow() {
                            root.selectedPath = rowDelegate.path
                            root.selectedName = rowDelegate.displayName
                        }

                        function openContextMenu() {
                            fileMenu.path = rowDelegate.path
                            fileMenu.name = rowDelegate.displayName
                            fileMenu.isDir = rowDelegate.isDir
                            fileMenu.editable = rowDelegate.editable
                            fileMenu.size = Number(rowDelegate.size)
                            fileMenu.popup()
                        }

                        onPressed: function(mouse) {
                            selectRow()
                            if (mouse.button === Qt.RightButton)
                                openContextMenu()
                        }
                        onDoubleClicked: {
                            if (rowDelegate.isDir) {
                                tree.toggleExpanded(rowDelegate.row)
                            } else if (rowDelegate.editable) {
                                root.requestOpen(rowDelegate.path, rowDelegate.size)
                            }
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: !root.controller.fileAvailable
                text: root.controller.fileStatusText
                color: Theme.faint
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                width: Math.min(parent.width - 32, 300)
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 28
            radius: 3
            color: Theme.panelAlt
            border.color: Theme.borderSubtle

            Text {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: Text.AlignVCenter
                text: root.selectedPath.length > 0 ? root.selectedPath : "未选择文件"
                color: root.selectedPath.length > 0 ? Theme.textSoft : Theme.faint
                font.pixelSize: 11
                font.family: "Cascadia Mono"
                elide: Text.ElideMiddle
            }
        }
    }

    Menu {
        id: fileMenu
        property string path: ""
        property string name: ""
        property bool isDir: false
        property bool editable: false
        property var size: 0

        MenuItem {
            text: "打开到编辑器"
            enabled: root.controller.fileAvailable && fileMenu.editable && !fileMenu.isDir
            onTriggered: root.requestOpen(fileMenu.path, fileMenu.size)
        }
        MenuSeparator {}
        MenuItem {
            text: "剪切"
            enabled: root.controller.fileAvailable
            onTriggered: root.controller.cutPath(fileMenu.path)
        }
        MenuItem {
            text: "复制"
            enabled: root.controller.fileAvailable
            onTriggered: root.controller.copyPath(fileMenu.path)
        }
        MenuItem {
            text: root.controller.clipboardCut ? "移动到此处" : "粘贴到此处"
            enabled: root.controller.fileAvailable && fileMenu.isDir && root.controller.clipboardName.length > 0
            onTriggered: root.requestPaste(fileMenu.path)
        }
        MenuItem {
            text: "重命名"
            enabled: root.controller.fileAvailable
            onTriggered: {
                nameDialog.mode = "rename"
                nameDialog.path = fileMenu.path
                nameDialog.parentDir = root.parentDir(fileMenu.path)
                nameField.text = fileMenu.name
                nameDialog.open()
            }
        }
        MenuItem {
            text: "删除"
            enabled: root.controller.fileAvailable
            onTriggered: {
                deleteDialog.path = fileMenu.path
                deleteDialog.name = fileMenu.name
                deleteDialog.isDir = fileMenu.isDir
                deleteDialog.open()
            }
        }
        MenuSeparator {}
        MenuItem {
            text: "新建文件"
            enabled: root.controller.fileAvailable
            onTriggered: {
                nameDialog.mode = "mkfile"
                nameDialog.parentDir = root.targetDirForMenu()
                nameField.text = ""
                nameDialog.open()
            }
        }
        MenuItem {
            text: "新建目录"
            enabled: root.controller.fileAvailable
            onTriggered: {
                nameDialog.mode = "mkdir"
                nameDialog.parentDir = root.targetDirForMenu()
                nameField.text = ""
                nameDialog.open()
            }
        }
        MenuItem {
            text: "上传到此处"
            enabled: root.controller.fileAvailable && fileMenu.isDir
            onTriggered: {
                uploadDialog.destDir = fileMenu.path
                uploadDialog.open()
            }
        }
    }

    Dialog {
        id: nameDialog
        property string mode: "rename"
        property string path: ""
        property string parentDir: ""

        modal: true
        anchors.centerIn: parent
        width: 300
        padding: 0
        title: mode === "rename" ? "重命名" : (mode === "mkfile" ? "新建文件" : "新建目录")

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
                text: nameDialog.mode === "rename" ? "输入新名称" : (nameDialog.mode === "mkfile" ? "输入文件名称" : "输入目录名称")
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                color: Theme.text
                selectByMouse: true
                background: Rectangle {
                    radius: 4
                    color: Theme.panelAlt
                    border.color: nameField.activeFocus ? Theme.accent : Theme.border
                }
                onAccepted: nameDialog.accept()
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "取消"
                    onClicked: nameDialog.reject()
                }
                StyledButton {
                    text: "确定"
                    primary: true
                    enabled: root.validRemoteName(nameField.text)
                    onClicked: nameDialog.accept()
                }
            }
        }

        onAccepted: {
            if (mode === "rename")
                root.requestRename(path, nameField.text)
            else if (mode === "mkfile")
                root.requestMakeFile(parentDir, nameField.text)
            else
                root.requestMakeDir(parentDir, nameField.text)
        }
    }

    Dialog {
        id: overwriteDialog
        property string mode: "upload"
        property string path: ""
        property string destDir: ""
        property url localFile: ""
        property string targetName: ""

        modal: true
        anchors.centerIn: parent
        width: 340
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
                text: "确认覆盖"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: "目标目录中已存在：" + overwriteDialog.targetName
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
                    onClicked: overwriteDialog.reject()
                }
                StyledButton {
                    text: "覆盖"
                    primary: true
                    accent: Theme.warning
                    onClicked: overwriteDialog.accept()
                }
            }
        }

        onAccepted: {
            if (mode === "upload")
                root.controller.uploadFile(destDir, localFile)
            else if (mode === "paste")
                root.controller.paste(destDir)
            else if (mode === "rename")
                root.controller.renamePath(path, targetName)
        }
    }

    Dialog {
        id: nameExistsDialog
        property string name: ""

        modal: true
        anchors.centerIn: parent
        width: 300
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
                text: "名称已存在"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: nameExistsDialog.name
                color: Theme.textSoft
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "确定"
                    primary: true
                    onClicked: nameExistsDialog.accept()
                }
            }
        }
    }

    Dialog {
        id: deleteDialog
        property string path: ""
        property string name: ""
        property bool isDir: false

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
                text: "确认删除"
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                text: deleteDialog.isDir
                      ? "将递归删除目录及其内容：" + deleteDialog.name
                      : "将删除文件：" + deleteDialog.name
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
                    onClicked: deleteDialog.reject()
                }
                StyledButton {
                    text: "删除"
                    primary: true
                    accent: Theme.danger
                    onClicked: deleteDialog.accept()
                }
            }
        }

        onAccepted: root.controller.deletePath(path, isDir)
    }

    FileDialog {
        id: uploadDialog
        property string destDir: ""
        title: "选择要上传的文件"
        fileMode: FileDialog.OpenFile
        onAccepted: root.requestUpload(destDir, selectedFile)
    }
}
