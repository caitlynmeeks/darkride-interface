//
//  WorldDialogs.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "WorldDialogs.h"

#include <QtQuick/QQuickItem>

#include <AddressManager.h>
#include <DependencyManager.h>
#include <DomainHandler.h>
#include <NodeList.h>
#include <OffscreenUi.h>
#include <SharedUtil.h>

#include "InterfaceLogging.h"

namespace {
// at most this many of a world's dialogs in this long: a world can't keep the visitor answering
const size_t LIMIT_COUNT = 3;
const quint64 LIMIT_USECS = 30 * USECS_PER_SECOND;
const int MAX_NAME_LENGTH = 48;
}

WorldDialogs::WorldDialogs(QObject* parent) : QObject(parent) {
    const DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, this, &WorldDialogs::leftWorld);
    connect(DependencyManager::get<AddressManager>().data(), &AddressManager::hostChanged, this, &WorldDialogs::leftWorld);
}

QString WorldDialogs::worldName() {
    auto addressManager = DependencyManager::get<AddressManager>();
    QString name = addressManager->getPlaceName().trimmed();
    if (name.isEmpty()) {
        name = addressManager->getHost();
    }
    if (name.isEmpty()) {
        name = "This world";
    }
    if (name.length() > MAX_NAME_LENGTH) {
        name = name.left(MAX_NAME_LENGTH - 1) + QChar(0x2026);
    }
    return name;
}

QVariantMap WorldDialogs::admit() {
    if (_blocked) {
        qCDebug(interfaceapp) << "World dialogs: refused, the visitor asked for no more from this world";
        return QVariantMap();
    }
    if (_open) {
        qCDebug(interfaceapp) << "World dialogs: refused, one of this world's dialogs is open";
        return QVariantMap();
    }
    const quint64 now = usecTimestampNow();
    while (!_recent.empty() && now - _recent.front() > LIMIT_USECS) {
        _recent.pop_front();
    }
    if (_recent.size() >= LIMIT_COUNT) {
        qCDebug(interfaceapp) << "World dialogs: refused, more than" << LIMIT_COUNT << "in" << LIMIT_USECS / USECS_PER_SECOND << "s";
        return QVariantMap();
    }
    _recent.push_back(now);
    ++_shown;
    return QVariantMap { { "world", worldName() }, { "offerBlock", _shown > 1 } };
}

void WorldDialogs::track(QQuickItem* dialog) {
    _open = dialog;
    if (!dialog) {
        return;
    }
    // a message box answers with selected(int), a prompt with selected(var) or canceled(); each destroys itself after
    connect(dialog, SIGNAL(selected(int)), this, SLOT(answered()), Qt::DirectConnection);
    connect(dialog, SIGNAL(selected(QVariant)), this, SLOT(answered()), Qt::DirectConnection);
    connect(dialog, SIGNAL(canceled()), this, SLOT(answered()), Qt::DirectConnection);
}

void WorldDialogs::answered() {
    auto dialog = qobject_cast<QQuickItem*>(sender());
    if (dialog && dialog->property("blockWorld").toBool() && !_blocked) {
        _blocked = true;
        qCDebug(interfaceapp) << "World dialogs: the visitor asked for no more from this world, until leaving it";
    }
}

void WorldDialogs::leftWorld() {
    // a dialog of the world left behind is answered Cancel (its script gets the answer, if it's still running)
    if (QQuickItem* dialog = _open.data()) {
        _open.clear();
        if (dialog->metaObject()->indexOfMethod("cancel()") >= 0) {
            QMetaObject::invokeMethod(dialog, "cancel");
        } else if (dialog->metaObject()->indexOfMethod("dismiss()") >= 0) {
            QMetaObject::invokeMethod(dialog, "dismiss");
        }
    }
    _recent.clear();
    _shown = 0;
    _blocked = false;
}

void WorldDialogs::alert(const QString& text) {
    const QVariantMap properties = admit();
    if (properties.isEmpty()) {
        return;
    }
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQuickItem* box = offscreenUi->createMessageBox(OffscreenUi::ICON_WARNING, properties["world"].toString() + " asks",
                                                    text, QMessageBox::Ok, QMessageBox::Ok, properties);
    track(box);
    // nobody waits for an alert's answer: the box destroys itself when it's answered
}

bool WorldDialogs::confirm(const QString& text) {
    const QVariantMap properties = admit();
    if (properties.isEmpty()) {
        return false;
    }
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQuickItem* box = offscreenUi->createMessageBox(OffscreenUi::ICON_QUESTION, properties["world"].toString() + " asks",
                                                    text, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes, properties);
    track(box);
    return offscreenUi->waitForMessageBoxResult(box) == QMessageBox::Yes;
}

QVariant WorldDialogs::prompt(const QString& text, const QString& defaultText, const QVariantMap& properties) {
    QVariantMap world = admit();
    if (world.isEmpty()) {
        return QVariant();
    }
    for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
        world.insert(it.key(), it.value());
    }
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQuickItem* box = offscreenUi->createInputDialog(OffscreenUi::ICON_NONE, world["world"].toString() + " asks", text,
                                                     defaultText, world);
    track(box);
    return offscreenUi->waitForInputDialogResult(box);
}

ModalDialogListener* WorldDialogs::promptAsync(const QString& text, const QString& defaultText,
                                               const QVariantMap& properties) {
    QVariantMap world = admit();
    if (world.isEmpty()) {
        return nullptr;
    }
    for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
        world.insert(it.key(), it.value());
    }
    ModalDialogListener* listener = DependencyManager::get<OffscreenUi>()->inputDialogAsync(
        OffscreenUi::ICON_NONE, world["world"].toString() + " asks", text, defaultText, world);
    track(listener ? listener->getDialogItem() : nullptr);
    return listener;
}
