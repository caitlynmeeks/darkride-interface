import QtQuick 2.7
import "../../../js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1); a file selector page resolves from hifi/, not +webengine/
import QtWebEngine   // Qt 6: versionless; this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads (the qt6 file selector)

Item {
    id: root

    property bool webViewProfileSetup: false
    property string currentUrl: ""
    property string downloadUrl: ""
    property string adaptedPath: ""
    property string tempDir: ""
    property bool downloadAsking: false   // a download question is open (one at a time)
    function setupWebEngineSettings() {
        WebEngine.settings.javascriptCanOpenWindows = true;
        WebEngine.settings.javascriptCanAccessClipboard = false;
        WebEngine.settings.spatialNavigationEnabled = false;
        WebEngine.settings.localContentCanAccessRemoteUrls = true;
    }


    function initWebviewProfileHandlers(profile) {
        downloadUrl = currentUrl;
        if (webViewProfileSetup) return;
        webViewProfileSetup = true;

        profile.downloadRequested.connect(function(download){
            // one question at a time: a page that starts more downloads while one is asked gets them refused
            if (downloadAsking) {
                download.cancel();
                return;
            }
            // a page can start a download by itself: the user says yes to each one. The file's name goes in the title,
            // which shows as plain text; the site's name only in the line under it.
            // One name, shown and written: the page's filename= (adaptedPath, the way the marketplace flow named its
            // zips), else the name the page offers; its last piece only, inside the temp folder, without control or
            // direction-override characters; an empty name, "." or ".." is refused.
            // downloadFileName and downloadDirectory since Qt 5.14; Qt 6 has no path
            adaptedPath = File.convertUrlToPath(downloadUrl);
            var fileName = String(adaptedPath || download.downloadFileName || download.path || "").split(/[\\/]/).pop()
                .replace(/[\u0000-\u001f\u007f\u200e\u200f\u202a-\u202e\u2066-\u2069]/g, "");
            if (!fileName || fileName === "." || fileName === "..") {
                download.cancel();
                return;
            }
            // the host after any user-info part, read as browsers read it (to the last '@')
            var site = String(download.url).match(/^[a-z][a-z0-9+.-]*:\/\/(?:[^\/?#]*@)?([^\/?#@]+)/i);
            var from = site ? "From " + site[1].replace(/[^a-z0-9.:\[\]-]/gi, "") + ". " : "";
            // Only a zip is saved by Darkride (into its own temp folder, for the import below). Any other file is the
            // visitor's own browser's to download: it saves under its own folder and name, with the system's quarantine
            // mark and its own progress, so Darkride writes nothing and nothing vanishes into a temp folder. Only a web
            // link is handed over: a custom scheme could start another app, and blob: or data: can't be opened outside.
            var shown = fileName.length > 80 ? fileName.slice(0, 79) + "\u2026" : fileName;   // the question stays on screen
            var isZip = /\.zip$/i.test(fileName);
            var link = String(download.url);
            var webLink = /^https?:\/\//i.test(link);
            var question = desktop.messageBox(isZip ? {
                title: "Download \u201C" + shown + "\u201D?",
                text: from + "Download only files you trust.",
                buttons: OriginalDialogs.StandardButton.Yes | OriginalDialogs.StandardButton.Cancel,
                buttonTexts: { yes: "Download" },
                defaultButton: OriginalDialogs.StandardButton.Cancel
            } : webLink ? {
                title: "Open \u201C" + shown + "\u201D in your browser?",
                text: from + "Your browser downloads it. Download only files you trust.",
                buttons: OriginalDialogs.StandardButton.Yes | OriginalDialogs.StandardButton.Cancel,
                buttonTexts: { yes: "Open in Browser" },
                defaultButton: OriginalDialogs.StandardButton.Cancel
            } : {
                title: "Can\u2019t download \u201C" + shown + "\u201D",
                text: from + "Darkride downloads only zip files here. Open the page in your browser to download it.",
                buttons: OriginalDialogs.StandardButton.Ok,
                defaultButton: OriginalDialogs.StandardButton.Ok
            });
            downloadAsking = true;
            var answer = question.exec();
            downloadAsking = false;
            if (!isZip) {
                download.cancel();
                if (webLink && answer === OriginalDialogs.StandardButton.Yes) {
                    Qt.openUrlExternally(link);
                }
                return;
            }
            if (answer !== OriginalDialogs.StandardButton.Yes) {
                download.cancel();
                return;
            }
            tempDir = File.getTempDir();
            download.downloadDirectory = tempDir;
            download.downloadFileName = fileName;
            download.accept();
            if (download.state === WebEngineDownloadRequest.DownloadInterrupted) {
                console.log("download failed to complete");
            }
        })

        profile.downloadFinished.connect(function(download){
            if (download.state === WebEngineDownloadRequest.DownloadCompleted) {
                File.runUnzip(download.downloadDirectory + "/" + download.downloadFileName, downloadUrl, autoAdd);
            } else {
                console.log("The download was corrupted, state: " + download.state);
            }
            autoAdd = false;
        })
    }
}
