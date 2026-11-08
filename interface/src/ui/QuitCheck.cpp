//
//  QuitCheck.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "QuitCheck.h"

#include <memory>

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#ifndef QT_NO_SESSIONMANAGER
#include <QtGui/QSessionManager>
#endif

#include <DependencyManager.h>
#include <MainWindow.h>

#include "Application.h"
#include "CodeEditor.h"
#include "InterfaceLogging.h"
#include "Popup.h"
#include "editor/Editor.h"

namespace quitcheck {

namespace {

const int TERRAIN_WAIT_MS = 15000;      // the longest a quit waits for the terrain's upload
const int TERRAIN_NOTICE_AFTER_MS = 400;  // a quick save shows no notice
const int SESSION_END_MS = 30000;       // a session's end that didn't come (another app said no) is forgotten

bool goingAheadNow { false };   // this quit was answered yes: the windows Qt closes next don't ask
bool asking { false };          // a check is waiting for the terrain or for the person's answer
bool sessionEnding { false };   // Windows, Linux: the session is ending (commitDataRequest)

bool systemEnding() {
    if (sessionEnding) {
        return true;
    }
#if defined(Q_OS_MACOS)
    bool fromSystem = false;
    return mac::isQuitEvent(fromSystem) && fromSystem;
#else
    return false;
#endif
}

// The terrain sculpting still waiting to be saved goes up now, and the quit waits for it: "" when it's saved (or
// there was nothing to save), else why not.
QString saveTerrain() {
    if (!DependencyManager::isSet<Editor>()) {
        return QString();
    }
    EditorTerrain& terrain = DependencyManager::get<Editor>()->terrain();
    if (!terrain.hasPendingSaves()) {
        return QString();
    }
    const int failuresBefore = terrain.saveFailures();
    terrain.flushSaves();
    qCInfo(interfaceapp) << "QuitCheck: saving the terrain before quitting";

    QElapsedTimer waited;
    waited.start();
    std::unique_ptr<popup::Notice> notice;
    QEventLoop loop;
    QTimer tick;
    tick.setInterval(50);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (!terrain.hasPendingSaves()) {
            loop.quit();
            return;
        }
        if (!terrain.isUploading()) {
            if (terrain.saveFailures() > failuresBefore) {
                loop.quit();   // it went up and didn't take: asking beats trying again for ever
                return;
            }
            terrain.flushSaves();   // changed again while it went up: now, not after the usual pause
        }
        if (!notice && waited.elapsed() > TERRAIN_NOTICE_AFTER_MS) {
            notice.reset(new popup::Notice(popup::appIcon(), QString("Saving your terrain") + QChar(0x2026),
                                           "Darkride quits when it's saved.", qApp->getWindow()));
            notice->show();
        }
        if (waited.elapsed() > TERRAIN_WAIT_MS) {
            loop.quit();
        }
    });
    tick.start();
    // the person's clicks and keys wait: nothing changes while the last of it goes up
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    tick.stop();
    notice.reset();

    if (!terrain.hasPendingSaves()) {
        qCInfo(interfaceapp) << "QuitCheck: the terrain is saved (" << waited.elapsed() << "ms)";
        return QString();
    }
    QString reason = terrain.saveFailures() > failuresBefore ? terrain.lastSaveError() : QString();
    if (reason.isEmpty()) {
        reason = "it took too long";
    }
    while (reason.endsWith('.')) {
        reason.chop(1);
    }
    qCWarning(interfaceapp) << "QuitCheck: the terrain isn't saved:" << reason;
    return reason;
}

// mary's words (11-08 16:41): one file named, several counted, as macOS does
bool ask(const QStringList& scripts, const QString& terrainReason) {
    const int count = scripts.size();
    const QString scriptsText = count == 1 && !scripts.first().isEmpty() ? scripts.first()
                                                                         : QString::number(count) + " scripts";
    QString title;
    QString text;
    if (terrainReason.isEmpty()) {
        title = "Quit without saving " + scriptsText + "?";
        text = count == 1 ? "Your changes to it will be lost." : "Your changes to them will be lost.";
    } else if (count == 0) {
        title = "Quit without saving your terrain?";
        text = "It couldn't be saved: " + terrainReason + ".";
    } else {
        title = "Quit without saving your terrain and " + scriptsText + "?";
        text = "Your terrain couldn't be saved: " + terrainReason + ". All these changes will be lost.";
    }
    qCInfo(interfaceapp) << "QuitCheck: asking:" << title << "| scripts" << scripts << "| terrain"
                         << (terrainReason.isEmpty() ? QString("saved") : terrainReason);
    popup::Alert alert(popup::appIcon(), title, text, QString(),
                       { { "Cancel", popup::Role::Cancel }, { "Discard Changes", popup::Role::Destructive } },
                       qApp->getWindow());
    alert.exec();
    const bool discard = alert.answer() == 1;
    qCInfo(interfaceapp) << "QuitCheck: answered" << (discard ? "Discard Changes" : "Cancel");
    return discard;
}

}

void install() {
    if (auto window = qApp->getWindow()) {
        window->setCloseCheck([] { return mayQuit(); });
    }
#ifndef QT_NO_SESSIONMANAGER
    // the session ends (Windows, Linux): the quit that follows doesn't ask; if it doesn't come, it's forgotten
    QObject::connect(qApp, &QGuiApplication::commitDataRequest, qApp, [](QSessionManager&) {
        sessionEnding = true;
        QTimer::singleShot(SESSION_END_MS, qApp, [] { sessionEnding = false; });
    });
#endif
}

bool mayQuit() {
    if (goingAheadNow || qApp->isAboutToQuit()) {
        return true;
    }
    if (systemEnding()) {
        qCInfo(interfaceapp) << "QuitCheck: the system is ending the session: no question";
        goingAheadNow = true;
        return true;
    }
    if (asking) {
        return false;   // a second quit while the first one asks: the first one's answer stands
    }
    asking = true;
    const QString terrainReason = saveTerrain();
    const QStringList scripts =
        DependencyManager::isSet<CodeEditor>() ? DependencyManager::get<CodeEditor>()->unsavedNames() : QStringList();
    const bool yes = (scripts.isEmpty() && terrainReason.isEmpty()) || ask(scripts, terrainReason);
    asking = false;
    if (yes) {
        goingAheadNow = true;
    }
    return yes;
}

void askThenQuit() {
    if (mayQuit()) {
        qApp->quit();
    }
}

bool isOutsideRequest() {
#if defined(Q_OS_MACOS)
    bool fromSystem = false;
    return mac::isQuitEvent(fromSystem) && !fromSystem;
#else
    return false;
#endif
}

void goingAhead() {
    goingAheadNow = true;
}

}
