//
//  ApplicationAvatar.cpp
//  interface/src
//
//  Application's hooks on its own avatar: the accessors (getMyAvatar, getAvatarPosition), the avatar's own scripts
//  loaded and unloaded as its model changes (loadAvatarScripts, unloadAvatarScripts; MyAvatar calls them), the
//  skeleton check when a model loads, with the offer to rig a local one (checkSkeleton), and the avatar override
//  from the command line (setAvatarOverrideUrl). The members as they stood in Application.cpp, in their order; a
//  home of their own until an owner takes them (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include <glm/glm.hpp>
#include <QtWidgets/QMessageBox>
#include <OffscreenUi.h>
#include <PathUtils.h>
#include <ScriptCaller.h>
#include <ScriptEngines.h>
#include "InterfaceLogging.h"
#include "avatar/AvatarManager.h"
#include "avatar/AvatarImporter.h"

using namespace std;

namespace {
// The world the avatar's scripts run as (ScriptCaller): what they make is theirs, not the world's nor the visitor's,
// and goes when the avatar changes. 0: none running.
int avatarScriptsWorld = 0;
}

// An avatar's scripts (the .fst's script lines) come with content the visitor wears, not from the visitor: they run
// under a world's rules (ScriptEngine::holdsWorldRules) as a world of their own, and only from the avatar's own
// folder (security finding #66). A script line that points elsewhere is logged and not loaded.
void Application::loadAvatarScripts(const QVector<QString>& urls) {
    auto scriptEngines = DependencyManager::get<ScriptEngines>();
    auto runningScripts = scriptEngines->getRunningScripts();
    // a path's dots are decoded before its segments are normalized, so an encoded ".." counts as one
    auto plainPath = [](const QUrl& given) {
        QUrl url = given;
        url.setPath(url.path(QUrl::FullyDecoded), QUrl::DecodedMode);
        return url.adjusted(QUrl::NormalizePathSegments);
    };
    const QUrl avatarUrl = plainPath(getMyAvatar()->getSkeletonModelURL());
    for (auto url : urls) {
        const QUrl scriptUrl = plainPath(QUrl(url));
        if (!PathUtils::isDescendantOf(scriptUrl, avatarUrl)) {
            qCWarning(interfaceapp) << "Avatar script" << url << "is outside the avatar's own folder"
                                    << PathUtils::stripFilename(avatarUrl) << "and is not loaded";
            continue;
        }
        if (runningScripts.indexOf(url) < 0) {
            if (avatarScriptsWorld == 0) {
                avatarScriptsWorld = ScriptCaller::newWorld();
            }
            scriptEngines->loadAvatarScript(url, avatarScriptsWorld);
        }
    }
}

void Application::unloadAvatarScripts() {
    if (avatarScriptsWorld != 0) {
        // before they are stopped, as when a world is left: from here on the old avatar's scripts get nothing done, and
        // what they made outside their engines (mappings, pointers, picks, 2D overlays, their entities) goes
        getEntities()->markWorldLeft(avatarScriptsWorld);
        avatarScriptsWorld = 0;
    }
    auto scriptEngines = DependencyManager::get<ScriptEngines>();
    auto urls = scriptEngines->getRunningScripts();
    for (auto url : urls) {
        auto scriptEngine = scriptEngines->getScriptEngine(url);
        if (scriptEngine->getType() == ScriptEngine::Type::AVATAR) {
            scriptEngines->stopScript(url, false);
        }
    }
}

std::shared_ptr<MyAvatar> Application::getMyAvatar() const {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

glm::vec3 Application::getAvatarPosition() const {
    return getMyAvatar()->getWorldPosition();
}

void Application::checkSkeleton() const {
    if (getMyAvatar()->getSkeletonModel()->isActive() && !getMyAvatar()->getSkeletonModel()->hasSkeleton()) {
        qCDebug(interfaceapp) << "MyAvatar model has no skeleton";

        const QUrl url = getMyAvatar()->getFullAvatarURLFromPreferences();
        QString name = getMyAvatar()->getFullAvatarModelName().trimmed();
        if (name.isEmpty()) {
            name = QFileInfo(url.path()).completeBaseName();
        }
        // A model on this computer can be rigged by the importer on the spot; .fst files name a model of their own.
        const bool canRig = url.isLocalFile() && QFileInfo(url.path()).suffix().toLower() != "fst";
        ModalDialogListener* dlg = OffscreenUi::asyncWarning((name.isEmpty() ? QString("This avatar") : name) + " has no skeleton",
                                                             "You're wearing the default avatar for now.",
                                                             canRig ? QMessageBox::Ok | QMessageBox::Cancel : QMessageBox::Ok,
                                                             QMessageBox::Ok);
        if (canRig) {
            OffscreenUi::setButtonText(dlg->getDialogItem(), QMessageBox::Ok, "Auto Rig");
            OffscreenUi::setButtonText(dlg->getDialogItem(), QMessageBox::Cancel, "OK");
            QObject::connect(dlg, &ModalDialogListener::response, qApp, [dlg, url] (QVariant answer) {
                QObject::disconnect(dlg, &ModalDialogListener::response, qApp, nullptr);
                if (static_cast<QMessageBox::StandardButton>(answer.toInt()) == QMessageBox::Ok) {
                    AvatarImporter::importFile(url.toLocalFile());
                }
            });
        }

        getMyAvatar()->useFullAvatarURL(MyAvatar::defaultAvatarURL(), DEFAULT_FULL_AVATAR_MODEL_NAME);
    } else {
        _physicsEngine->setCharacterController(getMyAvatar()->getCharacterController());
    }
}

void Application::setAvatarOverrideUrl(const QUrl& url, bool save) {
    _avatarOverrideUrl = url;
    _saveAvatarOverrideUrl = save;
}
