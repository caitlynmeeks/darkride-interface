//
//  TrackPointsEditor.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TrackPointsEditor.h"

#include <cfloat>
#include <cmath>

#include <QtCore/QJsonDocument>
#include <QtGui/QVector3D>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <glm/glm.hpp>

#include <TrackCurve.h>
#include <TrackEntityItem.h>

namespace {
    const int ROW_PIXELS = 22;
    const int ROWS_SHOWN = 8;   // the list grows to this many rows, then scrolls
    const int MIN_POINTS = 2;
    const int MAX_POINTS = 256;

    QString shortNumber(double value) {
        QString text = QString::number(value, 'f', 2);
        while (text.contains('.') && (text.endsWith('0') || text.endsWith('.'))) {
            text.chop(1);
        }
        return text;
    }

    glm::vec3 positionOf(const QJsonObject& point) {
        return glm::vec3((float)point.value("x").toDouble(), (float)point.value("y").toDouble(), (float)point.value("z").toDouble());
    }
}

TrackPointsEditor::TrackPointsEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(4);

    _heading = new QLabel(spec.label, this);
    _heading->setObjectName("EditorInspectorLabel");
    layout->addWidget(_heading);

    _note = new QLabel(this);
    _note->setObjectName("EditorInspectorNote");
    _note->setWordWrap(true);
    _note->hide();
    layout->addWidget(_note);

    _body = new QWidget(this);
    auto bodyLayout = new QVBoxLayout(_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(4);
    layout->addWidget(_body);

    // the list, in the order the train runs them
    _list = new QListWidget(_body);
    _list->setObjectName("EditorVehicleList");   // the vehicle lists' look
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _list->setToolTip("The control points the curve passes through, in the track's own metres; select one to edit it, "
                      "to see it bright in the viewport and to move it with the Move gizmo. Shift+click on the curve in "
                      "the viewport inserts a point there; Delete removes the chosen one.");
    connect(_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (_updating) {
            return;
        }
        _selected = row;
        markSelectedRow();
        showSelected();
        emit pointChosen(_selected);
    });
    bodyLayout->addWidget(_list);

    auto buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(2);
    _addButton = new QToolButton(_body);
    _addButton->setText("+");
    _addButton->setAutoRaise(true);
    _addButton->setToolTip("Add a point after the selected one, halfway to the next (or on from the last on an open track)");
    connect(_addButton, &QToolButton::clicked, this, [this] { addPoint(); });
    _removeButton = new QToolButton(_body);
    _removeButton->setText("−");
    _removeButton->setAutoRaise(true);
    _removeButton->setToolTip("Remove the selected point (a track keeps two at least)");
    connect(_removeButton, &QToolButton::clicked, this, [this] { removePoint(); });
    buttons->addWidget(_addButton);
    buttons->addWidget(_removeButton);
    buttons->addStretch(1);
    bodyLayout->addLayout(buttons);

    // the selected point's fields
    _fields = new QWidget(_body);
    auto grid = new QGridLayout(_fields);
    grid->setContentsMargins(0, 2, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);
    auto field = [&](int row, const char* key, const char* label, PropertyKind kind, double min, double max, double step,
                     int decimals, const char* unit, const char* toolTip) {
        PropertySpec s;
        s.key = _spec.key + ":" + key;
        s.label = label;
        s.kind = kind;
        s.section = _spec.section;
        s.min = min;
        s.max = max;
        s.step = step;
        s.decimals = decimals;
        s.unit = unit;
        PropertyEditor* editor = PropertyEditor::create(s, _fields);
        editor->setToolTip(toolTip);
        auto fieldLabel = editor->createLabel(_fields);
        fieldLabel->setObjectName("EditorInspectorLabel");
        fieldLabel->setToolTip(toolTip);
        grid->addWidget(fieldLabel, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(editor, row, 1);
        wireField(row, editor);
        return editor;
    };
    const double reach = TrackEntityItem::MAX_POINT_COORDINATE;   // the entity clamps a point to the domain anyway
    _position = field(0, "position", "Position", PropertyKind::Vec3, -reach, reach, 0.1, 3, "m",
                      "Where the point is, in the track's own metres (from the entity's position, turned by its rotation)");
    _bank = field(1, "bank", "Bank", PropertyKind::Float, -89, 89, 1, 1, "°",
                  "The lean of the track at this point, a roll about its direction: positive leans the right side up");
    _dwell = field(2, "dwell", "Stop", PropertyKind::Float, 0, 3600, 1, 1, "s",
                   "How long the train stands here; more than zero makes this point a station, zero runs through");
    bodyLayout->addWidget(_fields);
    showSelected();
}

QWidget* TrackPointsEditor::createLabel(QWidget* parent) {
    auto label = new QWidget(parent);
    label->setFixedSize(0, 0);
    return label;
}

void TrackPointsEditor::wireField(int field, PropertyEditor* editor) {
    // a scrub: the list as it was is the undo point, the values go out live, the end is one step; Escape brings it back
    connect(editor, &PropertyEditor::editBegan, this, [this] {
        _before = _items;
        emit editBegan();
    });
    connect(editor, &PropertyEditor::liveValue, this, [this, field](const QVariant& value) {
        if (!_updating && applyField(field, value)) {
            emit liveValue(QVariant(json()));
        }
    });
    connect(editor, &PropertyEditor::committed, this, [this, field](const QVariant& value) {
        if (!_updating && applyField(field, value)) {
            commitList();
        }
    });
    connect(editor, &PropertyEditor::editCancelled, this, [this] {
        _items = _before;
        showSelected();
        emit editCancelled();
    });
}

bool TrackPointsEditor::applyField(int field, const QVariant& value) {
    if (_selected < 0 || _selected >= _items.size()) {
        return false;
    }
    QJsonObject point = _items[_selected].toObject();
    if (field == 0) {
        QVector3D v = value.value<QVector3D>();
        point["x"] = (double)v.x();
        point["y"] = (double)v.y();
        point["z"] = (double)v.z();
    } else if (field == 1) {
        point["bank"] = value.toDouble();
    } else {
        point["dwell"] = value.toDouble();
    }
    _items[_selected] = point;
    return true;
}

void TrackPointsEditor::setValue(const QVariant& value, bool mixed) {
    QString text = mixed ? QString() : value.toString();
    if (mixed == _mixed && text == _shown) {
        return;   // nothing new: the list and its selection stay
    }
    _updating = true;
    _mixed = mixed;
    _shown = text;
    _items = QJsonArray();
    _closed = false;
    if (!mixed) {
        QJsonDocument document = QJsonDocument::fromJson(text.trimmed().toUtf8());
        // the accessor wraps the list with the loop flag, so the heading can give the length
        QJsonArray array;
        if (document.isObject()) {
            array = document.object().value("points").toArray();
            _closed = document.object().value("closed").toBool();
        } else if (document.isArray()) {
            array = document.array();
        }
        for (const QJsonValue& item : array) {
            if (item.isObject()) {
                _items.append(item);
            }
        }
    }
    _note->setText(mixed ? "Differs between the selected entities" : QString());
    _note->setVisible(mixed);
    _body->setVisible(!mixed);
    _updating = false;
    rebuildList();
}

bool TrackPointsEditor::isEditing() const {
    return _position->isEditing() || _bank->isEditing() || _dwell->isEditing();
}

void TrackPointsEditor::revertTyping() {
    _position->revertTyping();
    _bank->revertTyping();
    _dwell->revertTyping();
    showSelected();
}

QVariant TrackPointsEditor::currentValue() const {
    return QVariant(_mixed ? _shown : json());
}

QString TrackPointsEditor::json() const {
    QJsonObject wrapper;
    wrapper["points"] = _items;
    wrapper["closed"] = _closed;
    return QString::fromUtf8(QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
}

void TrackPointsEditor::choose(int index) {
    if (index == _selected || index >= _items.size()) {
        return;
    }
    _updating = true;
    _selected = index;
    _list->setCurrentRow(_selected);
    _updating = false;
    markSelectedRow();
    showSelected();
}

void TrackPointsEditor::forgetChoice() {
    _selected = -1;
    _shown.clear();   // the next setValue rebuilds the list, with no row current
    _mixed = false;
}

void TrackPointsEditor::rebuildList() {
    _updating = true;
    _list->clear();
    for (int i = 0; i < _items.size(); ++i) {
        auto item = new QListWidgetItem(_list);
        item->setSizeHint(QSize(0, ROW_PIXELS));
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        _list->setItemWidget(item, makeRow(_items[i].toObject(), i));
    }
    if (_selected >= _items.size()) {
        _selected = _items.size() - 1;
    }
    _list->setCurrentRow(_selected);
    int rows = glm::clamp(int(_items.size()), 1, ROWS_SHOWN);   // size() is qsizetype on Qt 6
    _list->setFixedHeight(rows * ROW_PIXELS + 6);
    _list->setVerticalScrollBarPolicy(_items.size() > ROWS_SHOWN ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    _updating = false;
    refreshHeading();
    markSelectedRow();
    showSelected();
    emit pointChosen(_selected);
}

void TrackPointsEditor::refreshHeading() {
    // the curve's length, from the same curve the cars ride
    std::vector<glm::vec3> points;
    std::vector<float> bank;
    for (const QJsonValue& item : _items) {
        QJsonObject point = item.toObject();
        points.push_back(positionOf(point));
        bank.push_back((float)point.value("bank").toDouble());
    }
    TrackCurve curve;
    curve.build(points, bank, _closed);
    int stations = 0;
    for (const QJsonValue& item : _items) {
        if (item.toObject().value("dwell").toDouble() > 0.0) {
            ++stations;
        }
    }
    QString text = QString("%1: %2").arg(_spec.label).arg(_items.size());
    if (curve.isValid()) {
        text += QString(", %1 m").arg(shortNumber(curve.length()));
    }
    if (stations > 0) {
        text += QString(", %1 station%2").arg(stations).arg(stations == 1 ? "" : "s");
    }
    _heading->setText(text);
}

QWidget* TrackPointsEditor::makeRow(const QJsonObject& point, int index) {
    auto row = new QWidget(_list);
    row->setObjectName("EditorVehicleRow");
    row->setAttribute(Qt::WA_StyledBackground);
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 1, 6, 1);
    layout->setSpacing(8);
    auto name = new QLabel(QString("Point %1").arg(index + 1), row);
    name->setObjectName("EditorVehicleName");
    layout->addWidget(name);
    auto summary = new QLabel(summaryOf(point), row);
    summary->setObjectName("EditorVehicleSummary");
    layout->addWidget(summary, 1);
    return row;
}

QString TrackPointsEditor::summaryOf(const QJsonObject& point) const {
    glm::vec3 p = positionOf(point);
    QString text = QString("%1, %2, %3").arg(shortNumber(p.x)).arg(shortNumber(p.y)).arg(shortNumber(p.z));
    double bank = point.value("bank").toDouble();
    if (bank != 0.0) {
        text += QString("  bank %1°").arg(shortNumber(bank));
    }
    double dwell = point.value("dwell").toDouble();
    if (dwell > 0.0) {
        text += QString("  stop %1 s").arg(shortNumber(dwell));
    }
    return text;
}

void TrackPointsEditor::markSelectedRow() {
    for (int i = 0; i < _list->count(); ++i) {
        QWidget* row = _list->itemWidget(_list->item(i));
        if (!row) {
            continue;
        }
        row->setProperty("selected", i == _selected);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
}

void TrackPointsEditor::showSelected() {
    bool has = _selected >= 0 && _selected < _items.size();
    _fields->setVisible(has);
    _removeButton->setEnabled(has && _items.size() > MIN_POINTS);
    _addButton->setEnabled(_items.size() < MAX_POINTS);
    if (!has) {
        return;
    }
    bool was = _updating;
    _updating = true;
    QJsonObject point = _items[_selected].toObject();
    glm::vec3 p = positionOf(point);
    _position->setValue(QVariant(QVector3D(p.x, p.y, p.z)), false);
    _bank->setValue(QVariant(point.value("bank").toDouble()), false);
    _dwell->setValue(QVariant(point.value("dwell").toDouble()), false);
    _updating = was;
}

void TrackPointsEditor::commitList() {
    _updating = true;
    for (int i = 0; i < _list->count(); ++i) {
        if (QWidget* row = _list->itemWidget(_list->item(i))) {
            if (auto summary = row->findChild<QLabel*>("EditorVehicleSummary")) {
                summary->setText(summaryOf(_items[i].toObject()));
            }
        }
    }
    _updating = false;
    refreshHeading();
    emit committed(QVariant(json()));
}

void TrackPointsEditor::addPoint() {
    if (_items.size() >= MAX_POINTS) {
        return;
    }
    int at = _selected >= 0 && _selected < _items.size() ? _selected : _items.size() - 1;
    QJsonObject point;
    if (at < 0) {
        point["x"] = 0.0; point["y"] = 0.0; point["z"] = 0.0;
    } else {
        glm::vec3 here = positionOf(_items[at].toObject());
        glm::vec3 there;
        if (at + 1 < _items.size() || (_closed && _items.size() >= 2)) {
            // halfway to the next point (round the loop from the last)
            there = positionOf(_items[(at + 1) % _items.size()].toObject());
            there = 0.5f * (here + there);
        } else if (at > 0) {
            // on from the last point, by the last segment's step
            there = here + (here - positionOf(_items[at - 1].toObject()));
        } else {
            there = here + glm::vec3(0.0f, 0.0f, -5.0f);
        }
        point["x"] = (double)there.x; point["y"] = (double)there.y; point["z"] = (double)there.z;
    }
    point["bank"] = 0.0;
    point["dwell"] = 0.0;
    _before = _items;
    emit editBegan();
    _items.insert(at + 1, point);
    _selected = at + 1;
    rebuildList();
    emit committed(QVariant(json()));
}

void TrackPointsEditor::removePoint() {
    if (_selected < 0 || _selected >= _items.size() || _items.size() <= MIN_POINTS) {
        return;
    }
    _before = _items;
    emit editBegan();
    _items.removeAt(_selected);
    if (_selected >= _items.size()) {
        _selected = _items.size() - 1;
    }
    rebuildList();
    emit committed(QVariant(json()));
}
