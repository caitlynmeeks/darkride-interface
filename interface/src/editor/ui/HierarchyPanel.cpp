//
//  HierarchyPanel.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HierarchyPanel.h"

#include <algorithm>
#include <functional>

#include <QtCore/QCoreApplication>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QTimer>
#include <QtWidgets/QScrollBar>
#include <QtGui/QGuiApplication>
#include <QtGui/QHelpEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include "../EntityTreeModel.h"
#include "../../ui/DockTitleBar.h"

namespace {
    const int CELL = 18;          // each toggle's column in the gutter
    const int GUTTER_PAD = 2;     // before the first column and after the last
    const int STROKE_SCROLL_MS = 40;   // a toggle stroke held past the list's edge scrolls a step this often
    const QColor GUTTER_FILL("#323232");                // a shade darker than the rows, as Unity's
    const QColor ICON_ON("#d2d2d2");                    // the rows' text colour (editor.qss)
    const QColor ICON_HINT(210, 210, 210, 120);         // under the pointer, while off
    const QColor ICON_OFF(210, 210, 210, 60);           // off: always there, dim, as Photoshop's eyes (Caity, 10-27)
    const QColor ICON_UNUSABLE(210, 210, 210, 35);      // a lock or scripts that can't be changed here
    const qreal ICON_PEN = 1.2;

    const QString CLICK_HOW = "\nClick: with everything under it. Alt-click: this one alone. Drag along the column to "
                              "set every row you pass the same way.";

    // the icons, drawn as Apple's symbols are, in lines of one weight: an eye (struck through: hidden here), the
    // pointer's arrow (struck through: clicks pass it by), a padlock (shut and filled: locked) and curly braces for
    // scripts (struck through: off)
    void strike(QPainter& painter, const QPointF& c) {
        painter.drawLine(QPointF(c.x() - 5.5, c.y() + 5.5), QPointF(c.x() + 5.5, c.y() - 5.5));
    }
    void drawEye(QPainter& painter, const QRectF& cell, bool hidden) {
        QPointF c = cell.center();
        QPainterPath almond;
        almond.moveTo(c.x() - 6.0, c.y());
        almond.quadTo(c.x(), c.y() - 6.5, c.x() + 6.0, c.y());
        almond.quadTo(c.x(), c.y() + 6.5, c.x() - 6.0, c.y());
        painter.drawPath(almond);
        painter.drawEllipse(c, 1.8, 1.8);
        if (hidden) {
            strike(painter, c);
        }
    }
    void drawPointer(QPainter& painter, const QRectF& cell, bool passedBy) {
        QPointF c = cell.center();
        QPointF o = c + QPointF(-3.0, -6.0);
        QPolygonF arrow({ o, o + QPointF(0.0, 10.5), o + QPointF(2.6, 8.0), o + QPointF(4.4, 11.8),
                          o + QPointF(6.0, 11.0), o + QPointF(4.2, 7.3), o + QPointF(7.5, 7.3) });
        painter.drawPolygon(arrow);
        if (passedBy) {
            strike(painter, c);
        }
    }
    void drawLock(QPainter& painter, const QRectF& cell, bool locked) {
        QPointF c = cell.center();
        QRectF body(c.x() - 4.5, c.y() - 0.5, 9.0, 6.5);
        qreal left = c.x() - 2.8;
        qreal top = c.y() - 6.5;
        QPainterPath shackle;
        shackle.moveTo(left, body.top());
        shackle.lineTo(left, top + 2.8);
        shackle.arcTo(QRectF(left, top, 5.6, 5.6), 180.0, -180.0);
        // open: its right leg stands clear of the body
        shackle.lineTo(left + 5.6, locked ? body.top() : top + 4.0);
        painter.drawPath(shackle);
        if (locked) {
            painter.setBrush(painter.pen().color());
        }
        painter.drawRoundedRect(body, 1.2, 1.2);
        painter.setBrush(Qt::NoBrush);
    }
    void drawScript(QPainter& painter, const QRectF& cell, bool off) {
        QPointF c = cell.center();
        for (qreal side : { -1.0, 1.0 }) {
            // a brace, { on the left and } on the right: its ends curl in toward the middle, its point sticks out
            qreal x = c.x() + side * 3.0;
            QPainterPath brace;
            brace.moveTo(x - side * 1.5, c.y() - 6.0);
            brace.quadTo(x, c.y() - 6.0, x, c.y() - 3.5);
            brace.lineTo(x, c.y() - 1.5);
            brace.quadTo(x, c.y(), x + side * 1.8, c.y());
            brace.quadTo(x, c.y(), x, c.y() + 1.5);
            brace.lineTo(x, c.y() + 3.5);
            brace.quadTo(x, c.y() + 6.0, x - side * 1.5, c.y() + 6.0);
            painter.drawPath(brace);
        }
        if (off) {
            strike(painter, c);
        }
    }
}

// the strip at the tree's left edge with each row's eye, hand, lock and scripts, lined up with the rows
class HierarchyGutter : public QWidget {
public:
    HierarchyGutter(HierarchyPanel* panel, QWidget* parent) : QWidget(parent), _panel(panel) {
        setObjectName("EditorHierarchyGutter");
        setFixedWidth((HierarchyPanel::Scripts + 1) * CELL + 2 * GUTTER_PAD);
        setMouseTracking(true);
        // a stroke held past the list's top or bottom scrolls it, and takes in the rows that come into sight
        _scroll.setInterval(STROKE_SCROLL_MS);
        connect(&_scroll, &QTimer::timeout, this, [this] {
            QScrollBar* bar = _panel->_tree->verticalScrollBar();
            bar->setValue(bar->value() + (_strokeY < offset() ? -1 : 1) * bar->singleStep());
            strokeTo(_strokeY);
        });
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), GUTTER_FILL);
        painter.setRenderHint(QPainter::Antialiasing);
        QTreeView* tree = _panel->_tree;
        int viewportHeight = tree->viewport()->height();
        int dy = offset();
        for (QModelIndex index = tree->indexAt(QPoint(0, 0)); index.isValid(); index = tree->indexBelow(index)) {
            QRect row = tree->visualRect(index);
            if (row.top() > viewportHeight) {
                break;
            }
            QUuid id = index.data(EntityTreeModel::IdRole).toUuid();
            bool hovered = _panel->_hovered.isValid() && QModelIndex(_panel->_hovered) == index;
            bool scripted = index.data(EntityTreeModel::ScriptsRole).toInt() != EntityTreeModel::NoScripts;
            for (int t = HierarchyPanel::Hide; t <= HierarchyPanel::Scripts; ++t) {
                auto toggle = (HierarchyPanel::Toggle)t;
                bool on = _panel->isOn(toggle, id);
                if (t == _stroke && (toggle != HierarchyPanel::Scripts || scripted) && inStroke(index)) {
                    on = _strokeOn;   // what the stroke will set, shown as it passes
                }
                if (toggle == HierarchyPanel::Scripts && !on && !hovered && !scripted) {
                    continue;   // braces only where there's a script to switch, or under the pointer
                }
                // every row shows its toggles, off ones dim; what can't be changed here is worked out for the row
                // under the pointer and what's on (scripts under a row cost a walk of its descendants)
                bool usable = on || hovered ? usableHere(toggle, id)
                                            : toggle != HierarchyPanel::Lock || _panel->_canLock;
                painter.setPen(QPen(HierarchyPanel::toggleColor(on, hovered, usable), ICON_PEN));
                HierarchyPanel::paintToggle(painter, toggle, QRectF(GUTTER_PAD + t * CELL, row.top() + dy, CELL,
                                                                    row.height()), on);
            }
        }
    }

    // A press on a toggle starts a stroke (Blender's outliner, Caity 11-01): dragging along the column, every row passed
    // takes the first row's new state, shown as it passes and applied to them all on release, one undo step. A press
    // and release on one row is the click it always was (with what's under the row; Alt: the row alone).
    void mousePressEvent(QMouseEvent* event) override {
        int toggle = toggleAt(event->pos().x());
        QModelIndex index = rowAt(event->pos().y());
        if (event->button() == Qt::LeftButton && toggle >= 0 && index.isValid()) {
            const bool onlyThis = event->modifiers().testFlag(Qt::AltModifier);
            bool on = false;
            const QUuid id = index.data(EntityTreeModel::IdRole).toUuid();
            if (_panel->strokeTarget((HierarchyPanel::Toggle)toggle, id, onlyThis, on)) {
                _stroke = toggle;
                _strokeOn = on;
                _strokeOnlyThis = onlyThis;
                _strokeRows = { id };
                _strokeSet = { id };
                _strokeLast = index;
                _strokeY = event->pos().y();
                update();
            }
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        hover(rowAt(event->pos().y()));
        if (_stroke >= 0 && event->buttons().testFlag(Qt::LeftButton)) {
            _strokeY = event->pos().y();
            strokeTo(_strokeY);
            const int top = offset();
            const bool outside = _strokeY < top || _strokeY >= top + _panel->_tree->viewport()->height();
            if (outside && !_scroll.isActive()) {
                _scroll.start();
            } else if (!outside) {
                _scroll.stop();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (_stroke >= 0) {
            _scroll.stop();
            auto toggle = (HierarchyPanel::Toggle)_stroke;
            QList<QUuid> rows = _strokeRows;
            _stroke = -1;
            _strokeRows.clear();
            _strokeSet.clear();
            const QList<QUuid> ids = _panel->strokeIds(toggle, rows, _strokeOnlyThis);
            if (!ids.isEmpty()) {
                emit _panel->toggleRequested(toggle, ids, _strokeOn);
            }
            update();
        }
        event->accept();
    }

    void leaveEvent(QEvent*) override {
        hover(QModelIndex());
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            auto help = static_cast<QHelpEvent*>(event);
            int toggle = toggleAt(help->pos().x());
            if (toggle < 0 || !rowAt(help->pos().y()).isValid()) {
                QToolTip::hideText();
                event->ignore();
                return true;
            }
            QModelIndex index = rowAt(help->pos().y());
            QToolTip::showText(help->globalPos(),
                               tip((HierarchyPanel::Toggle)toggle, index.data(EntityTreeModel::IdRole).toUuid()), this);
            return true;
        }
        return QWidget::event(event);
    }

private:
    // the stroke takes in every row from the last one to the row at y (the nearest in sight when y is past the list's
    // edge), so a quick move skips none; rows whose toggle can't change there are passed over
    void strokeTo(int y) {
        QTreeView* tree = _panel->_tree;
        const int viewportY = std::max(0, std::min(y - offset(), tree->viewport()->height() - 1));
        QModelIndex target = tree->indexAt(QPoint(0, viewportY));
        if (!target.isValid()) {
            // below the last row of a short list: the last row
            for (QModelIndex at = tree->indexAt(QPoint(0, 0)); at.isValid(); at = tree->indexBelow(at)) {
                target = at;
            }
        }
        if (!target.isValid() || !_strokeLast.isValid() || target == QModelIndex(_strokeLast)) {
            return;
        }
        const bool down = tree->visualRect(target).top() > tree->visualRect(_strokeLast).top();
        for (QModelIndex at = _strokeLast; at.isValid() && at != target;) {
            at = down ? tree->indexBelow(at) : tree->indexAbove(at);
            if (!at.isValid()) {
                break;
            }
            const QUuid id = at.data(EntityTreeModel::IdRole).toUuid();
            bool on = false;
            if (!_strokeSet.contains(id) && _panel->strokeTarget((HierarchyPanel::Toggle)_stroke, id, _strokeOnlyThis, on)) {
                _strokeRows << id;
                _strokeSet.insert(id);
            }
        }
        _strokeLast = target;
        update();
    }
    // a row the stroke will change: one it passed, or (without Alt) one under such a row
    bool inStroke(const QModelIndex& index) const {
        for (QModelIndex at = index; at.isValid(); at = _strokeOnlyThis ? QModelIndex() : at.parent()) {
            if (_strokeSet.contains(at.data(EntityTreeModel::IdRole).toUuid())) {
                return true;
            }
        }
        return false;
    }

    int _stroke { -1 };   // the toggle being stroked, or -1
    bool _strokeOn { false };
    bool _strokeOnlyThis { false };
    QList<QUuid> _strokeRows;   // in the order passed
    QSet<QUuid> _strokeSet;
    QPersistentModelIndex _strokeLast;
    int _strokeY { 0 };
    QTimer _scroll;

    // the tree's viewport top in the gutter's coordinates: the rows' heights there are the gutter's
    int offset() const {
        return _panel->_tree->viewport()->mapTo(window(), QPoint()).y() - mapTo(window(), QPoint()).y();
    }
    QModelIndex rowAt(int y) const {
        QTreeView* tree = _panel->_tree;
        int viewportY = y - offset();
        if (viewportY < 0 || viewportY >= tree->viewport()->height()) {
            return QModelIndex();
        }
        return tree->indexAt(QPoint(0, viewportY));
    }
    int toggleAt(int x) const {
        int t = (x - GUTTER_PAD) / CELL;
        return x >= GUTTER_PAD && t <= HierarchyPanel::Scripts ? t : -1;
    }
    // whether a click on the toggle would change something on that row
    bool usableHere(HierarchyPanel::Toggle toggle, const QUuid& id) const {
        if (toggle == HierarchyPanel::Lock) {
            return _panel->_canLock;
        }
        if (toggle == HierarchyPanel::Scripts) {
            // the row itself, or else what's under it (worked out only for the row under the pointer, and those
            // whose own scripts are off)
            return !_panel->_readOnly && (!_panel->scriptsToSwitch({ id }).isEmpty()
                                          || !_panel->scriptsToSwitch(_panel->withDescendants(id)).isEmpty());
        }
        return true;
    }
    void hover(const QModelIndex& index) {
        if (QModelIndex(_panel->_hovered) != index) {
            _panel->_hovered = index;
            update();
        }
    }
    QString tip(HierarchyPanel::Toggle toggle, const QUuid& id) const {
        if (toggle == HierarchyPanel::Scripts) {
            if (_panel->_readOnly) {
                return "You can't change things in this world.";
            }
            if (!usableHere(toggle, id)) {
                bool locked = _panel->isOn(HierarchyPanel::Lock, id);
                return locked ? "Locked: unlock it to turn its scripts on or off."
                              : "No scripts here: nothing on this row or under it has a script.";
            }
            return "Scripts off, for everyone: they stop running (a door stops opening) and keep their links, until "
                "they're turned on again." + CLICK_HOW;
        }
        if (toggle == HierarchyPanel::Hide) {
            return "Hide in your view only, while you build. Everyone else still sees it." + CLICK_HOW;
        }
        if (toggle == HierarchyPanel::NoPick) {
            return "Can't be selected by your clicks in the 3D view, while you build (the Entities list still selects "
                "it). Only for you." + CLICK_HOW;
        }
        return _panel->_canLock ? "Locked for everyone: no one can move, change or delete it." + CLICK_HOW
                                : _panel->_noLockWhy;
    }

    HierarchyPanel* _panel;
};

HierarchyPanel::HierarchyPanel(QWidget* parent) : QDockWidget("Entities", parent) {
    setObjectName("EditorHierarchy");
    // anywhere, floating or docked, tabbed with another panel (dockable panels, step 2): every change of its window
    // under the GL view's guard (ui/DockTitleBar.h), which once was why it was fixed in place
    setFeatures(DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);
    DockTitleBar::attach(this);
    setMinimumWidth(220);

    auto body = new QWidget(this);
    auto layout = new QVBoxLayout(body);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    _search = new QLineEdit(body);
    _search->setPlaceholderText("Search");
    _search->setClearButtonEnabled(true);

    _tree = new QTreeView(body);
    _tree->setHeaderHidden(true);
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tree->setUniformRowHeights(true);
    _tree->setIndentation(14);
    _tree->setFrameShape(QFrame::NoFrame);
    _tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    _tree->setExpandsOnDoubleClick(false);
    // rows dragged onto another become its children, onto the empty space below the rows top-level entities
    _tree->setDragEnabled(true);
    _tree->setAcceptDrops(true);
    _tree->setDropIndicatorShown(true);
    _tree->setDragDropMode(QAbstractItemView::InternalMove);

    // the search filters the tree and keeps a match's ancestors; names sort case-blind
    _proxy = new QSortFilterProxyModel(this);
    _proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    _proxy->setFilterRole(Qt::DisplayRole);
    _proxy->setRecursiveFilteringEnabled(true);
    _proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _proxy->setDynamicSortFilter(true);
    connect(_search, &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);

    // a row that is not loaded can't be selected for editing; a plain click takes the camera to it, and it is
    // selected when it arrives
    connect(_tree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        auto modifiers = QGuiApplication::keyboardModifiers();
        if (index.data(EntityTreeModel::LoadedRole).toBool()
            || (modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier))) {
            return;
        }
        QUuid id = idAt(index);
        if (!id.isNull()) {
            emit goToRequested(id);
        }
    });
    connect(_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        QUuid id = idAt(index);
        if (!id.isNull()) {
            emit frameRequested(id);
        }
    });
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(_tree, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        // a right click on a row outside the selection selects that row first, as Unity does
        QModelIndex index = _tree->indexAt(pos);
        if (index.isValid() && !_tree->selectionModel()->isSelected(index)) {
            _tree->setCurrentIndex(index);
        }
        emit contextMenuRequested(_tree->viewport()->mapToGlobal(pos));
    });

    _banner = new QLabel(body);
    _banner->setObjectName("EditorReadOnlyBanner");
    _banner->setWordWrap(true);
    _banner->hide();

    _counts = new QLabel(body);
    _counts->setObjectName("EditorHierarchyCounts");
    _counts->setWordWrap(true);
    _counts->setTextFormat(Qt::RichText);
    connect(_counts, &QLabel::linkActivated, this, &HierarchyPanel::showAllRequested);
    _refresh = new QToolButton(body);
    _refresh->setObjectName("EditorHierarchyRefresh");
    _refresh->setText("Refresh");
    _refresh->setToolTip("Ask the server again for everything in the world");
    connect(_refresh, &QToolButton::clicked, this, &HierarchyPanel::refreshRequested);
    auto countsRow = new QHBoxLayout();
    countsRow->setContentsMargins(0, 0, 0, 0);
    countsRow->addWidget(_counts, 1);
    countsRow->addWidget(_refresh, 0, Qt::AlignTop);

    _countsTimer.setSingleShot(true);
    _countsTimer.setInterval(150);
    connect(&_countsTimer, &QTimer::timeout, this, &HierarchyPanel::updateCounts);

    // the toggles' strip, then the tree
    _gutter = new HierarchyGutter(this, body);
    _tree->viewport()->setMouseTracking(true);
    _tree->viewport()->installEventFilter(this);
    auto treeRow = new QHBoxLayout();
    treeRow->setContentsMargins(0, 0, 0, 0);
    treeRow->setSpacing(0);
    treeRow->addWidget(_gutter);
    treeRow->addWidget(_tree, 1);

    layout->addWidget(_banner);
    layout->addWidget(_search);
    layout->addLayout(treeRow, 1);
    layout->addLayout(countsRow);
    setWidget(body);
}

void HierarchyPanel::setListNote(const QString& note) {
    _listNote = note;
    updateCounts();
}

void HierarchyPanel::updateCounts() {
    if (!_model) {
        return;
    }
    int loaded = _model->loadedCount();
    QStringList lines;
    lines << (_model->hasList() ? QString("%1 entities, %2 loaded").arg(_model->count()).arg(loaded)
                                : QString("%1 loaded").arg(loaded));
    if (!_listNote.isEmpty()) {
        lines << _listNote.toHtmlEscaped();
    }
    // what this builder has hidden stays hidden here until shown again, so it can't be lost: how many, and a way back
    int hidden = hiddenCount();
    if (hidden > 0) {
        lines << QString("%1 hidden in your view · <a href=\"show\" style=\"color: #6fa8dc; text-decoration: none;\">"
                         "Show&nbsp;All</a>").arg(hidden);
    }
    // scripts turned off stay off for everyone: how many, so they aren't forgotten
    int scriptsOff = _model->scriptsOffCount();
    if (scriptsOff > 0) {
        lines << QString("%1 with scripts off, for everyone").arg(scriptsOff);
    }
    _counts->setText(lines.join("<br>"));
}

void HierarchyPanel::setLocalStates(const QSet<QUuid>& hidden, const QSet<QUuid>& noPick) {
    _hidden = hidden;
    _noPick = noPick;
    _gutter->update();
    updateCounts();
}

void HierarchyPanel::setCanLock(bool canLock, const QString& why) {
    _canLock = canLock;
    _noLockWhy = why;
    _gutter->update();
}

bool HierarchyPanel::isOn(Toggle toggle, const QUuid& id) const {
    switch (toggle) {
        case Hide:
            return _hidden.contains(id);
        case NoPick:
            return _noPick.contains(id);
        case Lock:
            return _model && _model->indexForId(id).data(EntityTreeModel::LockedRole).toBool();
        case Scripts:
            return _model && _model->indexForId(id).data(EntityTreeModel::ScriptsRole).toInt()
                == EntityTreeModel::ScriptsOff;
    }
    return false;
}

bool HierarchyPanel::strokeTarget(Toggle toggle, const QUuid& id, bool onlyThis, bool& on) const {
    if (!_model || id.isNull() || !_model->indexForId(id).isValid()) {
        return false;
    }
    // a lock is the entity's own: it needs the right, and the entity here to change
    if (toggle == Lock && (!_canLock || !_model->isLoaded(id))) {
        return false;
    }
    on = !isOn(toggle, id);
    if (toggle == Scripts) {
        // only what has scripts to switch; a row without scripts of its own turns off what runs under it, or, when
        // nothing there runs, turns it all on again
        const QList<QUuid> ids = _readOnly ? QList<QUuid>() : scriptsToSwitch(onlyThis ? QList<QUuid> { id } : withDescendants(id));
        if (ids.isEmpty()) {
            return false;
        }
        if (_model->indexForId(id).data(EntityTreeModel::ScriptsRole).toInt() == EntityTreeModel::NoScripts) {
            on = std::any_of(ids.begin(), ids.end(), [this](const QUuid& each) { return !isOn(Scripts, each); });
        }
    }
    return true;
}

QList<QUuid> HierarchyPanel::strokeIds(Toggle toggle, const QList<QUuid>& rows, bool onlyThis) const {
    QList<QUuid> ids;
    QSet<QUuid> seen;
    for (const auto& row : rows) {
        for (const auto& id : onlyThis ? QList<QUuid> { row } : withDescendants(row)) {
            if (!seen.contains(id)) {
                seen.insert(id);
                ids << id;
            }
        }
    }
    if (toggle == Scripts) {
        ids = _readOnly ? QList<QUuid>() : scriptsToSwitch(ids);
    }
    return ids;
}

bool HierarchyPanel::clickToggle(Toggle toggle, const QUuid& id, bool onlyThis) {
    bool on = false;
    if (!strokeTarget(toggle, id, onlyThis, on)) {
        return false;
    }
    const QList<QUuid> ids = strokeIds(toggle, { id }, onlyThis);
    if (ids.isEmpty()) {
        return false;
    }
    emit toggleRequested(toggle, ids, on);
    return true;
}

bool HierarchyPanel::testStroke(Toggle toggle, const QList<QUuid>& rows, bool onlyThis) {
    // the rows' toggles where the gutter draws them, found among the rows in sight
    QList<QPoint> points;
    for (const auto& id : rows) {
        QModelIndex found;
        for (QModelIndex at = _tree->indexAt(QPoint(0, 0)); at.isValid(); at = _tree->indexBelow(at)) {
            if (at.data(EntityTreeModel::IdRole).toUuid() == id) {
                found = at;
                break;
            }
        }
        if (!found.isValid()) {
            return false;
        }
        QPoint center = _tree->viewport()->mapToGlobal(_tree->visualRect(found).center());
        points << QPoint(GUTTER_PAD + (int)toggle * CELL + CELL / 2, _gutter->mapFromGlobal(center).y());
    }
    if (points.isEmpty()) {
        return false;
    }
    const Qt::KeyboardModifiers modifiers = onlyThis ? Qt::AltModifier : Qt::NoModifier;
    QMouseEvent press(QEvent::MouseButtonPress, points.first(), Qt::LeftButton, Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(_gutter, &press);
    for (const auto& point : points) {
        QMouseEvent move(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton, modifiers);
        QCoreApplication::sendEvent(_gutter, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, points.last(), Qt::LeftButton, Qt::NoButton, modifiers);
    QCoreApplication::sendEvent(_gutter, &release);
    return true;
}

QList<QUuid> HierarchyPanel::scriptsToSwitch(const QList<QUuid>& ids) const {
    QList<QUuid> switchable;
    for (const auto& id : ids) {
        QModelIndex index = _model ? _model->indexForId(id) : QModelIndex();
        if (index.isValid() && index.data(EntityTreeModel::LoadedRole).toBool()
            && index.data(EntityTreeModel::ScriptsRole).toInt() != EntityTreeModel::NoScripts
            && !index.data(EntityTreeModel::LockedRole).toBool()) {
            switchable << id;
        }
    }
    return switchable;
}

QVector<HierarchyPanel::ToggleState> HierarchyPanel::statesFor(const QList<QUuid>& ids) const {
    QVector<ToggleState> states(Scripts + 1);
    if (!_model || ids.isEmpty()) {
        return states;
    }
    // scripts: only what has a script counts, on or off
    QList<QUuid> scripted;
    for (const auto& id : ids) {
        if (_model->indexForId(id).data(EntityTreeModel::ScriptsRole).toInt() != EntityTreeModel::NoScripts) {
            scripted << id;
        }
    }
    for (int t = Hide; t <= Scripts; ++t) {
        auto toggle = (Toggle)t;
        const QList<QUuid>& counted = toggle == Scripts ? scripted : ids;
        int on = (int)std::count_if(counted.begin(), counted.end(), [this, toggle](const QUuid& id) {
            return isOn(toggle, id);
        });
        states[t].look = on == 0 ? Off : on == counted.size() ? On : Mixed;
    }
    bool anyLoaded = std::any_of(ids.begin(), ids.end(), [this](const QUuid& id) { return _model->isLoaded(id); });
    states[Lock].usable = _canLock && anyLoaded;
    states[Lock].why = !_canLock ? _noLockWhy : "Not here yet: it hasn't loaded.";
    states[Scripts].usable = !_readOnly && !scriptsToSwitch(ids).isEmpty();
    states[Scripts].why = _readOnly ? QString("You can't change things in this world.")
                          : scripted.isEmpty() ? QString("No scripts here.")
                                               : QString("Locked: unlock it to turn its scripts on or off.");
    return states;
}

bool HierarchyPanel::toggleIds(Toggle toggle, const QList<QUuid>& ids) {
    if (!_model) {
        return false;
    }
    QList<QUuid> targets;
    for (const auto& id : ids) {
        // a lock is the entity's own: it needs the right, and the entity here to change
        if (_model->indexForId(id).isValid() && (toggle != Lock || (_canLock && _model->isLoaded(id)))) {
            targets << id;
        }
    }
    if (toggle == Scripts) {
        targets = _readOnly ? QList<QUuid>() : scriptsToSwitch(targets);
    }
    if (targets.isEmpty()) {
        return false;
    }
    bool on = !std::all_of(targets.begin(), targets.end(), [this, toggle](const QUuid& id) { return isOn(toggle, id); });
    emit toggleRequested(toggle, targets, on);
    return true;
}

void HierarchyPanel::paintToggle(QPainter& painter, Toggle toggle, const QRectF& cell, bool on) {
    if (toggle == Hide) {
        drawEye(painter, cell, on);
    } else if (toggle == NoPick) {
        drawPointer(painter, cell, on);
    } else if (toggle == Lock) {
        drawLock(painter, cell, on);
    } else {
        drawScript(painter, cell, on);
    }
}

QColor HierarchyPanel::toggleColor(bool on, bool hovered, bool usable) {
    return !usable ? ICON_UNUSABLE : on ? ICON_ON : hovered ? ICON_HINT : ICON_OFF;
}

int HierarchyPanel::hiddenCount() const {
    int count = 0;
    if (_model) {
        for (const auto& id : _hidden) {
            count += _model->indexForId(id).isValid() ? 1 : 0;
        }
    }
    return count;
}

QList<QUuid> HierarchyPanel::withDescendants(const QUuid& id) const {
    QList<QUuid> ids { id };
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int row = 0; row < _model->rowCount(parent); ++row) {
            QModelIndex child = _model->index(row, 0, parent);
            ids << child.data(EntityTreeModel::IdRole).toUuid();
            walk(child);
        }
    };
    QModelIndex start = _model->indexForId(id);
    if (start.isValid()) {
        walk(start);
    }
    return ids;
}

bool HierarchyPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _tree->viewport()) {
        switch (event->type()) {
            case QEvent::MouseMove: {
                QModelIndex index = _tree->indexAt(static_cast<QMouseEvent*>(event)->pos());
                if (QModelIndex(_hovered) != index) {
                    _hovered = index;
                    _gutter->update();
                }
                break;
            }
            case QEvent::Leave:
                _hovered = QModelIndex();
                _gutter->update();
                break;
            case QEvent::Paint:
                // scrolled, opened, closed, a row's lock changed: the gutter follows the rows
                _gutter->update();
                break;
            default:
                break;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void HierarchyPanel::setReadOnly(bool readOnly, const QString& reason) {
    _readOnly = readOnly;
    if (_gutter) {
        _gutter->update();
    }
    _banner->setText(reason);
    _banner->setVisible(readOnly);
    _tree->setEditTriggers(readOnly ? QAbstractItemView::NoEditTriggers
                                    : QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    _tree->setDragEnabled(!readOnly);
    // the style sheet dims the names through this property
    _tree->setProperty("readOnly", readOnly);
    _tree->style()->unpolish(_tree);
    _tree->style()->polish(_tree);
    _tree->viewport()->update();
}

void HierarchyPanel::setModel(EntityTreeModel* model) {
    _model = model;
    _proxy->setSourceModel(model);
    _tree->setModel(_proxy);
    _proxy->setSortRole(EntityTreeModel::CreatedRole);   // siblings in the order they were made, as in Unity
    _proxy->sort(0, Qt::AscendingOrder);
    auto recount = [this] {
        if (!_countsTimer.isActive()) {
            _countsTimer.start();
        }
    };
    connect(model, &QAbstractItemModel::rowsInserted, this, recount);
    connect(model, &QAbstractItemModel::rowsRemoved, this, recount);
    connect(model, &QAbstractItemModel::modelReset, this, recount);
    connect(model, &QAbstractItemModel::dataChanged, this, recount);
    updateCounts();
    connect(_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!_settingSelection) {
            _wantedIds = selectedIds();
            emit selectionChangedByUser(_wantedIds);
        }
    });
    // a new entity is selected before its row exists (the model adds rows a moment later):
    // when rows arrive, a wanted one that is not yet selected gets selected
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] {
        if (_wantedIds.isEmpty()) {
            return;
        }
        auto selectionModel = _tree->selectionModel();
        for (const auto& id : _wantedIds) {
            QModelIndex proxyIndex = _proxy->mapFromSource(_model->indexForId(id));
            if (proxyIndex.isValid() && !selectionModel->isSelected(proxyIndex)) {
                selectIds(_wantedIds);
                return;
            }
        }
    });
}

QUuid HierarchyPanel::idAt(const QModelIndex& proxyIndex) const {
    return proxyIndex.data(EntityTreeModel::IdRole).toUuid();
}

QList<QUuid> HierarchyPanel::selectedIds() const {
    QList<QUuid> ids;
    if (!_tree->selectionModel()) {
        return ids;
    }
    for (const auto& index : _tree->selectionModel()->selectedRows()) {
        QUuid id = idAt(index);
        if (!id.isNull()) {
            ids << id;
        }
    }
    return ids;
}

void HierarchyPanel::selectIds(const QList<QUuid>& ids) {
    auto selectionModel = _tree->selectionModel();
    if (!_model || !selectionModel) {
        return;
    }
    _wantedIds = ids;
    _settingSelection = true;
    QItemSelection selection;
    QModelIndex first;
    for (const auto& id : ids) {
        QModelIndex proxyIndex = _proxy->mapFromSource(_model->indexForId(id));
        if (!proxyIndex.isValid()) {
            continue;
        }
        selection.select(proxyIndex, proxyIndex);
        if (!first.isValid()) {
            first = proxyIndex;
        }
    }
    selectionModel->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (first.isValid()) {
        selectionModel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        _tree->scrollTo(first);
    }
    _settingSelection = false;
}

void HierarchyPanel::expand(const QUuid& id) {
    if (!_model) {
        return;
    }
    QModelIndex proxyIndex = _proxy->mapFromSource(_model->indexForId(id));
    if (proxyIndex.isValid()) {
        _tree->expand(proxyIndex);
    }
}

void HierarchyPanel::reveal(const QUuid& id) {
    if (!_model) {
        return;
    }
    QModelIndex proxyIndex = _proxy->mapFromSource(_model->indexForId(id));
    if (!proxyIndex.isValid()) {
        return;   // not in the tree yet, or hidden by the search
    }
    for (QModelIndex parent = proxyIndex.parent(); parent.isValid(); parent = parent.parent()) {
        _tree->expand(parent);
    }
    _tree->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
}
