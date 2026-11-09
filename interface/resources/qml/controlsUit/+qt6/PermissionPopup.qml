import QtQuick 2.5
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)

import "qrc:///qml/dialogs/alert"

// A web page asks for the microphone, the camera, the location or the screen: the alert every popup uses, naming the
// site that asks, over the page.
AlertPanel {
    id: root
    anchors.centerIn: parent
    height: implicitHeight

    property string currentRequestedPermission
    property string requestingOrigin
    signal permissionButtonPressed(int buttonNumber)

    // What the question can name, by the request's type. It's also the allow-list: a request of any other type never
    // reaches the visitor (PermissionPopupBackground.ask ends it denied).
    readonly property var permissionLanguage: ({
        [WebEnginePermission.PermissionType.MediaAudioCapture]: "wants to use your microphone",
        [WebEnginePermission.PermissionType.MediaVideoCapture]: "wants to use your camera",
        [WebEnginePermission.PermissionType.MediaAudioVideoCapture]: "wants to use your microphone and camera",
        [WebEnginePermission.PermissionType.Geolocation]: "wants to know your location",
        [WebEnginePermission.PermissionType.DesktopVideoCapture]: "wants to share your screen",
        [WebEnginePermission.PermissionType.DesktopAudioVideoCapture]: "wants to share your screen and its sound"
    })

    function canAsk(type) {
        return permissionLanguage[type] !== undefined;
    }

    // the site's name, as "music.example.com", from its address
    readonly property string site: {
        var match = /^[a-z][a-z0-9+.-]*:\/\/([^\/:?#]+)/i.exec(requestingOrigin);
        return match ? match[1] : "This page";
    }

    title: site + " " + (permissionLanguage[currentRequestedPermission] || "")
    text: "Allow it only if you trust this site."
    buttons: [
        { text: "Don't Allow", value: 0, role: "plain", cancel: true },
        { text: "Allow", value: 1, role: "default" }
    ]
    onClicked: root.permissionButtonPressed(value)
}
