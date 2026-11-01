import QtQuick 2.7
import QtQuick.Dialogs 1.2 as OriginalDialogs
import QtWebEngine 1.5

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
            // which shows as plain text; the site's name only in the line under it
            var fileName = String(download.path).split(/[\\/]/).pop() || "this file";
            // the host after any user-info part, read as browsers read it (to the last '@')
            var site = String(download.url).match(/^[a-z][a-z0-9+.-]*:\/\/(?:[^\/?#]*@)?([^\/?#@]+)/i);
            var question = desktop.messageBox({
                title: "Download \u201C" + fileName + "\u201D?",
                text: site ? "From " + site[1].replace(/[^a-z0-9.:\[\]-]/gi, "") + ". Download only files you trust."
                           : "Download only files you trust.",
                buttons: OriginalDialogs.StandardButton.Yes | OriginalDialogs.StandardButton.Cancel,
                buttonTexts: { yes: "Download" },
                defaultButton: OriginalDialogs.StandardButton.Cancel
            });
            downloadAsking = true;
            var answer = question.exec();
            downloadAsking = false;
            if (answer !== OriginalDialogs.StandardButton.Yes) {
                download.cancel();
                return;
            }
            adaptedPath = File.convertUrlToPath(downloadUrl);
            tempDir = File.getTempDir();
            download.path = tempDir + "/" + adaptedPath;
            download.accept();
            if (download.state === WebEngineDownloadItem.DownloadInterrupted) {
                console.log("download failed to complete");
            }
        })

        profile.downloadFinished.connect(function(download){
            if (download.state === WebEngineDownloadItem.DownloadCompleted) {
                File.runUnzip(download.path, downloadUrl, autoAdd);
            } else {
                console.log("The download was corrupted, state: " + download.state);
            }
            autoAdd = false;
        })
    }
}
