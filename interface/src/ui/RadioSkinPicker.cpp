//
//  RadioSkinPicker.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "RadioSkinPicker.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QFileDialog>

#include "InterfaceLogging.h"

namespace {
    // as the radio's page takes them (html/radio/radio.js SKIN_MAX_BYTES; classic skins are 20-500 KB)
    const qint64 SKIN_MAX_BYTES = 4 * 1024 * 1024;
}

void RadioSkinPicker::pick() {
    if (_dialog) {
        _dialog->raise();
        _dialog->activateWindow();
        return;
    }
    // as the app's other desktop file dialogs (OffscreenUi::fileDialogAsync): a window of its own, answered later
    auto dialog = new QFileDialog(nullptr, tr("Load a skin"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), tr("Classic skins (*.wsz *.zip)"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    _dialog = dialog;
    connect(dialog, &QFileDialog::fileSelected, this, [this, dialog](const QString& path) {
        dialog->deleteLater();
        const QFileInfo info(path);
        QFile file(path);
        if (!info.isFile() || info.size() <= 0) {
            emit picked(info.fileName(), QString(), "That skin is empty.");
            return;
        }
        if (info.size() > SKIN_MAX_BYTES) {
            emit picked(info.fileName(), QString(), "That skin is too big (the radio takes skins up to 4 MB).");
            return;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            emit picked(info.fileName(), QString(), "That skin couldn't be read.");
            return;
        }
        const QByteArray bytes = file.read(SKIN_MAX_BYTES + 1);
        if (bytes.size() > SKIN_MAX_BYTES) {
            emit picked(info.fileName(), QString(), "That skin is too big (the radio takes skins up to 4 MB).");
            return;
        }
        qCDebug(interfaceapp) << "Radio: a skin picked," << bytes.size() << "bytes";
        emit picked(info.fileName(), QString::fromLatin1(bytes.toBase64()), QString());
    });
    connect(dialog, &QFileDialog::rejected, this, [this, dialog] {
        dialog->deleteLater();
        emit picked(QString(), QString(), QString());
    });
    dialog->show();
}
