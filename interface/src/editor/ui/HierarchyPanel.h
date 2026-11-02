//
//  HierarchyPanel.h
//  interface/src/editor/ui
//
//  The Hierarchy dock: a search field over the tree of the domain's entities, and under it how many
//  there are and how many have loaded, with Refresh for the entity server's list of them. At each row's left edge, as
//  in Unity, an eye (hidden in this builder's view only), a hand (not picked by this builder's clicks in the 3D view)
//  and a lock (the entity's Locked, for everyone): each shows while it's on, and all three under the pointer. A click
//  sets the row and everything under it, an Alt-click the row alone (Caity, 2026-10-26: "add lock, hide, ignore
//  picking to the entity list ... do it like unity").
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HierarchyPanel_h
#define hifi_HierarchyPanel_h

#include <QtCore/QList>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QColor>
#include <QtWidgets/QDockWidget>

class QItemSelection;
class QLabel;
class QLineEdit;
class QModelIndex;
class QSortFilterProxyModel;
class QToolButton;
class QTreeView;
class EntityTreeModel;
class HierarchyGutter;
class QPainter;

class HierarchyPanel : public QDockWidget {
    Q_OBJECT
public:
    explicit HierarchyPanel(QWidget* parent = nullptr);

    void setModel(EntityTreeModel* model);
    // shows these as selected without reporting it back as the user's doing
    void selectIds(const QList<QUuid>& ids);
    // opens the rows above this one and scrolls it to the middle of the panel
    void reveal(const QUuid& id);
    // opens the entity's row, so that what has just been put under it shows
    void expand(const QUuid& id);

    QTreeView* treeView() const { return _tree; }
    QLineEdit* searchField() const { return _search; }
    // the world cannot be edited: a note at the top, dimmed names, no renaming; selecting and
    // framing still work
    void setReadOnly(bool readOnly, const QString& reason);
    // a line under the counts: the list is being asked for, or the server did not send it
    void setListNote(const QString& note);

    // the row's toggles, in the order the gutter shows them; on is hidden, unpickable, locked, scripts off
    enum Toggle { Hide, NoPick, Lock, Scripts };
    // what this builder has hidden in their view and made unpickable by their clicks (the Editor keeps them)
    void setLocalStates(const QSet<QUuid>& hidden, const QSet<QUuid>& noPick);
    // whether the lock can be changed here (the world's Lock / Unlock right, and the right to edit at all), and why
    // not
    void setCanLock(bool canLock, const QString& why);
    // whether the toggle is on for that row: hidden, unpickable, locked, its scripts off
    bool isOn(Toggle toggle, const QUuid& id) const;
    // what a click on the row's toggle does (onlyThis: an Alt-click's, the row alone); false when the row isn't there,
    // the lock can't be changed, or there are no scripts to turn on or off
    bool clickToggle(Toggle toggle, const QUuid& id, bool onlyThis);
    // a stroke along a toggle's column (Blender's outliner; Caity 11-01): a press on one row's toggle and a drag, every
    // row passed taking the first row's new state, applied together on release as one undo step. strokeTarget: what a
    // click on the row's toggle would set (false when it can't change there); strokeIds: what the stroke's rows, and
    // what's under them unless onlyThis, amount to for the toggle
    bool strokeTarget(Toggle toggle, const QUuid& id, bool onlyThis, bool& on) const;
    QList<QUuid> strokeIds(Toggle toggle, const QList<QUuid>& rows, bool onlyThis) const;
    // a test's stroke (Test.hierarchyStroke): the gutter gets a press on the first row's toggle, a move over each row
    // and a release on the last, as the mouse gives them; false when a row isn't in sight
    bool testStroke(Toggle toggle, const QList<QUuid>& rows, bool onlyThis);
    // how many of this world's rows are hidden in this builder's view
    int hiddenCount() const;

    // the toggles for several entities together, as the Inspector's header shows them for its selection (Caity,
    // 10-27): on when all are, mixed when some are; usable when a click would change something, else why not
    enum ToggleLook { Off, On, Mixed };
    struct ToggleState {
        ToggleLook look { Off };
        bool usable { true };
        QString why;
    };
    QVector<ToggleState> statesFor(const QList<QUuid>& ids) const;
    // a click on the Inspector header's toggle: for these alone, by the rows' rules (the lock right; only what has
    // scripts to switch), on unless all of them are; false when nothing would change
    bool toggleIds(Toggle toggle, const QList<QUuid>& ids);
    // a toggle's icon in a cell, in the painter's pen, as the rows draw it; and the pen's colour
    static void paintToggle(QPainter& painter, Toggle toggle, const QRectF& cell, bool on);
    static QColor toggleColor(bool on, bool hovered, bool usable);

signals:
    void selectionChangedByUser(const QList<QUuid>& ids);
    void frameRequested(const QUuid& id);
    // a plain click on a row that is not loaded: go there
    void goToRequested(const QUuid& id);
    void refreshRequested();
    // a right click in the tree, after the row under it joined the selection
    void contextMenuRequested(const QPoint& globalPos);
    // a row's eye, hand, lock or script mark clicked: to turn on or off for these (the row, and everything under it
    // unless Alt-clicked; for scripts, only those with scripts that aren't locked)
    void toggleRequested(Toggle toggle, const QList<QUuid>& ids, bool on);
    // the note's Show All: everything hidden in this builder's view shown again
    void showAllRequested();

protected:
    // the tree's viewport: the row under the pointer, and the gutter drawn again with the rows
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class HierarchyGutter;
    QList<QUuid> selectedIds() const;
    QList<QUuid> withDescendants(const QUuid& id) const;
    // of these, the ones whose scripts can be turned on or off here: loaded, with a script, not locked
    QList<QUuid> scriptsToSwitch(const QList<QUuid>& ids) const;
    QUuid idAt(const QModelIndex& proxyIndex) const;
    void updateCounts();

    QLabel* _banner { nullptr };
    QLineEdit* _search { nullptr };
    QTreeView* _tree { nullptr };
    QLabel* _counts { nullptr };
    QToolButton* _refresh { nullptr };
    QString _listNote;
    QTimer _countsTimer;    // the counts are redone once after a burst of rows
    QSortFilterProxyModel* _proxy { nullptr };
    HierarchyGutter* _gutter { nullptr };
    QPersistentModelIndex _hovered;   // the row under the pointer, in the tree or the gutter: its toggles all show
    QSet<QUuid> _hidden;
    QSet<QUuid> _noPick;
    bool _canLock { true };
    QString _noLockWhy;
    bool _readOnly { false };
    EntityTreeModel* _model { nullptr };
    bool _settingSelection { false };
    QList<QUuid> _wantedIds;   // the last selection asked for; rows that arrive later are selected on arrival
};

#endif // hifi_HierarchyPanel_h
