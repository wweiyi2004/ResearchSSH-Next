pragma Singleton

import QtQuick

QtObject {
    id: theme
    property bool dark: true

    readonly property color window: dark ? "#0f1115" : "#f3f4f6"
    readonly property color header: dark ? "#151922" : "#ffffff"
    readonly property color panel: dark ? "#151922" : "#ffffff"
    readonly property color panelAlt: dark ? "#1f232b" : "#f8fafc"
    readonly property color surface: dark ? "#202020" : "#ffffff"
    readonly property color editor: dark ? "#1e1e1e" : "#ffffff"
    readonly property color editorGutter: dark ? "#1b1b1b" : "#f3f3f3"
    readonly property color terminal: dark ? "#05080d" : "#ffffff"
    readonly property color tabBar: dark ? "#252526" : "#f3f3f3"
    readonly property color tabActive: dark ? "#1e1e1e" : "#ffffff"
    readonly property color tabInactive: dark ? "#2d2d2d" : "#ececec"
    readonly property color tree: dark ? "#202020" : "#ffffff"
    readonly property color treeHover: dark ? "#2b2b2b" : "#eef6ff"
    readonly property color treeSelected: dark ? "#333b46" : "#dbeafe"
    readonly property color border: dark ? "#30363d" : "#d0d7de"
    readonly property color borderSubtle: dark ? "#343434" : "#e5e7eb"
    readonly property color text: dark ? "#f0f3f6" : "#1f2328"
    readonly property color textSoft: dark ? "#d4d4d4" : "#24292f"
    readonly property color muted: dark ? "#8b949e" : "#57606a"
    readonly property color faint: dark ? "#6e7681" : "#8c959f"
    readonly property color accent: dark ? "#2f81f7" : "#0969da"
    readonly property color accentStrong: dark ? "#007acc" : "#0969da"
    readonly property color success: dark ? "#3fb950" : "#1a7f37"
    readonly property color warning: dark ? "#d7ba7d" : "#9a6700"
    readonly property color danger: dark ? "#f14c4c" : "#cf222e"
    readonly property color button: dark ? "#21262d" : "#f6f8fa"
    readonly property color buttonDown: dark ? "#2a2f3a" : "#eaeef2"
    readonly property color selectionText: dark ? "#ffffff" : "#0b1f33"

    function toggle() {
        dark = !dark
    }
}
