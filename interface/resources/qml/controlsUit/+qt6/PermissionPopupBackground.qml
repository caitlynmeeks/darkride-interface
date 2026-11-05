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
    signal sendPermission(string securityOrigin, string feature, bool shouldGivePermission)

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
        onPermissionButtonPressed: {
            if (buttonNumber === 0) {
                root.sendPermission(securityOrigin, feature, false);
            } else {
                root.sendPermission(securityOrigin, feature, true);
            }
            root.visible = false;
            securityOrigin = 'none';
            feature = 'none';
        }
    }
}
