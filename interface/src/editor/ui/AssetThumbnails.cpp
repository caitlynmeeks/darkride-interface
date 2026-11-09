//
//  AssetThumbnails.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetThumbnails.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QImageReader>

#include <DependencyManager.h>
#include <PathUtils.h>
#include <ResourceManager.h>
#include <ResourceRequest.h>

#include "../../InterfaceLogging.h"

namespace {
    const int FETCHES_AT_ONCE = 4;
}

AssetThumbnails::AssetThumbnails(QObject* parent) : QObject(parent) {
    _dir = PathUtils::getAppLocalDataPath() + "editor-thumbnails/";
    QDir().mkpath(_dir);
}

QString AssetThumbnails::keyOf(const UserFile& file) {
    return file.id + "-" + QString::number(file.updated.toSecsSinceEpoch());
}

QString AssetThumbnails::cachePath(const UserFile& file) const {
    return _dir + keyOf(file) + ".png";
}

QPixmap AssetThumbnails::get(const UserFile& file) const {
    return _memory.value(keyOf(file));
}

bool AssetThumbnails::isMissing(const UserFile& file) const {
    return _missing.contains(keyOf(file));
}

void AssetThumbnails::want(const QVector<UserFile>& files) {
    _queue.clear();
    for (const auto& file : files) {
        if (file.kind != UserFile::Kind::Picture || file.id.isEmpty() || file.url.isEmpty()) {
            continue;
        }
        QString key = keyOf(file);
        if (_memory.contains(key) || _missing.contains(key) || _inFlight.contains(key)) {
            continue;
        }
        // on disk from an earlier run: read now, no fetch
        QPixmap onDisk(cachePath(file));
        if (!onDisk.isNull()) {
            _memory.insert(key, onDisk);
            emit ready(file.id);
            continue;
        }
        _queue << file;
    }
    startNext();
}

void AssetThumbnails::startNext() {
    while (_inFlight.size() < FETCHES_AT_ONCE && !_queue.isEmpty()) {
        UserFile file = _queue.takeFirst();
        QString key = keyOf(file);
        _inFlight.insert(key);
        // through the app's own file requests (HTTPResourceRequest): a private file in the person's My Files, which our
        // files host refuses (403) without their login, is asked once more with it, at that host only, as every other
        // file the app fetches; a picture past the size limit for pictures is refused there too
        ResourceRequest* request = DependencyManager::get<ResourceManager>()->createResourceRequest(this, QUrl(file.url));
        if (!request) {
            _inFlight.remove(key);
            _missing.insert(key);
            emit ready(file.id);
            continue;
        }
        connect(request, &ResourceRequest::finished, this, [this, request, file] {
            request->deleteLater();
            QPixmap pixmap;
            if (request->getResult() == ResourceRequest::Success) {
                QByteArray bytes = request->getData();
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                // decoded small where the format allows it (JPEG), else decoded whole and scaled
                QSize size = reader.size();
                if (size.isValid() && (size.width() > PIXELS || size.height() > PIXELS)) {
                    reader.setScaledSize(size.scaled(PIXELS, PIXELS, Qt::KeepAspectRatio));
                }
                QImage image = reader.read();
                if (!image.isNull()) {
                    if (image.width() > PIXELS || image.height() > PIXELS) {
                        image = image.scaled(PIXELS, PIXELS, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    pixmap = QPixmap::fromImage(image);
                }
            } else {
                qCDebug(interfaceapp) << "thumbnail not fetched" << file.path << request->getResultString();
            }
            finish(file, pixmap);
        });
        request->send();
    }
}

void AssetThumbnails::finish(const UserFile& file, const QPixmap& pixmap) {
    QString key = keyOf(file);
    _inFlight.remove(key);
    if (pixmap.isNull()) {
        _missing.insert(key);
    } else {
        _memory.insert(key, pixmap);
        // the file's earlier versions go, this one stays for the next run
        for (const auto& old : QDir(_dir).entryList({ file.id + "-*.png" }, QDir::Files)) {
            QFile::remove(_dir + old);
        }
        pixmap.save(cachePath(file), "PNG");
    }
    emit ready(file.id);
    startNext();
}
