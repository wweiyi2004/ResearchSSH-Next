import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ResearchSSH

Rectangle {
    id: root
    property var controller

    color: Theme.editor

    function requestOpen(path, size) {
        workspaceTabs.currentIndex = 1
        editorPane.requestOpen(path, size)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 36
            color: Theme.tabBar

            TabBar {
                id: workspaceTabs
                anchors.fill: parent
                background: Rectangle { color: Theme.tabBar }

                TabButton {
                    id: terminalTab
                    width: Math.max(118, terminalLabel.implicitWidth + 32)
                    contentItem: Text {
                        id: terminalLabel
                        text: "终端"
                        color: terminalTab.checked ? Theme.text : Theme.muted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 13
                    }
                    background: Rectangle {
                        color: terminalTab.checked ? Theme.tabActive : Theme.tabInactive
                        Rectangle {
                            anchors.top: parent.top
                            width: parent.width
                            height: 2
                            color: terminalTab.checked ? Theme.accentStrong : "transparent"
                        }
                        Rectangle {
                            anchors.right: parent.right
                            width: 1
                            height: parent.height
                            color: Theme.borderSubtle
                        }
                    }
                }

                TabButton {
                    id: editorTab
                    width: Math.max(160, editorLabel.implicitWidth + 38)
                    contentItem: Text {
                        id: editorLabel
                        text: editorPane.openDocuments.length > 0
                              ? "编辑器 " + editorPane.openDocuments.length
                                + (editorPane.modified ? " *" : "")
                              : "编辑器"
                        color: editorTab.checked ? Theme.text : Theme.muted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        color: editorTab.checked ? Theme.tabActive : Theme.tabInactive
                        Rectangle {
                            anchors.top: parent.top
                            width: parent.width
                            height: 2
                            color: editorTab.checked ? Theme.accentStrong : "transparent"
                        }
                        Rectangle {
                            anchors.right: parent.right
                            width: 1
                            height: parent.height
                            color: Theme.borderSubtle
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

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: workspaceTabs.currentIndex

            TerminalPane {
                controller: root.controller
                showHeader: false
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            CodeEditorPane {
                id: editorPane
                controller: root.controller
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }

}
