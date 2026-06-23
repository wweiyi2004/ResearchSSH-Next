import QtQuick
import ResearchSSH

// A small colored dot reflecting an RsSessionState value.
//   0 Idle  1 Connecting  2 Connected  3 Disconnecting  4 Disconnected  5 Failed
Rectangle {
    id: root
    property int state: 0
    width: 10
    height: 10
    radius: width / 2

    color: {
        switch (root.state) {
        case 2: return Theme.success;      // Connected
        case 1: case 3: return Theme.warning; // Connecting / Disconnecting
        case 4: return Theme.muted;        // Disconnected
        case 5: return Theme.danger;       // Failed
        default: return Theme.faint;       // Idle / unknown
        }
    }

    // Subtle pulse while transitioning.
    SequentialAnimation on opacity {
        running: root.state === 1 || root.state === 3
        loops: Animation.Infinite
        NumberAnimation { from: 1.0; to: 0.3; duration: 600 }
        NumberAnimation { from: 0.3; to: 1.0; duration: 600 }
    }
}
