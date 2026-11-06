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
const int REFRESH_MS = 2000;

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
    _timer.setInterval(REFRESH_MS);
    connect(&_timer, &QTimer::timeout, this, &CodeNearby::refresh);
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
    if (active) {
        _timer.start();
        refresh();
    } else {
        _timer.stop();
        _questions.clear();
    }
}

void CodeNearby::refresh() {
    if (!_active) {
        return;
    }
    auto renderer = qApp->getEntities();
    auto tree = renderer ? renderer->getTree() : EntityTreePointer();
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    if (!tree || !avatar) {
        return;
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    const bool mayEdit = entities->canRez() || entities->canRezTmp();
    const glm::vec3 center = avatar->getWorldPosition();

    // one read of the tree: what's in the sphere, and each one's scripts
    QVector<Row> rows;
    tree->withReadLock([&] {
        QVector<QUuid> found;
        const unsigned int filter = PickFilter::getBitMask(PickFilter::FlagBit::DOMAIN_ENTITIES) |
            PickFilter::getBitMask(PickFilter::FlagBit::AVATAR_ENTITIES);
        tree->evalEntitiesInSphere(center, RADIUS_M, PickFilter(filter), found);
        for (const QUuid& id : found) {
            EntityItemPointer entity = tree->findEntityByEntityItemID(id);
            if (!entity) {
                continue;
            }
            const QString script = entity->getScript();
            const QString server = entity->getServerScripts();
            if (script.trimmed().isEmpty() && server.trimmed().isEmpty()) {
                continue;
            }
            const float distance = glm::distance(center, entity->getWorldPosition());
            const QString name = entity->getName();
            if (!script.trimmed().isEmpty()) {
                rows.append({ id, "script", fileOf(script), name, distance, QString() });
            }
            // a world's editors only: a server script isn't listed to anyone who can't change its entity
            if (!server.trimmed().isEmpty() && mayEdit && !entity->getLocked()) {
                rows.append({ id, "serverScripts", fileOf(server), name, distance, QString() });
            }
        }
    });
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.distance < b.distance; });
    if (rows.size() > MAX_ROWS) {
        rows.resize(MAX_ROWS);
    }

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
