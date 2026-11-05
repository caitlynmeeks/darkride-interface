import QtQuick 2.0
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)
import "qrc:///qml/controls" as Controls

Controls.TabletWebView {
	profile: WebEngineProfile { httpUserAgent: "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.90 Safari/537.36"}  
}
