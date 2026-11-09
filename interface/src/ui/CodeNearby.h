//
//  CodeNearby.h
//  interface/src/ui
//
//  The Code panel's "Code near you" list (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md; the list on the left, Caity
//  "EXCELLENT, as drawn" 11-07, team/editor-ui/mockups/code-panel-list): one row per FILE within 50 m, nearest first:
//  entity scripts, server scripts, and the fragment and vertex shaders of shapes, skies and materials. Each row has the
//  file's name, its kind, its path, what uses it (the nearest, "and N more") and how far. A script written into its
//  entity's field is a row of its own. Server scripts are listed only to people who can edit their entity (Caity, 22:25
//  11-06). It looks at the world only while the Code panel is open: a scan when the avatar has moved 5 m (at most every
//  5 s), every 10 s, or on Refresh, on a worker thread with the entity tree's read lock held only for the sphere
//  search, and the running status of the nearest entity scripts from this client's own engine. Nothing while closed.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_CodeNearby_h
#define hifi_CodeNearby_h

#include <memory>

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QPair>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVector>

#include <glm/glm.hpp>

class CallbackTarget;
class EntityTree;
using EntityTreePointer = std::shared_ptr<EntityTree>;   // as EntityTree.h

class CodeNearby : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        EntityRole = Qt::UserRole + 1,   // the nearest thing that uses it: what opens
        WhichRole,      // "script", "serverScripts", "fragmentShader" or "vertexShader"
        KindRole,       // "Entity script", "Server script", "Fragment shader", "Vertex shader"
        KindKeyRole,    // "entity", "server", "fragment", "vertex" (its colour)
        FileRole,       // the file's name; a written-in script's entity name
        NameRole,       // the nearest user's name
        PathRole,       // the file's path ("/tea_sea/steam/steam-9.fs", "example.com/door.js"), or "written in"
        UsedByRole,     // "Tea Sea: chair at deck table right, and 3 more"
        UsersRole,      // how many things nearby use it
        DistanceRole,   // metres, text ("3.2 m")
        StateRole,      // an entity script's: "running", "error", or ""
        KeyRole         // the file: the kind and its address, or the entity a written-in script is in (CodeEditor::openKey)
    };

    explicit CodeNearby(QObject* parent = nullptr);
    ~CodeNearby() override;

    // shown: the panel open
    void setActive(bool active);
    // the person's Refresh: a scan now (or as soon as the one under way is done)
    void refresh();

    int count() const { return _rows.size(); }
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // a file's row by its key, for the panel's header ("used by …"): false when it isn't within 50 m
    bool find(const QString& key, QString& usedBy, QString& distance, int& users) const;

    // one key per file: the kind and its address, or the entity a written-in script is in (shared with CodeEditor)
    static QString keyFor(const QString& which, const QString& value, const QUuid& entity);
    // a shown path for an address: an atp path as it is, a web address without its scheme
    static QString pathOf(const QString& address);
    // the shaders an entity's own fields give, as the renderer reads them (ProceduralData::parse): a userData's
    // "ProceduralEntity" (shapes, zones' skies) and a material's materialData "procedural" blocks; network addresses
    // only (http, https, atp: never a file on this computer). Pairs of ("fragmentShader" | "vertexShader", address).
    static QVector<QPair<QString, QUrl>> shadersOf(bool isMaterial, const QString& userData, const QString& materialURL,
                                                   const QString& materialData);

signals:
    void countChanged();

private:
    struct Row {
        QString key;
        QUuid entity;      // the nearest user
        QString which;
        QString file;
        QString name;      // the nearest user's name
        QString path;
        int users { 1 };
        float distance { 0.0f };
        QString state;
    };

    void tick();
    static QVector<Row> scan(const EntityTreePointer& tree, const glm::vec3& center, bool mayEdit);
    void take(QVector<Row> rows, int generation);
    void answered(int32_t callback, const QVariantList& arguments);

    bool _active { false };
    bool _scanning { false };     // a scan on its worker thread
    bool _refreshAfter { false }; // Refresh pressed while a scan was under way
    int _generation { 0 };        // shown and hidden again: a scan from before is dropped
    qint64 _scannedAt { 0 };
    glm::vec3 _scannedFrom { 0.0f };
    QTimer _timer;
    QVector<Row> _rows;
    std::shared_ptr<CallbackTarget> _target;
    QHash<int32_t, QString> _questions;   // by callback id: the row's key asked about
    int32_t _nextCallback { 1 };
};

#endif // hifi_CodeNearby_h
