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
        { label: "Python 版本",  cmd: "python --version" },
        { label: "当前目录",     cmd: "pwd" },
        { label: "环境列表",     cmd: "conda info --envs" },
        { label: "最近文件",     cmd: "ls -lt | head" }
    ]
    property var expandedProcessGroups: ({})

    function pct(value) {
        return Math.max(0, Math.min(100, Number(value) || 0))
    }

    function memoryText(device) {
        var used = Number(device.memoryUsed) || 0
        var total = Number(device.memoryTotal) || 0
        if (total <= 100 && device.kind === "CPU")
            return used + "%"
        if (total <= 0)
            return used + " MiB"
        return used + " / " + total + " MiB"
    }

    function groupExpanded(device) {
        return root.expandedProcessGroups[device] !== false
    }

    function toggleGroup(device) {
        var next = Object.assign({}, root.expandedProcessGroups)
        next[device] = !root.groupExpanded(device)
        root.expandedProcessGroups = next
    }

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
            TabButton {
                id: resourcesTab
                text: "资源"
                contentItem: Text {
                    text: resourcesTab.text
                    color: resourcesTab.checked ? Theme.text : Theme.muted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 13
                }
                background: Rectangle {
                    color: resourcesTab.checked ? Theme.panel : Theme.window
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: resourcesTab.checked ? Theme.accent : "transparent"
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

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                ColumnLayout {
                    width: Math.max(parent.width, 300)
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: 14
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: "资源快照"
                                color: Theme.text
                                font.pixelSize: 14
                                font.bold: true
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.controller.resourceSnapshotText
                                color: Theme.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                        StyledButton {
                            text: root.controller.resourceSnapshotBusy ? "采集中" : "刷新"
                            enabled: !root.controller.resourceSnapshotBusy
                            implicitWidth: 72
                            onClicked: root.controller.refreshResourceSnapshot()
                        }
                    }

                    Repeater {
                        model: root.controller.resourceDevices
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            height: 96
                            radius: 8
                            color: Theme.panel
                            border.color: Theme.border

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 7
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        text: modelData.id
                                        color: Theme.text
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: Theme.muted
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: root.pct(modelData.utilization) + "%"
                                        color: Theme.accent
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                                Rectangle {
                                    id: utilBack
                                    Layout.fillWidth: true
                                    height: 9
                                    radius: 4
                                    color: Theme.panelAlt
                                    Rectangle {
                                        width: Math.max(4, utilBack.width * root.pct(modelData.utilization) / 100)
                                        height: parent.height
                                        radius: 4
                                        color: modelData.kind === "GPU" ? Theme.accent : Theme.success
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: "显存/内存 " + root.memoryText(modelData)
                                        color: Theme.faint
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: modelData.kind
                                        color: Theme.faint
                                        font.pixelSize: 11
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        text: "磁盘用量"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Repeater {
                        model: root.controller.resourceDisks
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            height: 62
                            radius: 7
                            color: Theme.panel
                            border.color: Theme.borderSubtle

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 9
                                spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.mount + " · " + modelData.filesystem
                                        color: Theme.text
                                        font.pixelSize: 12
                                        font.family: "Cascadia Mono"
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        text: modelData.percent + "%"
                                        color: Number(modelData.percent) >= 85 ? Theme.warning : Theme.accent
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                                Rectangle {
                                    id: diskBack
                                    Layout.fillWidth: true
                                    height: 8
                                    radius: 4
                                    color: Theme.panelAlt
                                    Rectangle {
                                        width: Math.max(4, diskBack.width * root.pct(modelData.percent) / 100)
                                        height: parent.height
                                        radius: 4
                                        color: Number(modelData.percent) >= 85 ? Theme.warning : Theme.accent
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.used + " / " + modelData.size + " · 可用 " + modelData.available
                                    color: Theme.muted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        visible: root.controller.resourceDisks.length === 0
                        text: "暂无磁盘数据"
                        color: Theme.faint
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        text: "作业队列"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Repeater {
                        model: root.controller.resourceJobs
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            height: 54
                            radius: 7
                            color: Theme.panel
                            border.color: Theme.borderSubtle

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8
                                Text {
                                    text: modelData.state
                                    color: modelData.state === "RUNNING" ? Theme.success : Theme.warning
                                    font.pixelSize: 11
                                    font.bold: true
                                    Layout.preferredWidth: 74
                                    elide: Text.ElideRight
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.id + " · " + modelData.name
                                        color: Theme.text
                                        font.pixelSize: 12
                                        font.family: "Cascadia Mono"
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.user + " · " + modelData.partition
                                              + " · " + modelData.time
                                              + " · " + modelData.nodes + " node · " + modelData.where
                                        color: Theme.muted
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        visible: root.controller.resourceJobs.length === 0
                        text: "暂无作业数据"
                        color: Theme.faint
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        text: "占用进程"
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        spacing: 6

                        Repeater {
                            model: root.controller.resourceProcessGroups
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4

                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 44
                                    radius: 6
                                    color: Theme.panel
                                    border.color: Theme.borderSubtle

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8
                                        Text {
                                            text: root.groupExpanded(modelData.device) ? "v" : ">"
                                            color: Theme.muted
                                            font.pixelSize: 12
                                            font.family: "Cascadia Mono"
                                            Layout.preferredWidth: 12
                                        }
                                        Text {
                                            text: modelData.device
                                            color: modelData.device === "CPU" ? Theme.success : Theme.accent
                                            font.pixelSize: 12
                                            font.bold: true
                                            Layout.preferredWidth: 72
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.count + " 个进程"
                                                  + " · CPU " + modelData.cpu + "%"
                                                  + (Number(modelData.gpuMemory) > 0
                                                     ? " · GPU " + modelData.gpuMemory + " MiB"
                                                     : "")
                                            color: Theme.muted
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.toggleGroup(modelData.device)
                                    }
                                }

                                Repeater {
                                    model: root.groupExpanded(modelData.device) ? modelData.processes : []
                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        height: 54
                                        radius: 6
                                        color: Theme.panelAlt
                                        border.color: Theme.borderSubtle

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            spacing: 8
                                            Text {
                                                text: modelData.pid
                                                color: Theme.faint
                                                font.pixelSize: 11
                                                font.family: "Cascadia Mono"
                                                Layout.preferredWidth: 52
                                                elide: Text.ElideRight
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
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
                                                    text: modelData.user
                                                          + " · CPU " + modelData.cpu + "%"
                                                          + " · MEM " + modelData.memory + "%"
                                                          + (Number(modelData.gpuMemory) > 0
                                                             ? " · GPU " + modelData.gpuMemory + " MiB"
                                                             : "")
                                                    color: Theme.muted
                                                    font.pixelSize: 10
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.controller.resourceProcessGroups.length === 0
                            text: "暂无进程数据"
                            color: Theme.faint
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    Item { Layout.preferredHeight: 12 }
                }
            }
        }
    }
}
