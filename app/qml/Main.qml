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

    Connections {
        target: app
        function onHostKeyPromptRequested(fingerprint) {
            hostKeyDialog.show(fingerprint)
        }
    }
}
