//
//  ApplicationDialogs.cpp
//  interface/src
//
//  Application's asks and dialogs. The questions the client puts to the viewer: the ban confirmation
//  (userKickConfirmation); the avatar, script and attachment asks for a dropped or opened URL (askToSetAvatarUrl,
//  askToLoadScript, askToWearAvatarAttachmentUrl, displayAvatarAttachmentWarning); the URL handler's "open this
//  file?" ask (showUrlHandler), with openDirectory, which opens a folder past it. The dialogs and windows it opens:
//  showDialog (on the tablet, or on the desktop), the script logs, the asset server (showAssetServerWidget), the model
//  packager, the open-script and run-from-the-web dialogs with the remembered folder (loadDialog,
//  loadScriptURLDialog, get/setPreviousScriptLocation), the avatar importer and Put Avatar in My Files, the LOD tools,
//  entity statistics and domain connection dialogs, the log window (toggleLogDialog, recreateLogWindow) and the entity
//  script server's log window; getSampleSound, the preferences' test sound. The members as they stood in
//  Application.cpp, in their order; a home of their own until an owner takes them (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include "ApplicationConstants.h"
#include <thread>
#include <QtGui/QDesktopServices>
#include <QtQml/QQmlContext>
#include <QtWidgets/QMessageBox>
#include <AddressManager.h>
#include <MainWindow.h>
#include <NetworkAccessManager.h>
#include <OffscreenUi.h>
#include <PathUtils.h>
#include <ScriptEngines.h>
#include <ui/TabletScriptingInterface.h>
#include <UsersScriptingInterface.h>
#include "avatar/AvatarImporter.h"
#include "InterfaceLogging.h"
#include "ModelPackager.h"
#include "scripting/HMDScriptingInterface.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/DialogsManager.h"
#if defined(Q_OS_WIN)
#include <Windows.h>   // first: VersionHelpers.h needs its types
#include <VersionHelpers.h>
#endif
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's

// the system tablet's name, declared in ApplicationConstants.h (ApplicationUi.cpp reads it too)
const QString SYSTEM_TABLET = "com.highfidelity.interface.tablet.system";   // declared in ApplicationConstants.h; ApplicationUi.cpp reads it too

// The name of the world being joined, for a popup, or "" when it has none to show (an address is not a name).
QString worldNameForPopup() {   // DomainSession.cpp reads it too
    return DependencyManager::get<AddressManager>()->getDomainLabel().trimmed();
}

void Application::userKickConfirmation(const QUuid& nodeID) {
    auto avatarHashMap = DependencyManager::get<AvatarHashMap>();
    auto avatar = avatarHashMap->getAvatarBySessionID(nodeID);

    QString userName = avatar ? avatar->getSessionDisplayName().trimmed() : QString();
    const QString world = worldNameForPopup();

    // A ban keeps them out until it is lifted, which is what the People app's Ban button asks for.
    ModalDialogListener* dlg = OffscreenUi::asyncQuestion(
        "Ban " + (userName.isEmpty() ? QString("this person") : userName) + " from " +
            (world.isEmpty() ? QString("this world") : world) + "?",
        (userName.isEmpty() ? QString("They") : userName) + " can't come back until you lift the ban in the world settings.",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

    if (QQuickItem* box = dlg->getDialogItem()) {
        OffscreenUi::setButtonText(box, QMessageBox::Yes, "Ban");
        box->setProperty("destructiveButton", (int)QMessageBox::Yes);

        QObject::connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
            QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);

            bool yes = (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Yes);
            // ask the NodeList to kick the user with the given session ID

            if (yes) {
                DependencyManager::get<NodeList>()->kickNodeBySessionID(nodeID);
            }

            DependencyManager::get<UsersScriptingInterface>()->setWaitForKickResponse(false);
        });
        DependencyManager::get<UsersScriptingInterface>()->setWaitForKickResponse(true);
    }
}

bool Application::askToSetAvatarUrl(const QString& url) {
    QUrl realUrl(url);
    if (realUrl.isLocalFile()) {
        // an avatar on this computer goes through the importer, as with File > Import Avatar
        AvatarImporter::importFile(realUrl.toLocalFile());
        return true;
    }

    // Download the FST file, to attempt to determine its model type
    QVariantHash fstMapping = FSTReader::downloadMapping(url);

    FSTReader::ModelType modelType = FSTReader::predictModelType(fstMapping);

    QString modelName = fstMapping["name"].toString();
    QString modelLicense = fstMapping["license"].toString();
    // the avatar's own name in the popups, else its file's
    const QString shownName = modelName.trimmed().isEmpty() ? QFileInfo(realUrl.path()).completeBaseName() : modelName.trimmed();

    auto wear = [=] () {
        getMyAvatar()->useFullAvatarURL(url, modelName);
        emit fullAvatarURLChanged(url, modelName);
    };

    if (!modelLicense.isEmpty()) {
        ModalDialogListener* dlg = OffscreenUi::asyncQuestion(shownName + " comes with a license",
                                                              "Read it before you wear this avatar.",
                                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (QQuickItem* box = dlg->getDialogItem()) {
            OffscreenUi::setButtonText(box, QMessageBox::Yes, "Agree and Wear");
            OffscreenUi::setButtonText(box, QMessageBox::No, "Don't Wear");
            box->setProperty("detailedText", modelLicense);
        }
        QObject::connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
            QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);

            if (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Yes) {
                if (modelType == FSTReader::HEAD_AND_BODY_MODEL) {
                    wear();
                } else {
                    OffscreenUi::asyncWarning(shownName + " can't be worn", "It needs a head and a body.");
                }
            } else {
                qCDebug(interfaceapp) << "Declined to agree to avatar license";
            }
        });
    } else {
        ModalDialogListener* dlg = OffscreenUi::asyncQuestion("Wear " + shownName + "?",
                                                              realUrl.host().isEmpty() ? QString() : "From " + realUrl.host() + ".",
                                                              QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
        OffscreenUi::setButtonText(dlg->getDialogItem(), QMessageBox::Ok, "Wear");
        QObject::connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
            QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);

            if (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Ok) {
                wear();
            } else {
                qCDebug(interfaceapp) << "Declined to use the avatar";
            }
        });
    }

    return true;
}


bool Application::askToLoadScript(const QString& scriptFilenameOrURL) {
    QUrl scriptURL { scriptFilenameOrURL };

    QString name = QFileInfo(scriptURL.path()).fileName();
    if (name.isEmpty()) {
        name = "this script";
    }
    QString message = "Only run scripts from people you trust.";
    if (!scriptURL.isLocalFile() && !scriptURL.host().isEmpty()) {
        message = "From " + scriptURL.host() + ". " + message;
    }
    ModalDialogListener* dlg = OffscreenUi::asyncQuestion(getWindow(), "Run " + name + "?", message,
                                                           QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    OffscreenUi::setButtonText(dlg->getDialogItem(), QMessageBox::Yes, "Run");

    QObject::connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
        const QString& fileName = scriptFilenameOrURL;
        if (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Yes) {
            qCDebug(interfaceapp) << "Chose to run the script: " << fileName;
            DependencyManager::get<ScriptEngines>()->loadScript(fileName);
        } else {
            qCDebug(interfaceapp) << "Declined to run the script";
        }
        QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);
    });
    return true;
}

bool Application::askToWearAvatarAttachmentUrl(const QString& url) {
    QNetworkAccessManager& networkAccessManager = NetworkAccessManager::getInstance();
    QNetworkRequest networkRequest = QNetworkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    networkRequest.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    QNetworkReply* reply = networkAccessManager.get(networkRequest);
    int requestNumber = ++_avatarAttachmentRequest;
    connect(reply, &QNetworkReply::finished, [this, reply, url, requestNumber]() {

        if (requestNumber != _avatarAttachmentRequest) {
            // this request has been superseded by another more recent request
            reply->deleteLater();
            return;
        }

        QNetworkReply::NetworkError networkError = reply->error();
        if (networkError == QNetworkReply::NoError) {
            // download success
            QByteArray contents = reply->readAll();

            QJsonParseError jsonError;
            auto doc = QJsonDocument::fromJson(contents, &jsonError);
            if (jsonError.error == QJsonParseError::NoError) {

                auto jsonObject = doc.object();

                // retrieve optional name field from JSON
                QString name;
                auto nameValue = jsonObject.value("name");
                if (nameValue.isString()) {
                    name = nameValue.toString().trimmed();
                }

                ModalDialogListener* dlg = OffscreenUi::asyncQuestion(
                    tr("Wear %1 on your avatar?").arg(name.isEmpty() ? tr("this attachment") : name), QString(),
                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
                OffscreenUi::setButtonText(dlg->getDialogItem(), QMessageBox::Ok, tr("Wear"));
                QObject::connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
                    QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);
                    if (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Ok) {
                        // add attachment to avatar
                        auto myAvatar = getMyAvatar();
                        assert(myAvatar);
                        auto attachmentDataVec = myAvatar->getAttachmentData();
                        AttachmentData attachmentData;
                        attachmentData.fromJson(jsonObject);
                        attachmentDataVec.push_back(attachmentData);
                        myAvatar->setAttachmentData(attachmentDataVec);
                    } else {
                        qCDebug(interfaceapp) << "User declined to wear the avatar attachment";
                    }
                });
            } else {
                // json parse error
                qCWarning(interfaceapp) << "Attachment JSON doesn't parse:" << url;
                displayAvatarAttachmentWarning(tr("The attachment file is damaged."));
            }
        } else {
            // download failure
            qCWarning(interfaceapp) << "Attachment JSON didn't download:" << url << reply->errorString();
            displayAvatarAttachmentWarning(tr("The attachment file couldn't be downloaded."));
        }
        reply->deleteLater();
    });
    return true;
}

void Application::displayAvatarAttachmentWarning(const QString& message) const {
    OffscreenUi::asyncWarning(tr("Couldn't load the attachment"), message);
}

void Application::showDialog(const QUrl& widgetUrl, const QUrl& tabletUrl, const QString& name) const {
    auto tablet = DependencyManager::get<TabletScriptingInterface>()->getTablet(SYSTEM_TABLET);
    auto hmd = DependencyManager::get<HMDScriptingInterface>();
    bool onTablet = false;

    if (!tablet->getToolbarMode()) {
        onTablet = tablet->pushOntoStack(tabletUrl);
        if (onTablet) {
            toggleTabletUI(true);
        }
    } else {
        getOffscreenUI()->show(widgetUrl, name);
    }
}

void Application::showScriptLogs() {
    QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
    defaultScriptsLoc.setPath(defaultScriptsLoc.path() + "developer/debugging/debugWindow.js");
    DependencyManager::get<ScriptEngines>()->loadScript(defaultScriptsLoc.toString());
}

void Application::showAssetServerWidget(QString filePath) {
    if (!DependencyManager::get<NodeList>()->getThisNodeCanWriteAssets() || getLoginDialogPoppedUp()) {
        return;
    }
    static const QUrl url { "hifi/AssetServer.qml" };

    auto startUpload = [=](QQmlContext* context, QObject* newObject){
        if (!filePath.isEmpty()) {
            emit uploadRequest(filePath);
        }
    };
    auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = dynamic_cast<TabletProxy*>(tabletScriptingInterface->getTablet(SYSTEM_TABLET));
    auto hmd = DependencyManager::get<HMDScriptingInterface>();
    if (tablet->getToolbarMode()) {
        getOffscreenUI()->show(url, "AssetServer", startUpload);
    } else {
        if (!hmd->getShouldShowTablet() && !isHMDMode()) {
            getOffscreenUI()->show(url, "AssetServer", startUpload);
        } else {
            static const QUrl url("qrc:///qml/hifi/dialogs/TabletAssetServer.qml");
            if (!tablet->isPathLoaded(url)) {
                tablet->pushOntoStack(url);
            }
        }
    }

    startUpload(nullptr, nullptr);
}

void Application::packageModel() {
    ModelPackager::package();
}

void Application::loadDialog() {
    ModalDialogListener* dlg = OffscreenUi::getOpenFileNameAsync(_glWidget, tr("Open script"),
                                                                 getPreviousScriptLocation(),
                                                                 tr("JavaScript files (*.js)"));
    connect(dlg, &ModalDialogListener::response, this, [=] (QVariant answer) {
        disconnect(dlg, &ModalDialogListener::response, this, nullptr);
        const QString& response = answer.toString();
        if (!response.isEmpty() && QFile(response).exists()) {
            setPreviousScriptLocation(QFileInfo(response).absolutePath());
            DependencyManager::get<ScriptEngines>()->loadScript(response, true, false, false, true);  // Don't load from cache
        }
    });
}

void Application::importAvatarDialog() {
    static Setting::Handle<QString> previousLocation("previousAvatarImportLocation",
                                                     QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    // Qt's standard file dialog, as ModelSelector uses: the macOS panel that OffscreenUi's modeless dialog opens
    // showed every file greyed out (10-12), whatever the filter
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Import Avatar"), previousLocation.get(),
                                                      tr("Characters and avatars (*)"));
    qCDebug(interfaceapp) << "Import Avatar: picked" << (path.isEmpty() ? QString("nothing") : path);
    if (!path.isEmpty() && QFile(path).exists()) {
        previousLocation.set(QFileInfo(path).absolutePath());
        AvatarImporter::importFile(path);
    }
}

void Application::putAvatarInMyFiles() {
    AvatarImporter::putInMyFiles(getMyAvatar()->getFullAvatarURLFromPreferences());
}

QString Application::getPreviousScriptLocation() {
    QString result = _previousScriptLocation.get();
    return result;
}

void Application::setPreviousScriptLocation(const QString& location) {
    _previousScriptLocation.set(location);
}

void Application::loadScriptURLDialog() const {
    ModalDialogListener* dlg = OffscreenUi::getTextAsync(OffscreenUi::ICON_NONE, "Run a Script from the Web", QString());
    if (QQuickItem* box = dlg->getDialogItem()) {
        OffscreenUi::setButtonText(box, QMessageBox::Ok, "Run");
        box->setProperty("placeholderText", QString::fromUtf8("https://…/script.js"));
    }
    connect(dlg, &ModalDialogListener::response, this, [=] (QVariant response) {
        disconnect(dlg, &ModalDialogListener::response, this, nullptr);
        const QString& newScript = response.toString();
        if (QUrl(newScript).scheme() == "atp") {
            OffscreenUi::asyncWarning("Can't run a script from an atp: address", "Use an https: address.");
        } else if (!newScript.isEmpty()) {
            DependencyManager::get<ScriptEngines>()->loadScript(newScript.trimmed());
        }
    });
}

SharedSoundPointer Application::getSampleSound() const {
    return _sampleSound;
}

void Application::loadLODToolsDialog() {
    auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = dynamic_cast<TabletProxy*>(tabletScriptingInterface->getTablet(SYSTEM_TABLET));
    if (tablet->getToolbarMode() || (!tablet->getTabletRoot() && !isHMDMode())) {
        auto dialogsManager = DependencyManager::get<DialogsManager>();
        dialogsManager->lodTools();
    } else {
        tablet->pushOntoStack("hifi/dialogs/TabletLODTools.qml");
    }
}

void Application::loadEntityStatisticsDialog() {
    auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = dynamic_cast<TabletProxy*>(tabletScriptingInterface->getTablet(SYSTEM_TABLET));
    if (tablet->getToolbarMode() || (!tablet->getTabletRoot() && !isHMDMode())) {
        auto dialogsManager = DependencyManager::get<DialogsManager>();
        dialogsManager->octreeStatsDetails();
    } else {
        tablet->pushOntoStack("hifi/dialogs/TabletEntityStatistics.qml");
    }
}

void Application::loadDomainConnectionDialog() {
    auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = dynamic_cast<TabletProxy*>(tabletScriptingInterface->getTablet(SYSTEM_TABLET));
    if (tablet->getToolbarMode() || (!tablet->getTabletRoot() && !isHMDMode())) {
        auto dialogsManager = DependencyManager::get<DialogsManager>();
        dialogsManager->showDomainConnectionDialog();
    } else {
        tablet->pushOntoStack("hifi/dialogs/TabletDCDialog.qml");
    }
}

void Application::toggleLogDialog() {
#ifndef ANDROID_APP_QUEST_INTERFACE
    if (getLoginDialogPoppedUp()) {
        return;
    }
    if (! _logDialog) {

        bool keepOnTop =_keepLogWindowOnTop.get();
#ifdef Q_OS_WIN
        _logDialog = new LogDialog(keepOnTop ? qApp->getWindow() : nullptr, getLogger());
#elif !defined(Q_OS_ANDROID)
        _logDialog = new LogDialog(nullptr, getLogger());

        if (keepOnTop) {
            Qt::WindowFlags flags = _logDialog->windowFlags() | Qt::Tool;
            _logDialog->setWindowFlags(flags);
        }
#else
        Q_UNUSED(keepOnTop)
#endif
    }

    if (_logDialog->isVisible()) {
        _logDialog->hide();
    } else {
        _logDialog->show();
    }
#endif
}

 void Application::recreateLogWindow(int keepOnTop) {
     _keepLogWindowOnTop.set(keepOnTop != 0);
     if (_logDialog) {
         bool toggle = _logDialog->isVisible();
         _logDialog->close();
         _logDialog = nullptr;

         if (toggle) {
             toggleLogDialog();
         }
     }
 }

void Application::toggleEntityScriptServerLogDialog() {
    if (! _entityScriptServerLogDialog) {
        _entityScriptServerLogDialog = new EntityScriptServerLogDialog(nullptr);
    }

    if (_entityScriptServerLogDialog->isVisible()) {
        _entityScriptServerLogDialog->hide();
    } else {
        _entityScriptServerLogDialog->show();
    }
}

void Application::openDirectory(const QString& path) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "openDirectory", Q_ARG(const QString&, path));
        return;
    }

    QString dirPath = path;
    const QString FILE_SCHEME = "file:///";
    if (dirPath.startsWith(FILE_SCHEME)) {
        dirPath.remove(0, FILE_SCHEME.length());
    }
    QFileInfo fileInfo(dirPath);
    if (fileInfo.isDir()) {
        auto scheme = QUrl(path).scheme();
        QDesktopServices::unsetUrlHandler(scheme);
        QDesktopServices::openUrl(path);
        QDesktopServices::setUrlHandler(scheme, this, "showUrlHandler");
    }
}

void Application::showUrlHandler(const QUrl& url) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "showUrlHandler", Q_ARG(const QUrl&, url));
        return;
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toDisplayString();
    const QString name = QFileInfo(path).fileName();
    ModalDialogListener* dlg = OffscreenUi::asyncQuestion(
        name.isEmpty() ? QString("Open this file?") : QString::fromUtf8("Open “%1”?").arg(name),
        "A script wants to open this file on your computer.", QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
    if (QQuickItem* box = dlg->getDialogItem()) {
        box->setProperty("detailedText", path);
    }
    QObject::connect(dlg, &ModalDialogListener::response, this, [=](QVariant answer) {
        QObject::disconnect(dlg, &ModalDialogListener::response, this, nullptr);
        if (QMessageBox::Open == static_cast<QMessageBox::StandardButton>(answer.toInt())) {
            // Unset the handler, open the URL, and the reset the handler
            QDesktopServices::unsetUrlHandler(url.scheme());
            QDesktopServices::openUrl(url);
            QDesktopServices::setUrlHandler(url.scheme(), this, "showUrlHandler");
        }
    });
}
