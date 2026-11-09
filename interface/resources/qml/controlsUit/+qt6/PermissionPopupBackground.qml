import QtQuick 2.5
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)

Rectangle {
    id: root
    anchors.fill: parent
    color: Qt.rgba(0, 0, 0, 0.5);
    visible: false
    property string securityOrigin: 'none'
    property string feature: 'none'
    // the popup's scale: larger on a big page, as a screen in the world is seen from further away; a window on the
    // desktop sets 1, the size of every other popup there
    property real unit: Math.max(1.25, Math.min(3, width / 400, height / 300))
    // the request the open question belongs to: its answer goes to the frame that asked, and to nothing else
    property var request: null

    // A page's request: every request ends, here or at the visitor's answer. Only what the question can name reaches the
    // visitor, one question at a time.
    function ask(permission) {
        var what = permission.origin + " type " + permission.permissionType;
        if (!permissionPopupItem.canAsk(permission.permissionType)) {
            console.log("Web permission refused, not one we ask about: " + what);
            permission.deny();
            return;
        }
        if (request !== null) {
            console.log("Web permission refused, a question is open: " + what);
            permission.deny();
            return;
        }
        console.log("Web permission question: " + what);
        request = permission;
        securityOrigin = permission.origin.toString();
        feature = String(permission.permissionType);
        visible = true;
    }

    function answer(allow) {
        var asked = request;
        request = null;
        visible = false;
        securityOrigin = 'none';
        feature = 'none';
        if (asked !== null) {
            if (allow) {
                asked.grant();
            } else {
                asked.deny();
            }
        }
    }

    // The page that asked is going, or the view is: the question goes with it, so an Allow can't answer for the next page
    function withdraw() {
        if (request !== null) {
            console.log("Web permission question withdrawn: " + securityOrigin);
        }
        answer(false);
    }

    Component.onDestruction: withdraw()

    onFeatureChanged: {
        permissionPopupItem.currentRequestedPermission = feature;
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        propagateComposedEvents: false
    }

    PermissionPopup {
        id: permissionPopupItem
        requestingOrigin: root.securityOrigin
        unit: root.unit
        onPermissionButtonPressed: root.answer(buttonNumber !== 0)
    }
}
