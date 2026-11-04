//
//  VehiclePartsEditor.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "VehiclePartsEditor.h"

#include <cmath>

#include <QtCore/QJsonDocument>
#include <QtGui/QVector3D>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace {
    const int ROW_PIXELS = 22;
    const int ROWS_SHOWN = 6;   // the list grows to this many rows, then scrolls

    // the defaults the physics gives a wheel without the key (WheeledVehicleSpec)
    const double DEFAULT_RADIUS = 0.35;
    const double DEFAULT_WIDTH = 0.2;
    const double DEFAULT_MASS = 20.0;
    const double DEFAULT_TRAVEL = 0.2;
    const double DEFAULT_TARGET = 0.5;

    // a vector as the JSON holds it, [x, y, z] or {"x", "y", "z"} (Unity exporters write both)
    bool readVec3(const QJsonValue& value, QVector3D& out) {
        if (value.isArray()) {
            QJsonArray array = value.toArray();
            if (array.size() < 3) {
                return false;
            }
            out = QVector3D((float)array[0].toDouble(), (float)array[1].toDouble(), (float)array[2].toDouble());
            return true;
        }
        if (value.isObject()) {
            QJsonObject object = value.toObject();
            if (!object.contains("x") || !object.contains("y") || !object.contains("z")) {
                return false;
            }
            out = QVector3D((float)object["x"].toDouble(), (float)object["y"].toDouble(), (float)object["z"].toDouble());
            return true;
        }
        return false;
    }
    // written back in the form the key had; a list for a new one
    QJsonValue writeVec3(const QJsonValue& was, const QVector3D& v) {
        if (was.isObject()) {
            QJsonObject object = was.toObject();
            object["x"] = (double)v.x();
            object["y"] = (double)v.y();
            object["z"] = (double)v.z();
            return object;
        }
        return QJsonArray { (double)v.x(), (double)v.y(), (double)v.z() };
    }
    double readNumber(const QJsonObject& part, const QString& key, double fallback) {
        QJsonValue value = part.value(key);
        return value.isDouble() ? value.toDouble() : fallback;
    }
    // true/false, or a number (0 is false), as the physics reads it
    bool readBool(const QJsonObject& part, const QString& key, bool fallback) {
        QJsonValue value = part.value(key);
        if (value.isBool()) {
            return value.toBool();
        }
        if (value.isDouble()) {
            return value.toDouble() != 0.0;
        }
        return fallback;
    }
    QString shortNumber(double value) {
        return QString::number(value, 'g', 3);
    }
    // "front left" <-> "front right", "L" <-> "R" on its own; empty when the name has no side in it
    QString mirroredName(const QString& name) {
        static const QVector<QPair<QString, QString>> SIDES = { { "left", "right" }, { "Left", "Right" }, { "LEFT", "RIGHT" } };
        for (const auto& side : SIDES) {
            if (name.contains(side.first)) {
                return QString(name).replace(side.first, side.second);
            }
            if (name.contains(side.second)) {
                return QString(name).replace(side.second, side.first);
            }
        }
        static const QRegularExpression SIDE_LETTER("(^|[^A-Za-z])([LR])([^A-Za-z]|$)");
        auto match = SIDE_LETTER.match(name);
        if (match.hasMatch()) {
            QString copy = name;
            copy.replace(match.capturedStart(2), 1, match.captured(2) == "L" ? "R" : "L");
            return copy;
        }
        return QString();
    }
}

VehiclePartsEditor::VehiclePartsEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    _wheels = spec.kind == PropertyKind::VehicleWheels;
    const char* noun = _wheels ? "wheel" : "seat";

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(4);

    auto heading = new QLabel(spec.label, this);
    heading->setObjectName("EditorInspectorLabel");
    layout->addWidget(heading);

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

    // the list, in the order the entity holds them
    _list = new QListWidget(_body);
    _list->setObjectName("EditorVehicleList");
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _list->setToolTip(_wheels ? "The wheels hung from the chassis; select one to edit it and to see it bright in the viewport"
                              : "The seats; select one to edit it and to see it bright in the viewport");
    connect(_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (_updating) {
            return;
        }
        _selected = row;
        markSelectedRow();
        showSelected();
        emit partChosen(_selected);
    });
    bodyLayout->addWidget(_list);

    auto buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(2);
    _addButton = new QToolButton(_body);
    _addButton->setText("+ ▾");
    _addButton->setObjectName("EditorTerrainLayerAdd");   // the text carries the menu's chevron, as the layers' button
    _addButton->setAutoRaise(true);
    _addButton->setPopupMode(QToolButton::InstantPopup);
    _addButton->setToolTip(QString("Add a %1: a copy of the selected one, or its mirror image across the vehicle's middle").arg(noun));
    auto addMenu = new QMenu(_addButton);
    addMenu->addAction(QString("Add %1").arg(_wheels ? "Wheel" : "Seat"), this, [this] { addPart(false); });
    addMenu->addAction("Add Mirrored Copy", this, [this] { addPart(true); });
    connect(addMenu, &QMenu::aboutToShow, this, [this] { _menuOpen = true; });
    connect(addMenu, &QMenu::aboutToHide, this, [this] { _menuOpen = false; });
    _addButton->setMenu(addMenu);
    _removeButton = new QToolButton(_body);
    _removeButton->setText("−");
    _removeButton->setAutoRaise(true);
    _removeButton->setToolTip(QString("Remove the selected %1").arg(noun));
    connect(_removeButton, &QToolButton::clicked, this, [this] { removePart(); });
    buttons->addWidget(_addButton);
    buttons->addWidget(_removeButton);
    buttons->addStretch(1);
    bodyLayout->addLayout(buttons);

    // the selected part's fields
    _fields = new QWidget(_body);
    auto grid = new QGridLayout(_fields);
    grid->setContentsMargins(0, 2, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);
    _name = new QLineEdit(_fields);
    _name->setToolTip(_wheels ? "The wheel's name: a wheel entity (a child with Type Wheel) names the wheel it shows by this"
                              : "The seat's name");
    connect(_name, &QLineEdit::editingFinished, this, [this] {
        bool modified = _name->isModified();
        _name->setModified(false);
        if (_updating || !modified || _selected < 0 || _selected >= _items.size()) {
            return;
        }
        QJsonObject part = _items[_selected].toObject();
        part["name"] = _name->text().trimmed();
        _items[_selected] = part;
        commitList();
    });
    auto nameLabel = new QLabel("Name", _fields);
    nameLabel->setObjectName("EditorInspectorLabel");
    grid->addWidget(nameLabel, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(_name, 0, 1);
    bodyLayout->addWidget(_fields);

    if (_wheels) {
        addVec3Field("center", "Center", "The suspension's top attachment point (fully compressed), in the chassis's local metres: Unity's center");
        addNumberField("radius", "Radius", PropertyKind::Float, 0.01, 10, 0.01, 3, "m", DEFAULT_RADIUS, "The tyre's radius");
        addNumberField("width", "Width", PropertyKind::Float, 0.01, 5, 0.01, 3, "m", DEFAULT_WIDTH, "The tyre's width");
        addNumberField("mass", "Mass", PropertyKind::Float, 0.1, 10000, 1, 1, "kg", DEFAULT_MASS, "The wheel's mass, for its spin inertia");
        addNumberField("suspensionDistance", "Suspension Distance", PropertyKind::Float, 0, 5, 0.01, 3, "m", DEFAULT_TRAVEL,
                       "How far the wheel travels down from Center when the suspension is fully extended");
        addNumberField("targetPosition", "Target Position", PropertyKind::Slider, 0.05, 0.95, 0.01, 2, "", DEFAULT_TARGET,
                       "Where the suspension rests along its travel: 0 fully extended, 1 fully compressed (Unity's)");
        addNumberField("spring", "Spring", PropertyKind::Float, 0, 10000000, 10, 0, "N/m", 0.0,
                       "The suspension spring's stiffness; 0 sets it from the vehicle's Suspension Frequency and Damping instead");
        addNumberField("damper", "Damper", PropertyKind::Float, 0, 1000000, 10, 0, "N s/m", 0.0,
                       "The suspension damper, with a Spring given");
        addNumberField("steer", "Steer", PropertyKind::Slider, -1, 1, 0.01, 2, "", 0.0,
                       "The fraction of the vehicle's Steering Lock this wheel turns: 1 a front wheel, 0 a fixed one, negative to counter-steer");
        {
            PropertySpec spec = fieldSpec("drive", "Drive", PropertyKind::Enum, 0, 0, 0, 0, "");
            spec.enumValues = QStringList { "auto:By the vehicle's Drive", "yes:Driven", "no:Not driven" };
            addField(spec, "Whether the engine turns this wheel; By the vehicle's Drive follows the Drive setting by axle",
                [](const QVariant& value, const QJsonValue&) -> QJsonValue {
                    QString choice = value.toString();
                    if (choice == "yes") {
                        return QJsonValue(true);
                    }
                    if (choice == "no") {
                        return QJsonValue(false);
                    }
                    return QJsonValue(QJsonValue::Undefined);
                },
                [](const QJsonObject& part) -> QVariant {
                    if (!part.contains("drive")) {
                        return QVariant("auto");
                    }
                    return QVariant(readBool(part, "drive", false) ? "yes" : "no");
                });
        }
        addNumberField("brake", "Brake", PropertyKind::Slider, 0, 1, 0.01, 2, "", 1.0,
                       "The fraction of the vehicle's Brake Torque this wheel takes");
        addNumberField("handBrake", "Hand Brake", PropertyKind::Slider, 0, 1, 0.01, 2, "", 0.0,
                       "The fraction of the vehicle's Hand Brake Torque this wheel takes: 1 on the rear wheels of a car");
        addBoolField("suspensionForcePoint", "Suspension Force Point", true,
                     "Suspension forces act at the wheel's neutral centre, which is steadier than the contact point");
    } else {
        addVec3Field("position", "Position", "Where the avatar's hips go, in the vehicle's local metres");
        addNumberField("yaw", "Yaw", PropertyKind::Float, -180, 180, 1, 1, "°", 0.0,
                       "The seat's facing about the vehicle's up: 0 faces the vehicle's forward");
        addBoolField("driver", "Driver", false, "The one seat whose occupant drives");
    }
    showSelected();
}

QWidget* VehiclePartsEditor::createLabel(QWidget* parent) {
    auto label = new QWidget(parent);
    label->setFixedSize(0, 0);
    return label;
}

PropertySpec VehiclePartsEditor::fieldSpec(const char* key, const char* label, PropertyKind kind, double min, double max,
                                           double step, int decimals, const char* unit) const {
    PropertySpec spec;
    spec.key = _spec.key + ":" + key;
    spec.label = label;
    spec.kind = kind;
    spec.section = _spec.section;
    spec.min = min;
    spec.max = max;
    spec.step = step;
    spec.decimals = decimals;
    spec.unit = unit;
    return spec;
}

void VehiclePartsEditor::addField(const PropertySpec& spec, const char* toolTip,
                                  std::function<QJsonValue(const QVariant&, const QJsonValue&)> toJson,
                                  std::function<QVariant(const QJsonObject&)> fromJson) {
    auto grid = static_cast<QGridLayout*>(_fields->layout());
    int row = grid->rowCount();
    Field field;
    field.key = spec.key.mid(spec.key.indexOf(':') + 1);
    field.editor = PropertyEditor::create(spec, _fields);
    field.editor->setToolTip(toolTip);
    field.toJson = toJson;
    field.fromJson = fromJson;
    auto label = field.editor->createLabel(_fields);
    label->setObjectName("EditorInspectorLabel");
    label->setToolTip(toolTip);
    grid->addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(field.editor, row, 1);
    _fieldList.append(field);
    wireField(_fieldList.size() - 1);
}

void VehiclePartsEditor::addNumberField(const char* key, const char* label, PropertyKind kind, double min, double max,
                                        double step, int decimals, const char* unit, double fallback, const char* toolTip) {
    QString name = key;
    addField(fieldSpec(key, label, kind, min, max, step, decimals, unit), toolTip,
        [](const QVariant& value, const QJsonValue&) { return QJsonValue(value.toDouble()); },
        [name, fallback](const QJsonObject& part) { return QVariant(readNumber(part, name, fallback)); });
}

void VehiclePartsEditor::addVec3Field(const char* key, const char* label, const char* toolTip) {
    QString name = key;
    addField(fieldSpec(key, label, PropertyKind::Vec3, -DBL_MAX, DBL_MAX, 0.01, 3, "m"), toolTip,
        [](const QVariant& value, const QJsonValue& was) { return writeVec3(was, value.value<QVector3D>()); },
        [name](const QJsonObject& part) {
            QVector3D v;
            if (!readVec3(part.value(name), v)) {
                v = QVector3D();
            }
            return QVariant(v);
        });
}

void VehiclePartsEditor::addBoolField(const char* key, const char* label, bool fallback, const char* toolTip) {
    QString name = key;
    addField(fieldSpec(key, label, PropertyKind::Bool, 0, 0, 0, 0, ""), toolTip,
        [](const QVariant& value, const QJsonValue&) { return QJsonValue(value.toBool()); },
        [name, fallback](const QJsonObject& part) { return QVariant(readBool(part, name, fallback)); });
}

void VehiclePartsEditor::wireField(int index) {
    PropertyEditor* editor = _fieldList[index].editor;
    // a scrub: the list as it was is the undo point, the values go out live, the end is one step; Escape brings it back
    connect(editor, &PropertyEditor::editBegan, this, [this] {
        _before = _items;
        emit editBegan();
    });
    connect(editor, &PropertyEditor::liveValue, this, [this, index](const QVariant& value) {
        if (!_updating && applyField(index, value)) {
            emit liveValue(QVariant(json()));
        }
    });
    connect(editor, &PropertyEditor::committed, this, [this, index](const QVariant& value) {
        if (!_updating && applyField(index, value)) {
            commitList();
        }
    });
    connect(editor, &PropertyEditor::editCancelled, this, [this] {
        _items = _before;
        showSelected();
        emit editCancelled();
    });
}

bool VehiclePartsEditor::applyField(int index, const QVariant& value) {
    if (_selected < 0 || _selected >= _items.size()) {
        return false;
    }
    const Field& field = _fieldList[index];
    QJsonObject part = _items[_selected].toObject();
    QJsonValue json = field.toJson(value, part.value(field.key));
    if (json.isUndefined()) {
        part.remove(field.key);
    } else {
        part[field.key] = json;
    }
    _items[_selected] = part;
    return true;
}

void VehiclePartsEditor::setValue(const QVariant& value, bool mixed) {
    QString text = mixed ? QString() : value.toString();
    if (mixed == _mixed && text == _shown) {
        return;   // nothing new: the list and its selection stay
    }
    _updating = true;
    _mixed = mixed;
    _shown = text;
    _items = QJsonArray();
    if (!mixed) {
        QJsonDocument document = QJsonDocument::fromJson(text.trimmed().toUtf8());
        if (document.isArray()) {
            for (const QJsonValue& item : document.array()) {
                if (item.isObject()) {
                    _items.append(item);
                }
            }
        }
    }
    _note->setText(mixed ? "Differs between the selected entities" : QString());
    _note->setVisible(mixed);
    _body->setVisible(!mixed);
    _updating = false;
    rebuildList();
}

bool VehiclePartsEditor::isEditing() const {
    if (_menuOpen) {
        return true;
    }
    for (const Field& field : _fieldList) {
        if (field.editor->isEditing()) {
            return true;
        }
    }
    return _name->hasFocus() && _name->isModified();
}

void VehiclePartsEditor::revertTyping() {
    for (const Field& field : _fieldList) {
        field.editor->revertTyping();
    }
    showSelected();
}

QVariant VehiclePartsEditor::currentValue() const {
    return QVariant(_mixed ? _shown : json());
}

QString VehiclePartsEditor::json() const {
    if (_items.isEmpty()) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument(_items).toJson(QJsonDocument::Compact));
}

void VehiclePartsEditor::rebuildList() {
    _updating = true;
    _list->clear();
    for (int i = 0; i < _items.size(); ++i) {
        QJsonObject part = _items[i].toObject();
        auto item = new QListWidgetItem(_list);
        item->setSizeHint(QSize(0, ROW_PIXELS));
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        _list->setItemWidget(item, makeRow(part, i));
    }
    if (_selected >= _items.size()) {
        _selected = _items.size() - 1;
    }
    if (_selected < 0 && !_items.isEmpty()) {
        _selected = 0;
    }
    _list->setCurrentRow(_selected);
    int rows = glm::clamp(int(_items.size()), 1, ROWS_SHOWN);   // size() is qsizetype on Qt 6
    _list->setFixedHeight(rows * ROW_PIXELS + 6);
    _list->setVerticalScrollBarPolicy(_items.size() > ROWS_SHOWN ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    _updating = false;
    markSelectedRow();
    showSelected();
    emit partChosen(_selected);
}

QWidget* VehiclePartsEditor::makeRow(const QJsonObject& part, int index) {
    auto row = new QWidget(_list);
    row->setObjectName("EditorVehicleRow");
    row->setAttribute(Qt::WA_StyledBackground);
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 1, 6, 1);
    layout->setSpacing(8);
    auto name = new QLabel(nameOf(part, index), row);
    name->setObjectName("EditorVehicleName");
    layout->addWidget(name);
    auto summary = new QLabel(summaryOf(part), row);
    summary->setObjectName("EditorVehicleSummary");
    layout->addWidget(summary, 1);
    return row;
}

QString VehiclePartsEditor::nameOf(const QJsonObject& part, int index) const {
    QString name = part.value("name").toString().trimmed();
    // the physics names an unnamed one by its index, the same way
    return name.isEmpty() ? QString("%1%2").arg(_wheels ? "wheel" : "seat").arg(index) : name;
}

QString VehiclePartsEditor::summaryOf(const QJsonObject& part) const {
    QStringList notes;
    if (_wheels) {
        notes << QString("r %1 m").arg(shortNumber(readNumber(part, "radius", DEFAULT_RADIUS)));
        double steer = readNumber(part, "steer", 0.0);
        if (steer > 0.0) {
            notes << "steers";
        } else if (steer < 0.0) {
            notes << "counter-steers";
        }
        if (part.contains("drive")) {
            notes << (readBool(part, "drive", false) ? "driven" : "free");
        }
        if (readNumber(part, "handBrake", 0.0) > 0.0) {
            notes << "hand brake";
        }
    } else {
        notes << (readBool(part, "driver", false) ? "driver" : "passenger");
        double yaw = readNumber(part, "yaw", 0.0);
        if (std::abs(yaw) > 0.01) {
            notes << QString("yaw %1°").arg(shortNumber(yaw));
        }
    }
    return notes.join(" · ");
}

void VehiclePartsEditor::markSelectedRow() {
    for (int i = 0; i < _list->count(); ++i) {
        auto row = _list->itemWidget(_list->item(i));
        if (!row) {
            continue;
        }
        bool selected = i == _selected;
        if (row->property("selected").toBool() != selected) {
            row->setProperty("selected", selected);
            for (QWidget* widget : row->findChildren<QWidget*>()) {
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
            }
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
    }
}

void VehiclePartsEditor::showSelected() {
    _updating = true;
    bool any = _selected >= 0 && _selected < _items.size();
    _fields->setVisible(any);
    _removeButton->setEnabled(any);
    if (any) {
        QJsonObject part = _items[_selected].toObject();
        _name->setText(part.value("name").toString());
        _name->setModified(false);
        for (const Field& field : _fieldList) {
            field.editor->setValue(field.fromJson(part), false);
        }
    }
    _updating = false;
}

void VehiclePartsEditor::commitList() {
    _shown = json();
    rebuildList();
    emit committed(QVariant(_shown));
}

void VehiclePartsEditor::addPart(bool mirrored) {
    QJsonObject part;
    bool fromSelected = _selected >= 0 && _selected < _items.size();
    if (fromSelected) {
        part = _items[_selected].toObject();
    } else if (_wheels) {
        part["center"] = QJsonArray { 0.0, 0.0, 0.0 };
        part["radius"] = DEFAULT_RADIUS;
        part["width"] = DEFAULT_WIDTH;
        part["suspensionDistance"] = DEFAULT_TRAVEL;
    } else {
        part["position"] = QJsonArray { 0.0, 0.0, 0.0 };
    }
    const QString pointKey = _wheels ? "center" : "position";
    QString name = part.value("name").toString();
    if (mirrored && fromSelected) {
        QVector3D point;
        if (readVec3(part.value(pointKey), point)) {
            point.setX(-point.x());
            part[pointKey] = writeVec3(part.value(pointKey), point);
        }
        QString mirror = mirroredName(name);
        name = mirror.isEmpty() ? (name.isEmpty() ? QString() : name + " (mirror)") : mirror;
    } else if (fromSelected) {
        name = name.isEmpty() ? QString() : name + " (copy)";
    }
    if (name.isEmpty()) {
        name = QString("%1%2").arg(_wheels ? "wheel" : "seat").arg(_items.size());
    }
    part["name"] = name;
    if (!_wheels && !mirrored) {
        part.remove("driver");   // one driver's seat at most: a copy is a passenger's
    } else if (!_wheels) {
        part.remove("driver");
    }
    _items.append(part);
    _selected = _items.size() - 1;
    commitList();
}

void VehiclePartsEditor::removePart() {
    if (_selected < 0 || _selected >= _items.size()) {
        return;
    }
    _items.removeAt(_selected);
    if (_selected >= _items.size()) {
        _selected = _items.size() - 1;
    }
    commitList();
}
