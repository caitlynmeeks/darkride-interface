//
//  CodeNearby.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "CodeNearby.h"

#include <algorithm>
#include <functional>

#include <QtCore/QThreadPool>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <EntityScriptRights.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <EntityTreeRenderer.h>
#include <MaterialEntityItem.h>
#include <PickFilter.h>
#include <ScriptCallback.h>
#include <procedural/Procedural.h>

#include "Application.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"

namespace {

const float RADIUS_M = 50.0f;
const int MAX_ROWS = 50;
const int MAX_ASKED = 20;      // the running status of the nearest entity scripts, from this client's own engine
const int TICK_MS = 1000;        // the main thread's check: where the avatar is, and how long since the last scan
const float MOVE_M = 5.0f;        // a scan after the avatar has moved this far ...
const qint64 MIN_WAIT_MS = 5000;  // ... but no sooner than this after the last
const qint64 MAX_WAIT_MS = 10000; // and one this long after the last, moved or not

// queryPropertyMetadata's answer on the main thread (as CodeEditor's and the Inspector's)
class NearbyTarget : public CallbackTarget {
public:
    explicit NearbyTarget(std::function<void(int32_t, const QVariantList&)> onAnswer) : _onAnswer(std::move(onAnswer)) {}
    bool invoke(int32_t callback, const QVariantList& arguments, QVariant&, QString&) override {
        post(callback, arguments);
        return true;
    }
    void post(int32_t callback, const QVariantList& arguments) override {
        auto onAnswer = _onAnswer;
        QMetaObject::invokeMethod(qApp, [onAnswer, callback, arguments] { onAnswer(callback, arguments); },
            Qt::QueuedConnection);
    }
    void request(int32_t callback, const QVariantList& arguments, std::function<void(const QVariant&)>) override {
        post(callback, arguments);
    }

private:
    std::function<void(int32_t, const QVariantList&)> _onAnswer;
};

bool isAddress(const QString& value) {
    const QString trimmed = value.trimmed();
    return !trimmed.isEmpty() && !trimmed.contains('\n') && !QUrl(trimmed).scheme().isEmpty();
}

QString fileOf(const QString& address) {
    const QUrl url(address.trimmed());
    return url.fileName().isEmpty() ? url.host() : url.fileName();
}

bool isNetwork(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return scheme == "http" || scheme == "https" || scheme == "atp";
}

QString kindOf(const QString& which) {
    return which == "serverScripts" ? "Server script" : which == "fragmentShader" ? "Fragment shader"
        : which == "vertexShader" ? "Vertex shader" : "Entity script";
}

QString kindKeyOf(const QString& which) {
    return which == "serverScripts" ? "server" : which == "fragmentShader" ? "fragment"
        : which == "vertexShader" ? "vertex" : "entity";
}

QString distanceText(float distance) {
    return QString::number(distance, 'f', distance < 10.0f ? 1 : 0) + " m";
}

QString usedByText(const QString& name, int users) {
    const QString shown = name.isEmpty() ? QString("something unnamed") : name;
    return users > 1 ? shown + ", and " + QString::number(users - 1) + " more" : shown;
}

}

QString CodeNearby::keyFor(const QString& which, const QString& value, const QUuid& entity) {
    const QString kind = kindKeyOf(which);
    if ((which == "script" || which == "serverScripts") && !isAddress(value)) {
        return kind + "|written:" + entity.toString();
    }
    return kind + "|" + value.trimmed();
}

QString CodeNearby::pathOf(const QString& address) {
    const QUrl url(address.trimmed());
    const QString scheme = url.scheme().toLower();
    if (scheme == "atp") {
        return url.path();
    }
    if (scheme == "http" || scheme == "https") {
        return url.host() + (url.port() != -1 ? ":" + QString::number(url.port()) : QString()) + url.path();
    }
    return url.toDisplayString();
}

QVector<QPair<QString, QUrl>> CodeNearby::shadersOf(bool isMaterial, const QString& userData, const QString& materialURL,
                                                    const QString& materialData) {
    QVector<QPair<QString, QUrl>> found;
    auto add = [&found](const ProceduralData& data) {
        if (!data.fragmentShaderUrl.isEmpty() && isNetwork(data.fragmentShaderUrl)) {
            found.append({ QStringLiteral("fragmentShader"), data.fragmentShaderUrl });
        }
        if (!data.vertexShaderUrl.isEmpty() && isNetwork(data.vertexShaderUrl)) {
            found.append({ QStringLiteral("vertexShader"), data.vertexShaderUrl });
        }
    };
    // a shape's or a zone's own shader (most userData isn't one: the cheap look first)
    if (userData.contains("ProceduralEntity")) {
        add(ProceduralData::parse(userData));
    }
    // a material written into the entity: one material or a list, each with its own "procedural" block
    if (isMaterial && materialURL.startsWith("materialData") && materialData.contains("procedural")) {
        const QJsonObject root = QJsonDocument::fromJson(materialData.toUtf8()).object();
        const QJsonValue materials = root.value("materials");
        QJsonArray list = materials.isArray() ? materials.toArray() : QJsonArray{ materials };
        for (const QJsonValue& material : list) {
            const QJsonValue procedural = material.toObject().value("procedural");
            if (procedural.isObject()) {
                add(ProceduralData::parse(QString::fromUtf8(QJsonDocument(procedural.toObject()).toJson(QJsonDocument::Compact))));
            }
        }
    }
    // one of each: a material's two blocks may name the same file
    QVector<QPair<QString, QUrl>> unique;
    for (const auto& shader : found) {
        if (!unique.contains(shader)) {
            unique.append(shader);
        }
    }
    return unique;
}

CodeNearby::CodeNearby(QObject* parent) : QAbstractListModel(parent) {
    _timer.setInterval(TICK_MS);
    connect(&_timer, &QTimer::timeout, this, &CodeNearby::tick);
    _target = std::make_shared<NearbyTarget>([this](int32_t callback, const QVariantList& arguments) {
        answered(callback, arguments);
    });
}

CodeNearby::~CodeNearby() {
}

void CodeNearby::setActive(bool active) {
    if (active == _active) {
        return;
    }
    _active = active;
    ++_generation;
    _refreshAfter = false;
    if (active) {
        _scannedAt = 0;   // at once, then on the move or every 10 s
        _timer.start();
        tick();
    } else {
        _timer.stop();
        _questions.clear();
    }
}

void CodeNearby::refresh() {
    if (!_active) {
        return;
    }
    if (_scanning) {
        _refreshAfter = true;   // the one under way may have started before what the person wants to see
        return;
    }
    _scannedAt = 0;
    tick();
}

// a second's check on the main thread; the scan itself only when the avatar has moved 5 m (at most every 5 s), 10 s
// have passed or the person pressed Refresh, and off the main thread on Qt's thread pool (performance, 11-06: in Port
// Amarelle the 50 m sphere holds thousands)
void CodeNearby::tick() {
    if (!_active || _scanning) {
        return;
    }
    auto renderer = qApp->getEntities();
    auto tree = renderer ? renderer->getTree() : EntityTreePointer();
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    if (!tree || !avatar) {
        return;
    }
    const glm::vec3 center = avatar->getWorldPosition();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 since = now - _scannedAt;
    const bool moved = glm::distance(center, _scannedFrom) >= MOVE_M;
    if (_scannedAt != 0 && since < MAX_WAIT_MS && !(moved && since >= MIN_WAIT_MS)) {
        return;
    }
    const bool mayEdit = entity_script_rights::mayChangeScripts();   // the entity server's rule; unlocked too, per row
    _scanning = true;
    _scannedAt = now;
    _scannedFrom = center;
    const int generation = _generation;
    QPointer<CodeNearby> self(this);
    QThreadPool::globalInstance()->start([self, tree, center, mayEdit, generation] {
        QVector<Row> rows = scan(tree, center, mayEdit);
        QMetaObject::invokeMethod(qApp, [self, rows, generation] {
            if (self) {
                self->take(rows, generation);
            }
        }, Qt::QueuedConnection);
    });
}

// on a worker thread: the tree's read lock only for what's in the sphere (the entity packet thread's edits, a
// vehicle's too, wait on it: physics, 11-06), each entity's own fields after it, under the entity's own lock. One row
// per file: the nearest thing using it, and how many do.
QVector<CodeNearby::Row> CodeNearby::scan(const EntityTreePointer& tree, const glm::vec3& center, bool mayEdit) {
    QVector<EntityItemPointer> inSphere;
    tree->withReadLock([&] {
        QVector<QUuid> found;
        const unsigned int filter = PickFilter::getBitMask(PickFilter::FlagBit::DOMAIN_ENTITIES) |
            PickFilter::getBitMask(PickFilter::FlagBit::AVATAR_ENTITIES);
        tree->evalEntitiesInSphere(center, RADIUS_M, PickFilter(filter), found);
        inSphere.reserve(found.size());
        for (const QUuid& id : found) {
            if (EntityItemPointer entity = tree->findEntityByEntityItemID(id)) {
                inSphere.append(entity);
            }
        }
    });
    QVector<Row> rows;
    QHash<QString, int> byKey;
    auto use = [&rows, &byKey](const QString& which, const QString& value, const EntityItemPointer& entity,
                               float distance, const QString& name) {
        const QUuid id = entity->getEntityItemID();
        const QString key = keyFor(which, value, id);
        auto known = byKey.find(key);
        if (known != byKey.end()) {
            Row& row = rows[known.value()];
            row.users += 1;
            if (distance < row.distance) {
                row.entity = id;
                row.name = name;
                row.distance = distance;
            }
            return;
        }
        Row row;
        row.key = key;
        row.entity = id;
        row.which = which;
        const bool address = isAddress(value);
        row.file = address ? fileOf(value) : (name.isEmpty() ? QString("Script") : name);
        row.path = address ? pathOf(value) : QString("written in");
        row.name = name;
        row.distance = distance;
        byKey.insert(key, rows.size());
        rows.append(row);
    };
    for (const EntityItemPointer& entity : inSphere) {
        const QString script = entity->getScript();
        const QString server = entity->getServerScripts();
        const QString userData = entity->getUserData();
        const bool isMaterial = entity->getType() == EntityTypes::Material;
        QString materialURL;
        QString materialData;
        if (isMaterial) {
            auto material = std::static_pointer_cast<MaterialEntityItem>(entity);
            materialURL = material->getMaterialURL();
            materialData = material->getMaterialData();
        }
        const bool hasScript = !script.trimmed().isEmpty();
        // a world's editors only: a server script isn't listed to anyone who can't change its entity
        const bool hasServer = !server.trimmed().isEmpty() && mayEdit && !entity->getLocked();
        const bool maybeShader = userData.contains("ProceduralEntity") || (isMaterial && materialData.contains("procedural"));
        if (!hasScript && !hasServer && !maybeShader) {
            continue;
        }
        const float distance = glm::distance(center, entity->getWorldPosition());
        const QString name = entity->getName();
        if (hasScript) {
            use("script", script, entity, distance, name);
        }
        if (hasServer) {
            use("serverScripts", server, entity, distance, name);
        }
        if (maybeShader) {
            for (const auto& shader : shadersOf(isMaterial, userData, materialURL, materialData)) {
                use(shader.first, shader.second.toString(), entity, distance, name);
            }
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.distance < b.distance; });
    if (rows.size() > MAX_ROWS) {
        rows.resize(MAX_ROWS);
    }
    return rows;
}

// back on the main thread: the scan's rows into the list
void CodeNearby::take(QVector<Row> rows, int generation) {
    _scanning = false;
    if (!_active || generation != _generation) {
        return;   // hidden meanwhile
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();

    // the running states from before, by file, until the new questions answer
    QHash<QString, QString> states;
    for (const auto& row : _rows) {
        states.insert(row.key, row.state);
    }
    for (auto& row : rows) {
        row.state = states.value(row.key);
    }
    // the same files in the same order: only distances, names and counts change, the view keeps its place
    bool same = rows.size() == _rows.size();
    for (int i = 0; same && i < rows.size(); ++i) {
        same = rows[i].key == _rows[i].key;
    }
    if (same) {
        _rows = rows;
        if (!_rows.isEmpty()) {
            emit dataChanged(index(0), index(_rows.size() - 1),
                { EntityRole, NameRole, UsedByRole, UsersRole, DistanceRole });
        }
    } else {
        beginResetModel();
        _rows = rows;
        endResetModel();
        emit countChanged();
    }

    // the nearest entity scripts' status, from this client's own engine (no server round trips)
    _questions.clear();
    int asked = 0;
    for (const auto& row : _rows) {
        if (asked >= MAX_ASKED) {
            break;
        }
        if (row.which != "script") {
            continue;
        }
        ScriptCallback callback;
        callback.id = _nextCallback++;
        callback.target = _target;
        _questions.insert(callback.id, row.key);
        entities->queryPropertyMetadata(row.entity, "script", callback);
        ++asked;
    }

    if (_refreshAfter) {
        _refreshAfter = false;
        refresh();
    }
}

void CodeNearby::answered(int32_t callback, const QVariantList& arguments) {
    auto question = _questions.find(callback);
    if (question == _questions.end()) {
        return;
    }
    const QString key = question.value();
    _questions.erase(question);
    const QString status = arguments.value(1).toMap().value("status").toString();
    const QString state = status == "running" ? "running"
        : (status == "error_loading_script" || status == "error_running_script") ? "error" : QString();
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].key == key && _rows[i].state != state) {
            _rows[i].state = state;
            emit dataChanged(index(i), index(i), { StateRole });
        }
    }
}

bool CodeNearby::find(const QString& key, QString& usedBy, QString& distance, int& users) const {
    for (const auto& row : _rows) {
        if (row.key == key) {
            usedBy = usedByText(row.name, row.users);
            distance = distanceText(row.distance);
            users = row.users;
            return true;
        }
    }
    return false;
}

int CodeNearby::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : _rows.size();
}

QVariant CodeNearby::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= _rows.size()) {
        return QVariant();
    }
    const Row& row = _rows[index.row()];
    switch (role) {
        case EntityRole:
            return row.entity;
        case WhichRole:
            return row.which;
        case KindRole:
            return kindOf(row.which);
        case KindKeyRole:
            return kindKeyOf(row.which);
        case FileRole:
            return row.file;
        case NameRole:
            return row.name;
        case PathRole:
            return row.path;
        case UsedByRole:
            return usedByText(row.name, row.users);
        case UsersRole:
            return row.users;
        case DistanceRole:
            return distanceText(row.distance);
        case StateRole:
            return row.state;
        case KeyRole:
            return row.key;
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> CodeNearby::roleNames() const {
    return {
        { EntityRole, "entity" }, { WhichRole, "which" }, { KindRole, "kind" }, { KindKeyRole, "kindKey" },
        { FileRole, "file" }, { NameRole, "name" }, { PathRole, "path" }, { UsedByRole, "usedBy" },
        { UsersRole, "users" }, { DistanceRole, "distance" }, { StateRole, "state" }, { KeyRole, "key" },
    };
}
