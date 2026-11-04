import QtQuick 2.7
import "../js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1); a file selector page resolves from hifi/, not +webengine/
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
            tempDir = File.getTempDir();
            download.downloadDirectory = tempDir;
            download.downloadFileName = fileName;
            download.accept();
            if (download.state === WebEngineDownloadItem.DownloadInterrupted) {
                console.log("download failed to complete");
            }
        })

        profile.downloadFinished.connect(function(download){
            if (download.state === WebEngineDownloadItem.DownloadCompleted) {
                File.runUnzip(download.downloadDirectory + "/" + download.downloadFileName, downloadUrl, autoAdd);
            } else {
                console.log("The download was corrupted, state: " + download.state);
            }
            autoAdd = false;
        })
    }
}
