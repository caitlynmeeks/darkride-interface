//
//  UserFiles.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "UserFiles.h"

#include <algorithm>

#include <QtCore/QElapsedTimer>
#include <QtCore/QCollator>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <AccountManager.h>
#include <DependencyManager.h>
#include <NetworkAccessManager.h>

#include "../../InterfaceLogging.h"

namespace {
    const QString FILES_API_PATH = "/api/user/files";
    const QStringList PICTURE_SUFFIXES { "png", "jpg", "jpeg", "webp", "bmp", "gif", "tga", "tif", "tiff", "ktx", "exr", "hdr" };
    const QStringList MODEL_SUFFIXES { "fbx", "obj", "gltf", "glb", "vrm", "dae", "3ds", "stl", "ply", "usdz", "usd", "usda" };
    const QStringList SOUND_SUFFIXES { "wav", "mp3", "ogg", "flac", "aac", "m4a" };
    const QStringList SCRIPT_SUFFIXES { "js", "mjs", "qml", "wasm" };

    // a path as the files host wants it in a URL: each segment encoded, the slashes kept
    QString encodedPath(const QString& path) {
        QStringList segments = path.split('/');
        for (auto& segment : segments) {
            segment = QString::fromLatin1(QUrl::toPercentEncoding(segment));
        }
        return segments.join('/');
    }
}

// --- UserFile -----------------------------------------------------------------------------------

UserFile::Kind UserFile::kindOf(const QString& name, bool folder) {
    if (folder) {
        return Kind::Folder;
    }
    QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix == QStringLiteral("fst")) {
        return Kind::Avatar;
    }
    if (PICTURE_SUFFIXES.contains(suffix)) {
        return Kind::Picture;
    }
    if (MODEL_SUFFIXES.contains(suffix)) {
        return Kind::Model;
    }
    if (SOUND_SUFFIXES.contains(suffix)) {
        return Kind::Sound;
    }
    if (SCRIPT_SUFFIXES.contains(suffix)) {
        return Kind::Script;
    }
    return Kind::Other;
}

QString UserFile::kindName(Kind kind) {
    switch (kind) {
        case Kind::Folder: return "Folder";
        case Kind::Picture: return "Image";
        case Kind::Model: return "Model";
        case Kind::Avatar: return "Avatar";
        case Kind::Sound: return "Sound";
        case Kind::Script: return "Script";
        default: return "File";
    }
}

QString UserFile::folderOf(const QString& path) {
    if (path.isEmpty() || path == QStringLiteral("/")) {
        return QString();
    }
    int end = path.endsWith('/') ? path.length() - 2 : path.length() - 1;
    int slash = path.lastIndexOf('/', end);
    return slash < 0 ? QStringLiteral("/") : path.left(slash + 1);
}

QString UserFile::nameOf(const QString& path) {
    QString trimmed = path.endsWith('/') ? path.chopped(1) : path;
    return trimmed.mid(trimmed.lastIndexOf('/') + 1);
}

QString UserFile::sizeText(qint64 bytes) {
    const double KB = 1024.0;
    if (bytes < 1000) {
        return QString("%1 B").arg(bytes);
    }
    if (bytes < 1000 * KB) {
        return QString("%1 KB").arg(bytes / KB, 0, 'f', bytes < 10 * KB ? 1 : 0);
    }
    if (bytes < 1000 * KB * KB) {
        return QString("%1 MB").arg(bytes / (KB * KB), 0, 'f', bytes < 10 * KB * KB ? 1 : 0);
    }
    return QString("%1 GB").arg(bytes / (KB * KB * KB), 0, 'f', 2);
}

// --- UserFiles ----------------------------------------------------------------------------------

UserFiles::UserFiles(QObject* parent) : QObject(parent) {
    auto accountManager = DependencyManager::get<AccountManager>();
    connect(accountManager.data(), &AccountManager::logoutComplete, this, &UserFiles::clear);
}

bool UserFiles::isLoggedIn() {
    return DependencyManager::get<AccountManager>()->isLoggedIn();
}

const UserFile* UserFiles::find(const QString& path) const {
    auto found = _byPath.find(path);
    return found == _byPath.end() ? nullptr : &_files[*found];
}

QString UserFiles::withStableLinks(const QString& text, int& rewritten, QStringList& missing) const {
    if (_baseUrl.isEmpty() || !text.contains(QStringLiteral("://"))) {
        return text;
    }
    const QUrl base(_baseUrl);
    const QString host = base.host().toLower();
    QString own = base.path(QUrl::FullyDecoded);   // "/<username>/": the user's files on the host
    if (!own.endsWith('/')) {
        own += '/';
    }
    // a link runs to the first space, quote or angle bracket: as links sit in JSON, in scripts and in plain text
    static const QRegularExpression LINK(QStringLiteral("https?://[^\\s\"'<>\\\\]+"), QRegularExpression::CaseInsensitiveOption);
    static const QString TRAILING = QStringLiteral(".,;:!?)]}");
    QString result;
    int done = 0;   // the text up to here is in result
    auto matches = LINK.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        QString link = match.captured(0);
        const UserFile* file = nullptr;
        QUrl url;
        bool ours = false;
        // punctuation after a link in a sentence is not part of it: tried without it when the link names nothing
        while (true) {
            url = QUrl(link, QUrl::TolerantMode);
            const QString path = url.path(QUrl::FullyDecoded);
            ours = url.isValid() && url.host().toLower() == host && path.startsWith(own, Qt::CaseInsensitive);
            if (ours) {
                file = find(path.mid(own.length() - 1));   // "/images/a.png"
            }
            if ((ours && file) || link.isEmpty() || !TRAILING.contains(link.back())) {
                break;
            }
            link.chop(1);
        }
        if (!ours || (file && file->folder)) {
            continue;   // another host, another user's files, a stable link already (<files host>/f/<id>/...), a folder
        }
        if (!file || file->url.isEmpty()) {
            if (!missing.contains(link)) {
                missing << link;
            }
            continue;
        }
        QString stable = file->url;
        if (url.hasQuery()) {
            stable += '?' + url.query(QUrl::FullyEncoded);
        }
        if (url.hasFragment()) {
            stable += '#' + url.fragment(QUrl::FullyEncoded);
        }
        result += text.mid(done, match.capturedStart(0) - done) + stable;
        done = match.capturedStart(0) + link.length();
        ++rewritten;
    }
    if (done == 0) {
        return text;
    }
    return result + text.mid(done);
}

QVector<const UserFile*> UserFiles::childrenOf(const QString& folderPath) const {
    QVector<const UserFile*> children;
    const QVector<int> indexes = _childrenByFolder.value(folderPath);
    children.reserve(indexes.size());
    for (int index : indexes) {
        children << &_files[index];
    }
    return children;
}

QString UserFiles::freeName(const QString& folderPath, const QString& wanted) const {
    QSet<QString> taken;
    for (const auto& file : _files) {
        if (UserFile::folderOf(file.path) == folderPath) {
            taken.insert(file.name.toLower());
        }
    }
    if (!taken.contains(wanted.toLower())) {
        return wanted;
    }
    for (int i = 2;; ++i) {
        QString candidate = QString("%1 %2").arg(wanted).arg(i);
        if (!taken.contains(candidate.toLower())) {
            return candidate;
        }
    }
}

void UserFiles::setLoading(bool loading) {
    if (_loading != loading) {
        _loading = loading;
        emit loadingChanged(loading);
    }
}

void UserFiles::clear() {
    _files.clear();
    _byPath.clear();
    _countUnder.clear();
    _childrenByFolder.clear();
    _listedAt.invalidate();
    _baseUrl.clear();
    _listed = false;
    _usedSize = 0;
    _maxSize = 0;
    emit listed();
}

QNetworkReply* UserFiles::send(const QString& route, QNetworkAccessManager::Operation operation,
                               const QList<QPair<QString, QString>>& query) {
    auto accountManager = DependencyManager::get<AccountManager>();
    QNetworkRequest request = accountManager->createRequest(FILES_API_PATH + route, AccountManagerAuth::Required);
    if (request.url().isEmpty()) {
        return nullptr;   // not logged in
    }
    if (!query.isEmpty()) {
        // every value fully encoded here: a name with a + or an & in it would otherwise read as something else
        QStringList parts;
        for (const auto& item : query) {
            parts << QString::fromLatin1(QUrl::toPercentEncoding(item.first)) + "="
                + QString::fromLatin1(QUrl::toPercentEncoding(item.second));
        }
        QUrl url = request.url();
        url.setQuery(parts.join('&'), QUrl::StrictMode);
        request.setUrl(url);
    }
    auto& network = NetworkAccessManager::getInstance();
    switch (operation) {
        case QNetworkAccessManager::PostOperation:
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            return network.post(request, QByteArray());
        case QNetworkAccessManager::PutOperation:
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            return network.put(request, QByteArray());
        case QNetworkAccessManager::DeleteOperation:
            return network.deleteResource(request);
        default:
            return network.get(request);
    }
}

QString UserFiles::errorText(QNetworkReply* reply) {
    QString detail = QJsonDocument::fromJson(reply->readAll()).object().value("message").toString();
    return detail.isEmpty() ? reply->errorString() : detail;
}

void UserFiles::refreshIfStale(qint64 maxAgeMs) {
    if (_listed && _listedAt.isValid() && _listedAt.elapsed() < maxAgeMs && isLoggedIn()) {
        return;
    }
    refresh();
}

void UserFiles::refresh() {
    if (!isLoggedIn()) {
        clear();
        return;
    }
    if (_loading) {
        _refreshWanted = true;   // the listing on its way may miss the latest change: one more after it
        return;
    }
    QNetworkReply* reply = send(QString(), QNetworkAccessManager::GetOperation);
    if (!reply) {
        clear();
        return;
    }
    setLoading(true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        onListing(reply);
    });
    // the quota, beside it
    if (QNetworkReply* status = send("/status", QNetworkAccessManager::GetOperation)) {
        connect(status, &QNetworkReply::finished, this, [this, status] {
            status->deleteLater();
            if (status->error() == QNetworkReply::NoError) {
                QJsonObject quota = QJsonDocument::fromJson(status->readAll()).object();
                _usedSize = (qint64)quota.value("usedSize").toDouble();
                _maxSize = (qint64)quota.value("maxSize").toDouble();
                emit quotaChanged();
            }
        });
    }
}

void UserFiles::onListing(QNetworkReply* reply) {
    setLoading(false);
    if (reply->error() != QNetworkReply::NoError) {
        QString why = errorText(reply);
        qCWarning(interfaceapp) << "My Files listing failed:" << why;
        emit failed("Could not list your files: " + why);
        emit listingFailed(why);
        return;
    }
    QElapsedTimer listingTimer;
    listingTimer.start();
    QJsonObject listing = QJsonDocument::fromJson(reply->readAll()).object();
    _baseUrl = listing.value("url").toString();
    QVector<UserFile> files;
    QHash<QString, int> byPath;
    auto add = [&](UserFile file) {
        if (byPath.contains(file.path)) {
            return;
        }
        byPath.insert(file.path, files.size());
        files << file;
    };
    for (const auto& value : listing.value("files").toArray()) {
        QJsonObject record = value.toObject();
        UserFile file;
        file.path = record.value("key").toString();
        if (!file.path.startsWith('/') || file.path == QStringLiteral("/")) {
            continue;
        }
        file.folder = file.path.endsWith('/');
        file.name = UserFile::nameOf(file.path);
        file.size = (qint64)record.value("size").toDouble();
        file.updated = QDateTime::fromString(record.value("updated").toString(), Qt::ISODateWithMs).toLocalTime();
        file.isPrivate = record.value("tea").toBool();
        file.id = record.value("id").toString();
        file.url = record.value("url").toString();
        file.pathUrl = _baseUrl + encodedPath(file.path);
        file.kind = UserFile::kindOf(file.name, file.folder);
        add(file);
    }
    // a folder the listing does not name (files put there by other tools) is there all the same
    int named = files.size();
    for (int i = 0; i < named; ++i) {
        for (QString folder = UserFile::folderOf(files[i].path); folder.length() > 1; folder = UserFile::folderOf(folder)) {
            if (byPath.contains(folder)) {
                break;
            }
            UserFile implied;
            implied.path = folder;
            implied.folder = true;
            implied.name = UserFile::nameOf(folder);
            implied.pathUrl = _baseUrl + encodedPath(folder);
            implied.kind = UserFile::Kind::Folder;
            add(implied);
        }
    }
    // how many files each folder holds, counted once here: every file counts for each folder above it
    QHash<QString, int> countUnder;
    for (const auto& file : files) {
        if (!file.folder) {
            for (QString folder = UserFile::folderOf(file.path); !folder.isEmpty(); folder = UserFile::folderOf(folder)) {
                ++countUnder[folder];
            }
        }
    }
    // Each folder's entries, once, sorted as the panel shows them (folders first, then by name, numbers in order).
    // childrenOf used to scan every entry, and the folder tree asks it once a folder: with 18,746 entries that was
    // over a second of the main thread at each listing, which build mode asks for each time it comes on (10-24:
    // build mode toggles late, a frame held 1.6 s).
    QHash<QString, QVector<int>> childrenByFolder;
    for (int i = 0; i < files.size(); ++i) {
        childrenByFolder[UserFile::folderOf(files[i].path)] << i;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    for (auto it = childrenByFolder.begin(); it != childrenByFolder.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [&files, &collator](int a, int b) {
            if (files[a].folder != files[b].folder) {
                return files[a].folder;
            }
            return collator.compare(files[a].name, files[b].name) < 0;
        });
    }
    _files = files;
    _byPath = byPath;
    _countUnder = countUnder;
    _childrenByFolder = childrenByFolder;
    _listed = true;
    _listedAt.start();
    qCDebug(interfaceapp) << "My Files listed:" << _files.size() << "entries in" << listingTimer.elapsed() << "ms";
    emit listed();
    if (_refreshWanted) {
        _refreshWanted = false;
        refresh();
    }
}

void UserFiles::run(const QString& route, QNetworkAccessManager::Operation operation,
                    const QList<QPair<QString, QString>>& query, const QString& what) {
    QNetworkReply* reply = send(route, operation, query);
    if (!reply) {
        emit failed("Could not " + what + ": you are not logged in");
        return;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, what] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString why = errorText(reply);
            qCWarning(interfaceapp) << "My Files:" << what << "failed:" << reply->url().toString() << why;
            emit failed("Could not " + what + ": " + why);
            refresh();   // the views go back to what the server has
            return;
        }
        emit changed(what);
        refresh();
    });
}

void UserFiles::createFolder(const QString& folderPath) {
    run("/folder", QNetworkAccessManager::PutOperation, { { "path", folderPath } },
        "make the folder " + UserFile::nameOf(folderPath));
}

void UserFiles::move(const QString& path, const QString& newPath) {
    if (path == newPath) {
        return;
    }
    bool folder = path.endsWith('/');
    QString what = UserFile::folderOf(path) == UserFile::folderOf(newPath)
        ? "rename " + UserFile::nameOf(path)
        : "move " + UserFile::nameOf(path);
    run(folder ? "/folder/move" : "/move", QNetworkAccessManager::PostOperation,
        { { "oldPath", path }, { "newPath", newPath } }, what);
}

void UserFiles::remove(const QString& path) {
    bool folder = path.endsWith('/');
    run(folder ? "/folder" : QString(), QNetworkAccessManager::DeleteOperation, { { "path", path } },
        "delete " + UserFile::nameOf(path));
}

void UserFiles::togglePrivate(const QString& path) {
    bool folder = path.endsWith('/');
    const UserFile* file = find(path);
    bool makingPrivate = file && !file->isPrivate;
    run(folder ? "/folder/tea-only" : "/tea-only", QNetworkAccessManager::PostOperation, { { "path", path } },
        (makingPrivate ? "make private " : "make public ") + UserFile::nameOf(path));
}
