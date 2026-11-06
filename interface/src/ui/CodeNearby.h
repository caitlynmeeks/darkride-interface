//
//  CodeNearby.h
//  interface/src/ui
//
//  The Code panel's "Code near you" list (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md): one row per script within
//  50 m, nearest first, with its kind, file name, the entity it belongs to, the distance and whether it's running.
//  Server scripts are listed only to people who can edit their entity (Caity, 22:25 11-06). It looks at the world only
//  while the list is shown (the Code panel open with the list unfolded): every 2 s, one read of the entity tree, and
//  the running status of the nearest entity scripts from this client's own engine. Nothing while it's hidden.
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
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVector>

class CallbackTarget;

class CodeNearby : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        EntityRole = Qt::UserRole + 1,
        WhichRole,      // "script" or "serverScripts"
        KindRole,       // "Entity script", "Server script"
        FileRole,       // the file's name, or "Written in"
        NameRole,       // the entity's name
        DistanceRole,   // metres, text ("3.2 m")
        StateRole       // "running", "error", or ""
    };

    explicit CodeNearby(QObject* parent = nullptr);
    ~CodeNearby() override;

    // shown: the panel open and the list unfolded
    void setActive(bool active);

    int count() const { return _rows.size(); }
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    struct Row {
        QUuid entity;
        QString which;
        QString file;
        QString name;
        float distance { 0.0f };
        QString state;
    };

    void refresh();
    void answered(int32_t callback, const QVariantList& arguments);

    bool _active { false };
    QTimer _timer;
    QVector<Row> _rows;
    std::shared_ptr<CallbackTarget> _target;
    QHash<int32_t, QUuid> _questions;   // by callback id: the entity asked about
    int32_t _nextCallback { 1 };
};

#endif // hifi_CodeNearby_h
