import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

// Three-pane shell: servers | terminal/editor workspace | status & files.
ApplicationWindow {
    id: window
    width: 1180
    height: 720
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    title: "ResearchSSH-Next"
    color: Theme.window

    // Top bar.
    header: Rectangle {
        height: 48
        color: Theme.header
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 10
            Image {
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                source: "qrc:/app/assets/researchssh-next.png"
                fillMode: Image.PreserveAspectFit
                mipmap: true
            }
            Text {
                text: "ResearchSSH"
                color: Theme.text
                font.pixelSize: 16
                font.bold: true
            }
            Text {
                text: "Next"
                color: Theme.accent
                font.pixelSize: 16
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            StyledButton {
                text: Theme.dark ? "亮色" : "暗色"
                implicitWidth: 62
                implicitHeight: 30
                onClicked: Theme.toggle()
            }
            StatusDot { state: app.connectionState }
            Text {
                text: app.connectionStateText
                color: Theme.muted
                font.pixelSize: 13
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 2
            color: SplitHandle.pressed ? Theme.accent : Theme.border
        }

        ServerPane {
            controller: app
            onNewConnectionRequested: connectDialog.open()
            onSettingsRequested: settingsDialog.open()
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 200
        }

        WorkspacePane {
            id: workspacePane
            controller: app
            SplitView.fillWidth: true
            SplitView.minimumWidth: 460
        }

        StatusPane {
            controller: app
            editorHost: workspacePane
            SplitView.preferredWidth: 340
            SplitView.minimumWidth: 280
        }
    }

    // Dialogs (real-SSH connection + host-key confirmation).
    ConnectDialog {
        id: connectDialog
        controller: app
        onAboutToShow: resetFields()
    }

    HostKeyDialog {
        id: hostKeyDialog
        controller: app
    }

    Dialog {
        id: settingsDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, window.width - 48)
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panel
            radius: 10
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 18
                spacing: 12

                Image {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    source: "qrc:/app/assets/researchssh-next.png"
                    fillMode: Image.PreserveAspectFit
                    mipmap: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Text {
                        Layout.fillWidth: true
                        text: "设置"
                        color: Theme.text
                        font.pixelSize: 17
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "ResearchSSH-Next"
                        color: Theme.muted
                        font.pixelSize: 12
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            GridLayout {
                Layout.fillWidth: true
                Layout.margins: 18
                columns: 2
                rowSpacing: 10
                columnSpacing: 18

                Text { text: "应用版本"; color: Theme.muted; font.pixelSize: 13 }
                Text {
                    Layout.fillWidth: true
                    text: app.appVersion
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                Text { text: "核心版本"; color: Theme.muted; font.pixelSize: 13 }
                Text {
                    Layout.fillWidth: true
                    text: app.coreVersion
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                Text { text: "凭据后端"; color: Theme.muted; font.pixelSize: 13 }
                Text {
                    Layout.fillWidth: true
                    text: app.credentialBackend
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                Text { text: "热更新"; color: Theme.muted; font.pixelSize: 13 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    BusyIndicator {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        running: app.updateBusy
                        visible: running
                    }
                    Text {
                        Layout.fillWidth: true
                        text: app.updateStatusText
                        color: app.updateAvailable ? Theme.success : Theme.text
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 8
                StyledButton {
                    text: "检查热更新"
                    enabled: !app.updateBusy
                    onClicked: app.checkForUpdates()
                }
                StyledButton {
                    text: "打开下载页"
                    enabled: app.updateDownloadUrl.length > 0
                    onClicked: app.openUpdateDownload()
                }
                Item { Layout.fillWidth: true }
                StyledButton {
                    text: "关闭"
                    onClicked: settingsDialog.close()
                }
            }
        }
    }

    Connections {
        target: app
        function onHostKeyPromptRequested(fingerprint) {
            hostKeyDialog.show(fingerprint)
        }
    }
}
