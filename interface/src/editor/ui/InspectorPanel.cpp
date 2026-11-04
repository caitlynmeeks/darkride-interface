//
//  InspectorPanel.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "InspectorPanel.h"

#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include "../PropertySpec.h"
#include "PropertyEditors.h"
#include "ImpostorEditor.h"
#include "ScriptEditor.h"
#include "TerrainCapsEditor.h"
#include "TerrainLayersEditor.h"
#include "TrackPointsEditor.h"
#include "UserDataEditor.h"
#include "VehiclePartsEditor.h"
#include "../../ui/DockTitleBar.h"

namespace {
    const QString NULL_UUID = QUuid().toString(QUuid::WithoutBraces);
    const QString VISIBLE_KEY = QStringLiteral("visible");
    const QString LOCKED_KEY = QStringLiteral("locked");

    // the ticks that are the entity's own say what they do for everyone
    const QHash<QString, QString> TICK_TIPS {
        { "visible", "Visible to everyone. Untick it to hide this for everyone." },
        { "ignorePickIntersection", "Everyone's clicks, lasers and teleports go through it, in Build mode and out of "
                                    "it. (The pointer above is your own clicks', in Build mode.)" },
    };

    // the header's toggle icons, as the Hierarchy's rows have them; what a click does, for the selection alone
    const char* const TOGGLE_NAMES[] = { "Hide", "NoPick", "Lock", "Scripts" };
    QString toggleTip(int toggle, HierarchyPanel::ToggleLook look) {
        const bool on = look == HierarchyPanel::On;
        const bool mixed = look == HierarchyPanel::Mixed;
        switch (toggle) {
            case HierarchyPanel::Hide:
                return on ? "Hidden in your view only. Click to show it."
                     : mixed ? "Some of these are hidden in your view. Click to hide them all."
                             : "Hide it in your view only, while you build. Everyone else still sees it.";
            case HierarchyPanel::NoPick:
                return on ? "Your clicks in the 3D view pass through it. Click to select it by clicking again."
                     : mixed ? "Your clicks pass through some of these. Click for all of them."
                             : "Let your clicks in the 3D view pass through it, while you build (the Entities list "
                               "still selects it). Only for you.";
            case HierarchyPanel::Lock:
                return on ? "Locked for everyone: no one can move, change or delete it. Click to unlock it."
                     : mixed ? "Some of these are locked. Click to lock them all."
                             : "Lock it for everyone: no one can move, change or delete it until it's unlocked.";
            default:
                return on ? "Its scripts are off, for everyone. Click to run them again."
                     : mixed ? "Some of these have their scripts off. Click to turn them all off."
                             : "Turn its scripts off, for everyone: they stop running (a door stops opening) and "
                               "keep their links, until they're turned on again.";
        }
    }

    // one of the header's toggles, drawn as the Hierarchy's rows draw it (HierarchyPanel::paintToggle): bright while
    // on, dim while off, a dash under it while mixed
    class ToggleIcon : public QAbstractButton {
    public:
        ToggleIcon(int toggle, QWidget* parent) : QAbstractButton(parent), _toggle(toggle) {
            setObjectName(QString("EditorInspectorToggle") + TOGGLE_NAMES[toggle]);
            setFixedSize(26, 22);
            setFocusPolicy(Qt::NoFocus);
            setAttribute(Qt::WA_Hover);
        }
        void setState(const HierarchyPanel::ToggleState& state) {
            _state = state;
            setProperty("look", state.look == HierarchyPanel::On ? "on" : state.look == HierarchyPanel::Mixed ? "mixed" : "off");
            setProperty("usable", state.usable);
            setToolTip(state.usable ? toggleTip(_toggle, state.look) : state.why);
            update();
        }
        bool usable() const { return _state.usable; }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const bool hovered = underMouse() && _state.usable;
            if (hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 20));
                painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
                painter.setBrush(Qt::NoBrush);
            }
            const bool on = _state.look == HierarchyPanel::On;
            const bool mixed = _state.look == HierarchyPanel::Mixed;
            painter.setPen(QPen(HierarchyPanel::toggleColor(on || mixed, hovered, _state.usable), 1.2));
            HierarchyPanel::paintToggle(painter, (HierarchyPanel::Toggle)_toggle, QRectF(rect()).adjusted(0.0, -1.0, 0.0, -1.0), on);
            if (mixed) {
                painter.drawLine(QPointF(width() / 2.0 - 2.5, height() - 2.0), QPointF(width() / 2.0 + 2.5, height() - 2.0));
            }
        }

    private:
        int _toggle;
        HierarchyPanel::ToggleState _state;
    };

    // the value of a property as text, for the show-when rules
    QString valueText(const QVariant& value) {
        if (value.userType() == QMetaType::Bool) {
            return value.toBool() ? "true" : "false";
        }
        return value.toString();
    }
}

InspectorPanel::InspectorPanel(QWidget* parent) : QDockWidget("Inspector", parent) {
    setObjectName("EditorInspector");
    setFeatures(DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);   // anywhere, see HierarchyPanel
    DockTitleBar::attach(this);
    setMinimumWidth(340);   // room for a label and three vector fields

    auto body = new QWidget(this);
    auto bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    _banner = new QLabel(body);
    _banner->setObjectName("EditorReadOnlyBanner");
    _banner->setWordWrap(true);
    _banner->hide();
    bodyLayout->addWidget(_banner);

    _scroll = new QScrollArea(body);
    _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bodyLayout->addWidget(_scroll, 1);

    _empty = new QWidget(_scroll);
    auto emptyLayout = new QVBoxLayout(_empty);
    _emptyLabel = new QLabel("Nothing selected", _empty);
    _emptyLabel->setAlignment(Qt::AlignCenter);
    _emptyLabel->setObjectName("EditorInspectorEmpty");
    emptyLayout->addWidget(_emptyLabel);

    _content = new QWidget(_scroll);
    _contentLayout = new QVBoxLayout(_content);
    _contentLayout->setContentsMargins(6, 6, 18, 6);   // the overlay scroll bar draws over the right edge
    _contentLayout->setSpacing(4);
    _title = new QLabel(_content);
    _title->setObjectName("EditorInspectorTitle");
    _title->setWordWrap(true);
    // entity names are the world's: shown as typed, never as styled text
    _title->setTextFormat(Qt::PlainText);
    _contentLayout->addWidget(_title);
    // what the selection could not load (setLoadProblem), under the title; hidden while nothing has failed
    _loadProblem = new QLabel(_content);
    _loadProblem->setObjectName("EditorInspectorLoadProblem");
    _loadProblem->setWordWrap(true);
    _loadProblem->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _loadProblem->hide();
    _contentLayout->addWidget(_loadProblem);
    _contentLayout->addStretch(1);

    _scroll->setWidget(_empty);
    _scroll->viewport()->installEventFilter(this);
    setWidget(body);
}

void InspectorPanel::setReadOnly(bool readOnly, const QString& reason) {
    _readOnly = readOnly;
    _banner->setText(reason);
    _banner->setVisible(readOnly);
    applyReadOnly();
}

void InspectorPanel::setCanLock(bool canLock, const QString& why) {
    if (canLock != _canLock || why != _noLockWhy) {
        _canLock = canLock;
        _noLockWhy = why;
        applyReadOnly();
    }
}

void InspectorPanel::setToggleStates(const QVector<HierarchyPanel::ToggleState>& states) {
    _toggleStates = states;
    for (int t = 0; t < _toggleIcons.size() && t < states.size(); ++t) {
        static_cast<ToggleIcon*>(_toggleIcons[t])->setState(states[t]);
    }
}

void InspectorPanel::applyReadOnly() {
    // the fields grey out; the labels stay live so Copy still works on them (they do not scrub). With everything
    // selected locked, they all do (the world refuses their changes): the header's padlock unlocks
    for (auto it = _rows.begin(); it != _rows.end(); ++it) {
        it->editor->setEnabled(!_readOnly && !_allLocked);
    }
    if (_lockNote) {
        _lockNote->setText(_canLock ? "Locked. Click the padlock above to unlock it."
                                    : "Locked, and you can't unlock things in this world.");
        _lockNote->setVisible(_allLocked && !_readOnly);
    }
}

bool InspectorPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _scroll->viewport() && event->type() == QEvent::Resize) {
        int width = _scroll->viewport()->width();
        if (width > 0) {
            _content->setFixedWidth(width);
            _empty->setFixedWidth(width);
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

QPixmap InspectorPanel::contentPicture() const {
    QWidget* shown = _scroll->widget();
    return shown ? shown->grab() : QPixmap();
}

void InspectorPanel::setLoadProblem(const QString& problem) {
    if (_loadProblem->text() == problem && _loadProblem->isVisible() == !problem.isEmpty()) {
        return;
    }
    _loadProblem->setText(problem.isEmpty() ? QString() : "Couldn't load:\n" + problem);
    _loadProblem->setVisible(!problem.isEmpty());
    // the name goes red with it, as the entity's row does in the Hierarchy
    _title->setProperty("loadFailed", !problem.isEmpty());
    _title->style()->unpolish(_title);
    _title->style()->polish(_title);
}

void InspectorPanel::setSummary(const QString& text) {
    _emptyLabel->setText(text);
    if (_scroll->widget() != _empty) {
        _scroll->takeWidget();
        _scroll->setWidget(_empty);
    }
    _layoutKey.clear();
}

void InspectorPanel::showEntities(const QVector<EntityItemProperties>& snapshots, EntityTypes::EntityType type,
                                  bool multiple, const QString& title) {
    if (snapshots.isEmpty()) {
        setSummary("Nothing selected");
        return;
    }
    QString key = QString("%1/%2").arg((int)type).arg(multiple ? 1 : 0);
    if (key != _layoutKey) {
        _layoutKey = key;
        rebuild(PropertySpecs::sectionsForType(type, multiple));
    }
    _title->setText(title);
    if (_scroll->widget() != _content) {
        _scroll->takeWidget();
        _scroll->setWidget(_content);
    }
    // a new selection: a track's Points list starts with no point chosen (the editors are kept between selections of
    // one type, and the row chosen in the last track must not carry over to this one)
    for (auto& row : _rows) {
        if (auto points = qobject_cast<TrackPointsEditor*>(row.editor)) {
            points->forgetChoice();
        }
    }
    applyValues(snapshots, true);
}

void InspectorPanel::refreshValues(const QVector<EntityItemProperties>& snapshots) {
    if (_scroll->widget() == _content && !snapshots.isEmpty()) {
        applyValues(snapshots, false);
    }
}

void InspectorPanel::rebuild(const QStringList& sections) {
    // everything but the title, the load problem line and the stretch goes
    const int KEPT_AT_TOP = 2;
    for (auto& row : _rows) {
        delete row.label;
        delete row.editor;
    }
    _rows.clear();
    _sections.clear();
    while (_contentLayout->count() > KEPT_AT_TOP + 1) {
        auto item = _contentLayout->takeAt(KEPT_AT_TOP);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    int position = KEPT_AT_TOP;
    for (const auto& sectionId : sections) {
        if (auto widget = buildSection(sectionId, _content)) {
            _contentLayout->insertWidget(position++, widget);
        }
    }
    applyReadOnly();
}

QWidget* InspectorPanel::buildSection(const QString& sectionId, QWidget* parent) {
    auto specs = PropertySpecs::forSection(sectionId);
    auto section = PropertySpecs::section(sectionId);
    if (specs.isEmpty() || !section) {
        return nullptr;
    }
    auto frame = new QWidget(parent);
    frame->setObjectName(section->minor ? "EditorInspectorMinorSection" : "EditorInspectorSection");
    auto frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);

    auto body = new QWidget(frame);
    body->setObjectName("EditorInspectorBody");
    auto grid = new QGridLayout(body);
    grid->setContentsMargins(8, 4, 4, 6);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);

    // sections other than the top one fold
    if (!section->label.isEmpty()) {
        auto header = new QToolButton(frame);
        header->setObjectName("EditorInspectorHeader");
        header->setText(section->label);
        header->setCheckable(true);
        header->setChecked(!_collapsed.value(sectionId, false));
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setArrowType(header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        body->setVisible(header->isChecked());
        connect(header, &QToolButton::toggled, this, [this, sectionId, header, body](bool open) {
            header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            body->setVisible(open);
            _collapsed[sectionId] = !open;
        });
        frameLayout->addWidget(header);
    }
    frameLayout->addWidget(body);

    // the section's header, as Unity's: the Visible tick and the name on one line, the other ticks under them (a
    // Mac checkbox each, its name beside it), then a note while everything selected is locked
    QWidget* head = nullptr;
    QHBoxLayout* headLine = nullptr;
    QHBoxLayout* headTicks = nullptr;
    int rowIndex = 0;
    for (auto spec : specs) {
        if (spec->headerIcon) {
            continue;   // one of the header's toggle icons
        }
        PropertyEditor* editor = nullptr;
        QWidget* label = nullptr;
        if (spec->header) {
            if (!head) {
                head = new QWidget(frame);
                head->setObjectName("EditorInspectorHead");
                auto headLayout = new QVBoxLayout(head);
                headLayout->setContentsMargins(8, 4, 4, 2);
                headLayout->setSpacing(4);
                headLine = new QHBoxLayout();
                headLine->setSpacing(6);
                headLayout->addLayout(headLine);
                headTicks = new QHBoxLayout();
                headTicks->setSpacing(14);
                headLayout->addLayout(headTicks);
                // the Hierarchy rows' toggles, for the selection
                auto icons = new QHBoxLayout();
                icons->setSpacing(2);
                _toggleIcons.clear();
                for (int t = HierarchyPanel::Hide; t <= HierarchyPanel::Scripts; ++t) {
                    auto icon = new ToggleIcon(t, head);
                    if (t < _toggleStates.size()) {
                        icon->setState(_toggleStates[t]);
                    }
                    connect(icon, &QAbstractButton::clicked, this, [this, icon, t] {
                        if (icon->usable()) {
                            emit toggleClicked(t);
                        }
                    });
                    icons->addWidget(icon);
                    _toggleIcons << icon;
                }
                icons->addStretch(1);
                headLayout->addLayout(icons);
                _lockNote = new QLabel(head);
                _lockNote->setObjectName("EditorInspectorLockNote");
                _lockNote->setWordWrap(true);
                _lockNote->hide();
                headLayout->addWidget(_lockNote);
                frameLayout->insertWidget(frameLayout->indexOf(body), head);
            }
            editor = PropertyEditor::create(*spec, head);
            editor->setToolTip(TICK_TIPS.value(spec->key));
            if (auto tick = qobject_cast<BoolEditor*>(editor)) {
                if (spec->key == VISIBLE_KEY) {
                    editor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
                    headLine->addWidget(editor);   // before the name, as Unity's active tick
                } else {
                    // each takes a share of the line: held to its size hint, the style sheet's spacing (editor.qss)
                    // cuts the end off its name
                    tick->setText(spec->label);
                    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                    headTicks->addWidget(editor);
                }
            } else {
                headLine->addWidget(editor, 1);
            }
        } else {
            editor = PropertyEditor::create(*spec, body);
            if (TICK_TIPS.contains(spec->key)) {
                editor->setToolTip(TICK_TIPS.value(spec->key));
            }
            label = editor->createLabel(body);
            label->setObjectName("EditorInspectorLabel");
            bool parts = spec->kind == PropertyKind::VehicleWheels || spec->kind == PropertyKind::VehicleSeats
                || spec->kind == PropertyKind::TrackPoints;
            bool tall = spec->kind == PropertyKind::Json || spec->kind == PropertyKind::UserData
                || spec->kind == PropertyKind::CollisionMask || spec->kind == PropertyKind::TerrainLayers
                || spec->kind == PropertyKind::Impostor || parts || ScriptEditor::isFor(*spec);
            grid->addWidget(label, rowIndex, 0, Qt::AlignLeft | (tall ? Qt::AlignTop : Qt::AlignVCenter));
            if (spec->kind == PropertyKind::UserData || spec->kind == PropertyKind::TerrainLayers || spec->kind == PropertyKind::Impostor || parts) {
                grid->addWidget(editor, rowIndex, 0, 1, 2);   // its section's header names it: it takes the whole width
            } else {
                grid->addWidget(editor, rowIndex, 1);
            }
            ++rowIndex;
        }

        QString key = spec->key;
        connect(editor, &PropertyEditor::editBegan, this, [this, key] { emit editBegan(key); });
        connect(editor, &PropertyEditor::liveValue, this, [this, key](const QVariant& v) { emit liveEdit(key, v); });
        connect(editor, &PropertyEditor::committed, this, [this, key](const QVariant& v) { emit committed(key, v); });
        connect(editor, &PropertyEditor::editCancelled, this, [this, key] { emit editCancelled(key); });
        if (auto image = qobject_cast<ImageEditor*>(editor)) {
            connect(image, &ImageEditor::uploadRequested, this, [this, key](const QString& path) {
                emit uploadRequested(key, path);
            });
        }
        if (auto layers = qobject_cast<TerrainLayersEditor*>(editor)) {
            layers->setTerrainEditor(_terrainEditor);
            connect(layers, &TerrainLayersEditor::layerChosen, this, &InspectorPanel::terrainLayerChosen);
            connect(layers, &TerrainLayersEditor::layersShown, this, &InspectorPanel::terrainLayersShown);
        }
        if (auto caps = qobject_cast<TerrainCapsEditor*>(editor)) {
            caps->setTerrainEditor(_terrainEditor);
        }
        if (auto parts = qobject_cast<VehiclePartsEditor*>(editor)) {
            connect(parts, &VehiclePartsEditor::partChosen, this, [this, key](int index) { emit vehiclePartChosen(key, index); });
        }
        if (auto points = qobject_cast<TrackPointsEditor*>(editor)) {
            connect(points, &TrackPointsEditor::pointChosen, this, &InspectorPanel::trackPointChosen);
        }
        // right-click on a label: Copy and Paste the row's value, as Unity does
        if (label) {
            label->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(label, &QWidget::customContextMenuRequested, editor, [editor, label](const QPoint& pos) {
                editor->showValueMenu(label->mapToGlobal(pos));
            });
        }
        _rows.insert(key, Row { spec, label, editor });
    }
    _sections.insert(sectionId, frame);
    return frame;
}

void InspectorPanel::applyValues(const QVector<EntityItemProperties>& snapshots, bool force) {
    for (auto it = _rows.begin(); it != _rows.end(); ++it) {
        Row& row = it.value();
        if (!force && row.editor->isEditing()) {
            continue;
        }
        if (auto layers = qobject_cast<TerrainLayersEditor*>(row.editor)) {
            // not a property: the terrain's own, read through the Terrain tool
            layers->showTerrain(snapshots.size() == 1 ? snapshots.first().getID() : QUuid(), snapshots.size() > 1);
            continue;
        }
        if (auto caps = qobject_cast<TerrainCapsEditor*>(row.editor)) {
            caps->showTerrain(snapshots.size() == 1 ? snapshots.first().getID() : QUuid(), snapshots.size() > 1);
            continue;
        }
        if (auto script = qobject_cast<ScriptEditor*>(row.editor)) {
            // the entities its status line is about and its Reload reloads
            QList<QUuid> ids;
            for (const auto& snapshot : snapshots) {
                ids << snapshot.getID();
            }
            script->setEntities(ids);
        }
        if (auto impostor = qobject_cast<ImpostorEditor*>(row.editor)) {
            // the entities its Bake button bakes, beside the entry it shows
            QList<QUuid> ids;
            for (const auto& snapshot : snapshots) {
                ids << snapshot.getID();
            }
            impostor->setEntities(ids);
        }
        QVariant first = PropertySpecs::get(it.key(), snapshots.first());
        // a value not known yet (a model's scale before the model has loaded) shows as "—"
        bool mixed = !first.isValid();
        for (int i = 1; i < snapshots.size() && !mixed; ++i) {
            mixed = PropertySpecs::get(it.key(), snapshots[i]) != first;
        }
        row.editor->setValue(first, mixed);
    }
    applyVisibility(snapshots.first());
    bool allLocked = true;
    for (const auto& snapshot : snapshots) {
        allLocked = allLocked && PropertySpecs::get(LOCKED_KEY, snapshot).toBool();
    }
    if (allLocked != _allLocked) {
        _allLocked = allLocked;
        applyReadOnly();
    }
}

bool InspectorPanel::conditionHolds(const QString& showWhen, const EntityItemProperties& first) const {
    if (showWhen.isEmpty()) {
        return true;
    }
    int orAt = showWhen.indexOf(" or ");
    if (orAt > 0) {
        return conditionHolds(showWhen.left(orAt), first) || conditionHolds(showWhen.mid(orAt + 4), first);
    }
    int andAt = showWhen.indexOf(" and ");
    if (andAt > 0) {
        return conditionHolds(showWhen.left(andAt), first) && conditionHolds(showWhen.mid(andAt + 5), first);
    }
    int notIn = showWhen.indexOf(" not in ");
    if (notIn > 0) {
        QString value = valueText(PropertySpecs::get(showWhen.left(notIn), first));
        return !showWhen.mid(notIn + 8).split(',').contains(value);
    }
    int in = showWhen.indexOf(" in ");
    if (in > 0) {
        QString value = valueText(PropertySpecs::get(showWhen.left(in), first));
        return showWhen.mid(in + 4).split(',').contains(value);
    }
    int notEqual = showWhen.indexOf("!=");
    if (notEqual > 0) {
        QString value = valueText(PropertySpecs::get(showWhen.left(notEqual), first));
        QString expected = showWhen.mid(notEqual + 2);
        if (expected.isEmpty()) {
            return !value.isEmpty() && value != NULL_UUID;   // "has a value"
        }
        return value != expected;
    }
    int equal = showWhen.indexOf('=');
    if (equal > 0) {
        QString value = valueText(PropertySpecs::get(showWhen.left(equal), first));
        return value == showWhen.mid(equal + 1);
    }
    return true;
}

void InspectorPanel::applyVisibility(const EntityItemProperties& first) {
    QStringList claimed;
    QSet<QString> sectionsShown;
    for (auto it = _rows.begin(); it != _rows.end(); ++it) {
        bool shown = conditionHolds(it->spec->showWhen, first);
        if (it->label) {
            it->label->setVisible(shown);
        }
        it->editor->setVisible(shown);
        if (shown) {
            sectionsShown.insert(it->spec->section);
        }
        if (shown && PropertySpecs::isUserDataEntry(it.key())) {
            claimed << it.key().mid(it.key().indexOf(':') + 1);
        }
    }
    // a section with nothing to show goes, header and all (a material's Surface Inputs when it comes from a URL)
    for (auto it = _sections.begin(); it != _sections.end(); ++it) {
        it.value()->setVisible(sectionsShown.contains(it.key()));
    }
    // an entry of userData with a field of its own showing is left out of the User Data list; with the field
    // hidden (a Cookie on a light made a point light), the list shows it, so that it can still be seen
    if (auto userData = qobject_cast<UserDataEditor*>(_rows.value(QStringLiteral("userData")).editor)) {
        claimed.sort();
        userData->setClaimedEntries(claimed);
    }
}

void InspectorPanel::chooseTrackPoint(int index) {
    auto row = _rows.find(QStringLiteral("trackPoints"));
    if (row != _rows.end()) {
        if (auto points = qobject_cast<TrackPointsEditor*>(row->editor)) {
            points->choose(index);
        }
    }
}
