//
//  SavedScriptsReview.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "SavedScriptsReview.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtQml/QQmlContext>

#include <DependencyManager.h>
#include <OffscreenUi.h>
#include <ScriptEngines.h>

#include "InterfaceLogging.h"

namespace {
    const QString REVIEW_QML = "dialogs/SavedScriptsReview.qml";
    const int DESKTOP_WAIT_MS = 250;

    // what an address is, as its main line: the site as a browser reads it (QUrl drops anything before an '@') and the
    // file's name; for this computer's files, the folder and the file. Never the address's own start, which whoever
    // planted it chose ("https://files.vr.darkride.ai/x@evil.example/p.js" is evil.example's: security, 11-01)
    QString shownAs(const QString& address) {
        const QUrl url(address);
        const QString file = QFileInfo(url.path()).fileName();
        if (url.isLocalFile()) {
            const QFileInfo info(url.toLocalFile());
            return info.dir().dirName() + "/" + info.fileName();
        }
        const QString host = url.host();
        if (!host.isEmpty()) {
            return file.isEmpty() ? host : host + " \u2014 " + file;
        }
        return url.scheme().isEmpty() ? address : url.scheme() + ": " + file;
    }
}

void SavedScriptsReview::showPending() {
    // on a later turn of the main thread; ScriptEngines holds the scripts meanwhile
    QMetaObject::invokeMethod(qApp, [] {
        if (auto scriptEngines = DependencyManager::get<ScriptEngines>()) {
            show(scriptEngines->savedScriptsAwaitingReview());
        }
    }, Qt::QueuedConnection);
}

void SavedScriptsReview::show(const QStringList& scripts) {
    if (scripts.isEmpty()) {
        return;
    }
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    if (!offscreenUi || !offscreenUi->getDesktop()) {
        // the app's own windows aren't up yet: asked again shortly
        QTimer::singleShot(DESKTOP_WAIT_MS, qApp, [scripts] { show(scripts); });
        return;
    }
    qCDebug(interfaceapp) << "Saved scripts to review:" << scripts.size();
    offscreenUi->load(REVIEW_QML, [scripts](QQmlContext* context, QObject* dialog) {
        // a name in the dialog's context, which its scripts property reads as it's made (a property set here is
        // overwritten when the dialog finishes being made)
        QVariantList addresses;
        for (const QString& script : scripts) {
            addresses << QVariantMap { { "address", script }, { "main", shownAs(script) }, { "full", script } };
        }
        context->setContextProperty("savedScriptsHeld", addresses);
        // the answer's receiver lives as long as the dialog (Ask Me Later just closes it: the scripts stay held)
        auto receiver = new SavedScriptsReview(dialog);
        QObject::connect(dialog, SIGNAL(answered(QVariant)), receiver, SLOT(answered(QVariant)));
    });
}

void SavedScriptsReview::answered(const QVariant& kept) {
    const QStringList addresses = kept.toStringList();
    qCDebug(interfaceapp) << "Saved scripts reviewed:" << addresses.size() << "kept";
    if (auto scriptEngines = DependencyManager::get<ScriptEngines>()) {
        scriptEngines->finishSavedScriptsReview(addresses);
    }
}
