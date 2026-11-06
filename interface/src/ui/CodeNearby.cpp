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

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <EntityTreeRenderer.h>
#include <PickFilter.h>
#include <ScriptCallback.h>

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

QString fileOf(const QString& value) {
    if (!isAddress(value)) {
        return "Written in";
    }
    const QUrl url(value.trimmed());
    return url.fileName().isEmpty() ? url.host() : url.fileName();
}

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
    if (active) {
        _scannedAt = 0;   // at once, then on the move or every 10 s
        _timer.start();
        tick();
    } else {
        _timer.stop();
        _questions.clear();
    }
}

// a second's check on the main thread; the scan itself only when the avatar has moved 5 m (at most every 5 s) or 10 s
// have passed, and off the main thread (performance, 11-06: in Port Amarelle the 50 m sphere holds thousands)
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
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    const bool mayEdit = entities->canRez() || entities->canRezTmp();
    _scanning = true;
    _scannedAt = now;
    _scannedFrom = center;
    const int generation = _generation;
    QPointer<CodeNearby> self(this);
    QtConcurrent::run([self, tree, center, mayEdit, generation] {
        QVector<Row> rows = scan(tree, center, mayEdit);
        QMetaObject::invokeMethod(qApp, [self, rows, generation] {
            if (self) {
                self->take(rows, generation);
            }
        }, Qt::QueuedConnection);
    });
}

// on a worker thread: the tree's read lock only for what's in the sphere (the entity packet thread's edits, a
// vehicle's too, wait on it: physics, 11-06), each entity's own fields after it, under the entity's own lock
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
    for (const EntityItemPointer& entity : inSphere) {
        const QString script = entity->getScript();
        const QString server = entity->getServerScripts();
        if (script.trimmed().isEmpty() && server.trimmed().isEmpty()) {
            continue;
        }
        const float distance = glm::distance(center, entity->getWorldPosition());
        const QString name = entity->getName();
        if (!script.trimmed().isEmpty()) {
            rows.append({ entity->getEntityItemID(), "script", fileOf(script), name, distance, QString() });
        }
        // a world's editors only: a server script isn't listed to anyone who can't change its entity
        if (!server.trimmed().isEmpty() && mayEdit && !entity->getLocked()) {
            rows.append({ entity->getEntityItemID(), "serverScripts", fileOf(server), name, distance, QString() });
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

    // the same rows in the same order: only distances and names change, the view keeps its place
    bool same = rows.size() == _rows.size();
    for (int i = 0; same && i < rows.size(); ++i) {
        same = rows[i].entity == _rows[i].entity && rows[i].which == _rows[i].which;
    }
    if (same) {
        for (int i = 0; i < rows.size(); ++i) {
            rows[i].state = _rows[i].state;
        }
        _rows = rows;
        if (!_rows.isEmpty()) {
            emit dataChanged(index(0), index(_rows.size() - 1), { FileRole, NameRole, DistanceRole });
        }
    } else {
        QHash<QUuid, QString> states;
        for (const auto& row : _rows) {
            if (row.which == "script") {
                states.insert(row.entity, row.state);
            }
        }
        for (auto& row : rows) {
            if (row.which == "script") {
                row.state = states.value(row.entity);
            }
        }
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
        _questions.insert(callback.id, row.entity);
        entities->queryPropertyMetadata(row.entity, "script", callback);
        ++asked;
    }
}

void CodeNearby::answered(int32_t callback, const QVariantList& arguments) {
    auto question = _questions.find(callback);
    if (question == _questions.end()) {
        return;
    }
    const QUuid entity = question.value();
    _questions.erase(question);
    const QString status = arguments.value(1).toMap().value("status").toString();
    const QString state = status == "running" ? "running"
        : (status == "error_loading_script" || status == "error_running_script") ? "error" : QString();
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].entity == entity && _rows[i].which == "script" && _rows[i].state != state) {
            _rows[i].state = state;
            emit dataChanged(index(i), index(i), { StateRole });
        }
    }
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
            return row.which == "serverScripts" ? "Server script" : "Entity script";
        case FileRole:
            return row.file;
        case NameRole:
            return row.name;
        case DistanceRole:
            return QString::number(row.distance, 'f', row.distance < 10.0f ? 1 : 0) + " m";
        case StateRole:
            return row.state;
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> CodeNearby::roleNames() const {
    return {
        { EntityRole, "entity" }, { WhichRole, "which" }, { KindRole, "kind" }, { FileRole, "file" },
        { NameRole, "name" }, { DistanceRole, "distance" }, { StateRole, "state" },
    };
}
