//
//  PropertyEditors.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PropertyEditors.h"

#include <cmath>
#include <functional>

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QFileDialog>

#include <NetworkAccessManager.h>

#include "ImpostorEditor.h"
#include "ScriptEditor.h"
#include "TerrainCapsEditor.h"
#include "TerrainLayersEditor.h"
#include "TrackPointsEditor.h"
#include "VehiclePartsEditor.h"
#include "UserDataEditor.h"
#include <QtCore/QRect>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QVector2D>
#include <QtGui/QVector3D>
#include <QtWidgets/QApplication>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

#include <glm/glm.hpp>

namespace {
    const int SCRUB_SLOP_PIXELS = 3;
    const double SCRUB_FINE = 0.1;      // Alt
    const double SCRUB_COARSE = 10.0;   // Shift
    const double HUGE_RANGE = 1.0e9;
    const QString MIXED = QStringLiteral("—");
    const QString UNSET = QStringLiteral("auto");   // a value not set, which follows another (a particle's Middle)
    const int SLIDER_MAX_STEPS = 10000;
    const int SLIDER_FIELD_PIXELS = 52;     // a Range's field beside its slider: room for "0.00"
    const int STEP_RUN_PAUSE_MS = 500;      // a run of wheel or arrow-key steps is over when none comes for this long

    // the Inspector's slider: it says when the mouse goes down on it, before the press moves the value, and when it
    // comes up; the wheel scrolls the Inspector instead of changing the value under the pointer
    class InspectorSlider : public QSlider {
    public:
        explicit InspectorSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {}
        std::function<void()> pressed;
        std::function<void()> released;
    protected:
        void mousePressEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && pressed) {
                pressed();
            }
            QSlider::mousePressEvent(event);
        }
        void mouseReleaseEvent(QMouseEvent* event) override {
            QSlider::mouseReleaseEvent(event);
            if (event->button() == Qt::LeftButton && released) {
                released();
            }
        }
        void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    };

    // Qt's combo box changes its choice as the wheel passes over it, and takes the keyboard from the wheel; here
    // only one that has the keyboard (clicked into) does, and the wheel scrolls the Inspector over any other
    class InspectorComboBox : public QComboBox {
    public:
        explicit InspectorComboBox(QWidget* parent) : QComboBox(parent) { setFocusPolicy(Qt::StrongFocus); }
    protected:
        void wheelEvent(QWheelEvent* event) override {
            if (!hasFocus()) {
                event->ignore();
                return;
            }
            QComboBox::wheelEvent(event);
        }
    };

    const char* const COLLISION_GROUPS[] = { "static", "dynamic", "kinematic", "myAvatar", "otherAvatar" };
    const char* const COLLISION_LABELS[] = { "Static", "Dynamic", "Kinematic", "My Avatar", "Other Avatars" };

    // the component labels for each vector kind
    QStringList componentLabels(PropertyKind kind) {
        switch (kind) {
            case PropertyKind::Vec2: return { "X", "Y" };
            case PropertyKind::Polar: return { "Elevation", "Azimuth" };
            case PropertyKind::Rect: return { "X", "Y", "W", "H" };
            default: return { "X", "Y", "Z" };
        }
    }

    // a number as people write it: up to four decimals, no trailing zeros
    QString formatNumber(double value) {
        QString text = QString::number(value, 'f', 4);
        if (text.contains('.')) {
            while (text.endsWith('0')) {
                text.chop(1);
            }
            if (text.endsWith('.')) {
                text.chop(1);
            }
        }
        return text == "-0" ? "0" : text;
    }

    // every number in a piece of text, in order: "1, 2, 3", "(1 2 3)" and {"x":1,"y":2,"z":3} all work
    QVector<double> numbersIn(const QString& text) {
        static const QRegularExpression NUMBER("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?");
        QVector<double> numbers;
        auto matches = NUMBER.globalMatch(text);
        while (matches.hasNext()) {
            numbers << matches.next().captured(0).toDouble();
        }
        return numbers;
    }
}

// --- clipboard values ---------------------------------------------------------------------------

QString PropertyEditor::valueToText(const PropertySpec& spec, const QVariant& value) {
    switch (spec.kind) {
        case PropertyKind::Vec2:
        case PropertyKind::Polar: {
            QVector2D v = value.value<QVector2D>();
            return formatNumber(v.x()) + ", " + formatNumber(v.y());
        }
        case PropertyKind::Vec3:
        case PropertyKind::Euler: {
            QVector3D v = value.value<QVector3D>();
            return formatNumber(v.x()) + ", " + formatNumber(v.y()) + ", " + formatNumber(v.z());
        }
        case PropertyKind::Rect: {
            QRect r = value.toRect();
            return QString("%1, %2, %3, %4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        }
        case PropertyKind::Color:
        case PropertyKind::FloatColor:
            return value.value<QColor>().name().toUpper();
        case PropertyKind::Bool:
            return value.toBool() ? "true" : "false";
        case PropertyKind::Int:
        case PropertyKind::Float:
        case PropertyKind::Slider:
        case PropertyKind::Degrees:
            return formatNumber(value.toDouble());
        default:
            return value.toString();
    }
}

bool PropertyEditor::textToValue(const PropertySpec& spec, const QString& text, QVariant& value) {
    QString trimmed = text.trimmed();
    QVector<double> numbers = numbersIn(trimmed);
    switch (spec.kind) {
        case PropertyKind::Vec2:
        case PropertyKind::Polar:
            if (numbers.size() < 2) {
                return false;
            }
            value = QVariant(QVector2D((float)numbers[0], (float)numbers[1]));
            return true;
        case PropertyKind::Vec3:
        case PropertyKind::Euler:
            if (numbers.size() < 3) {
                return false;
            }
            value = QVariant(QVector3D((float)numbers[0], (float)numbers[1], (float)numbers[2]));
            return true;
        case PropertyKind::Rect:
            if (numbers.size() < 4) {
                return false;
            }
            value = QVariant(QRect((int)numbers[0], (int)numbers[1], (int)numbers[2], (int)numbers[3]));
            return true;
        case PropertyKind::Color:
        case PropertyKind::FloatColor: {
            static const QRegularExpression HEX("^#?([0-9a-fA-F]{6})$");
            auto hex = HEX.match(trimmed);
            if (hex.hasMatch()) {
                value = QVariant(QColor("#" + hex.captured(1)));
                return true;
            }
            if (numbers.size() == 3) {
                value = QVariant(QColor(glm::clamp((int)numbers[0], 0, 255), glm::clamp((int)numbers[1], 0, 255),
                                        glm::clamp((int)numbers[2], 0, 255)));
                return true;
            }
            return false;
        }
        case PropertyKind::Bool: {
            QString word = trimmed.toLower();
            if (word == "true" || word == "yes" || word == "on" || word == "1") {
                value = QVariant(true);
                return true;
            }
            if (word == "false" || word == "no" || word == "off" || word == "0") {
                value = QVariant(false);
                return true;
            }
            return false;
        }
        case PropertyKind::Int:
        case PropertyKind::Float:
        case PropertyKind::Slider:
        case PropertyKind::Degrees: {
            bool ok = false;
            double number = trimmed.toDouble(&ok);
            if (!ok) {
                return false;
            }
            value = QVariant(glm::clamp(number, spec.min, spec.max));
            return true;
        }
        case PropertyKind::Enum:
            for (const auto& entry : spec.enumValues) {
                int colon = entry.indexOf(':');
                QString entryValue = colon < 0 ? entry : entry.left(colon);
                QString entryLabel = colon < 0 ? entry : entry.mid(colon + 1);
                if (trimmed.compare(entryValue, Qt::CaseInsensitive) == 0
                    || trimmed.compare(entryLabel, Qt::CaseInsensitive) == 0) {
                    value = QVariant(entryValue);
                    return true;
                }
            }
            return false;
        case PropertyKind::Json: {
            QJsonParseError error;
            QJsonDocument::fromJson(trimmed.toUtf8(), &error);
            if (!trimmed.isEmpty() && error.error != QJsonParseError::NoError) {
                return false;
            }
            value = QVariant(trimmed);
            return true;
        }
        default:
            value = QVariant(trimmed);
            return true;
    }
}

void PropertyEditor::showValueMenu(const QPoint& globalPos) {
    QMenu menu;
    QAction* copy = menu.addAction("Copy " + _spec.label);
    copy->setEnabled(!isMixed());
    QVariant pasted;
    bool canPaste = !_spec.readOnly && isEnabled()
        && textToValue(_spec, QApplication::clipboard()->text(), pasted);
    QAction* paste = menu.addAction("Paste " + _spec.label);
    paste->setEnabled(canPaste);
    QAction* chosen = menu.exec(globalPos);
    if (chosen == copy) {
        QApplication::clipboard()->setText(valueToText(_spec, currentValue()));
    } else if (chosen == paste && canPaste) {
        setValue(pasted, false);
        emit committed(pasted);
    }
}

// --- ScrubLabel ---------------------------------------------------------------------------------

ScrubLabel::ScrubLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
    setCursor(Qt::SizeHorCursor);
}

void ScrubLabel::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _pressed = true;
        _scrubbing = false;
        _pressX = _lastX = event->globalX();
    }
    QLabel::mousePressEvent(event);
}

void ScrubLabel::mouseMoveEvent(QMouseEvent* event) {
    if (!_pressed) {
        QLabel::mouseMoveEvent(event);
        return;
    }
    int x = event->globalX();
    if (!_scrubbing) {
        if (std::abs(x - _pressX) < SCRUB_SLOP_PIXELS) {
            return;
        }
        _scrubbing = true;
        _lastX = x;
        emit scrubBegan();
    }
    if (x != _lastX) {
        emit scrubbed(x - _lastX, event->modifiers());
        _lastX = x;
    }
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && _pressed) {
        _pressed = false;
        if (_scrubbing) {
            _scrubbing = false;
            emit scrubEnded();
        }
    }
    QLabel::mouseReleaseEvent(event);
}

// --- ScrubSpinBox -------------------------------------------------------------------------------

ScrubSpinBox::ScrubSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {
    setKeyboardTracking(false);
    // Qt's up/down buttons stay, drawn 0 px wide by editor.qss (ScrubSpinBox::up-button): on Qt 6.11 a spin box with a
    // style sheet and NoButtons lays its text field out 1 px wide, so every number drew blank (qa's 1.1.4 bug, 11-08).
    // Nothing can click them (0 px); the keys, the scrub and the field's own typing step as before
    setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    setAlignment(Qt::AlignRight);
    setFocusPolicy(Qt::StrongFocus);    // Qt's spin boxes also take the keyboard from a wheel passing over them
    _stepPause = new QTimer(this);
    _stepPause->setSingleShot(true);
    _stepPause->setInterval(STEP_RUN_PAUSE_MS);
    connect(_stepPause, &QTimer::timeout, this, &ScrubSpinBox::endStepRun);
}

void ScrubSpinBox::stepBy(int steps) {
    if (!_stepping) {
        _stepping = true;
        emit stepRunBegan();
    }
    QDoubleSpinBox::stepBy(steps);
    emit stepped();
    _stepPause->start();
}

void ScrubSpinBox::endStepRun() {
    if (_stepping) {
        _stepping = false;
        _stepPause->stop();
        emit stepRunEnded();
    }
}

void ScrubSpinBox::wheelEvent(QWheelEvent* event) {
    if (!hasFocus()) {
        event->ignore();    // on to the panel, which scrolls
        return;
    }
    QDoubleSpinBox::wheelEvent(event);
}

void ScrubSpinBox::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        endStepRun();
    }
    QDoubleSpinBox::keyPressEvent(event);
}

void ScrubSpinBox::focusOutEvent(QFocusEvent* event) {
    endStepRun();
    QDoubleSpinBox::focusOutEvent(event);
}

void ScrubSpinBox::showMixed() {
    showMark(MIXED);
}

void ScrubSpinBox::showMark(const QString& text) {
    _mixed = true;
    lineEdit()->setText(text);
}

QSize ScrubSpinBox::sizeHint() const {
    QSize size = QDoubleSpinBox::sizeHint();
    size.setWidth(72);
    return size;
}

QSize ScrubSpinBox::minimumSizeHint() const {
    QSize size = QDoubleSpinBox::minimumSizeHint();
    size.setWidth(44);
    return size;
}

void ScrubSpinBox::focusInEvent(QFocusEvent* event) {
    if (_mixed) {
        _mixed = false;
        lineEdit()->setText(textFromValue(value()));
    }
    QDoubleSpinBox::focusInEvent(event);
}

// --- PropertyEditor -----------------------------------------------------------------------------

PropertyEditor::PropertyEditor(const PropertySpec& spec, QWidget* parent) : QWidget(parent), _spec(spec) {
}

QWidget* PropertyEditor::createLabel(QWidget* parent) {
    // vector rows carry their unit on the label, since their fields are too small for a suffix
    bool vector = _spec.kind == PropertyKind::Vec2 || _spec.kind == PropertyKind::Vec3 || _spec.kind == PropertyKind::Euler
        || _spec.kind == PropertyKind::Polar || _spec.kind == PropertyKind::Rect;
    QString text = (vector && !_spec.unit.isEmpty()) ? _spec.label + " (" + _spec.unit + ")" : _spec.label;
    return new QLabel(text, parent);
}

PropertyEditor* PropertyEditor::create(const PropertySpec& spec, QWidget* parent) {
    switch (spec.kind) {
        case PropertyKind::Bool: return new BoolEditor(spec, parent);
        case PropertyKind::Int:
        case PropertyKind::Float:
        case PropertyKind::Degrees: {
            auto editor = new NumberEditor(spec, parent);
            editor->setMaximumWidth(130);   // a lone number does not need the whole row
            return editor;
        }
        case PropertyKind::Slider: return new NumberEditor(spec, parent);
        case PropertyKind::String: return new TextEditor(spec, parent);
        case PropertyKind::Url:
            // Script and Server Script: the link, and what the script is doing now with Reload under it
            return ScriptEditor::isFor(spec) ? (PropertyEditor*)new ScriptEditor(spec, parent) : new TextEditor(spec, parent);
        case PropertyKind::Json: return new JsonEditor(spec, parent);
        case PropertyKind::Image: return new ImageEditor(spec, parent);
        case PropertyKind::UserData: return new UserDataEditor(spec, parent);
        case PropertyKind::TerrainLayers: return new TerrainLayersEditor(spec, parent);
        case PropertyKind::TerrainCaps: return new TerrainCapsEditor(spec, parent);
        case PropertyKind::Impostor: return new ImpostorEditor(spec, parent);
        case PropertyKind::VehicleWheels:
        case PropertyKind::VehicleSeats: return new VehiclePartsEditor(spec, parent);
        case PropertyKind::TrackPoints: return new TrackPointsEditor(spec, parent);
        case PropertyKind::Color:
        case PropertyKind::FloatColor: return new ColorEditor(spec, parent);
        case PropertyKind::Vec2:
        case PropertyKind::Vec3:
        case PropertyKind::Euler:
        case PropertyKind::Polar:
        case PropertyKind::Rect: return new VectorEditor(spec, parent);
        case PropertyKind::Enum: return new EnumEditor(spec, parent);
        case PropertyKind::CollisionMask: return new CollisionMaskEditor(spec, parent);
    }
    return new TextEditor(spec, parent);
}

// --- NumberEditor -------------------------------------------------------------------------------

NumberEditor::NumberEditor(const PropertySpec& spec, QWidget* parent, bool component) : PropertyEditor(spec, parent), _component(component) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _spin = new ScrubSpinBox(this);
    _spin->setDecimals(spec.kind == PropertyKind::Int ? 0 : spec.decimals);
    _spin->setRange(spec.min <= -HUGE_RANGE ? -HUGE_RANGE : spec.min, spec.max >= HUGE_RANGE ? HUGE_RANGE : spec.max);
    _spin->setSingleStep(spec.step);
    if (!spec.unit.isEmpty() && !_component) {
        _spin->setSuffix(" " + spec.unit);
    }
    _spin->setEnabled(!spec.readOnly);
    _spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (spec.kind == PropertyKind::Slider) {
        // one step of the slider is one step of the field's last decimal, so the two always agree
        auto slider = new InspectorSlider(this);
        _slider = slider;
        double span = spec.max - spec.min;
        _slider->setRange(0, qBound(1, (int)std::lround(span * std::pow(10.0, spec.decimals)), SLIDER_MAX_STEPS));
        _slider->setEnabled(!spec.readOnly);
        layout->addWidget(_slider, 1);
        _spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _spin->setFixedWidth(SLIDER_FIELD_PIXELS);
        // the edit begins as the mouse goes down, before the press moves the value (a click on the groove jumps
        // there); dragging previews live, and the release is the one undo step, or none if the value is where it was
        slider->pressed = [this] {
            _scrubbing = true;
            _scrubStart = _spin->value();
            emit editBegan();
        };
        slider->released = [this] {
            if (!_scrubbing) {
                return;
            }
            _scrubbing = false;
            if (_spin->value() == _scrubStart) {
                emit editCancelled();
            } else {
                emit committed(QVariant(_spin->value()));
            }
        };
        connect(_slider, &QSlider::valueChanged, this, [this](int position) {
            _updating = true;
            _spin->setValue(_spec.min + (_spec.max - _spec.min) * position / _slider->maximum());
            _updating = false;
            if (_scrubbing) {
                emit liveValue(QVariant(_spin->value()));
            } else {
                emit committed(QVariant(_spin->value()));   // the arrow keys
            }
        });
    }
    layout->addWidget(_spin);
    connect(_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        showOnSlider();
        if (!_updating && !_scrubbing) {
            emit committed(QVariant(value));
        }
    });
    // a run of wheel or arrow-key steps in the field is one edit, live while it goes on, as a scrub is
    connect(_spin, &ScrubSpinBox::stepRunBegan, this, [this] {
        _scrubbing = true;
        _scrubStart = _spin->value();
        emit editBegan();
    });
    connect(_spin, &ScrubSpinBox::stepped, this, [this] {
        if (_scrubbing) {
            emit liveValue(QVariant(_spin->value()));
        }
    });
    connect(_spin, &ScrubSpinBox::stepRunEnded, this, [this] {
        if (!_scrubbing) {
            return;
        }
        _scrubbing = false;
        if (_spin->value() == _scrubStart) {
            emit editCancelled();
        } else {
            emit committed(QVariant(_spin->value()));
        }
    });
}

void NumberEditor::showOnSlider() {
    if (!_slider) {
        return;
    }
    double span = _spec.max - _spec.min;
    QSignalBlocker blocker(_slider);
    _slider->setValue(span > 0.0 ? (int)std::lround((_spin->value() - _spec.min) / span * _slider->maximum()) : 0);
}

QWidget* NumberEditor::createLabel(QWidget* parent) {
    auto label = new ScrubLabel(_spec.label, parent);
    attachScrubLabel(label);
    return label;
}

void NumberEditor::attachScrubLabel(ScrubLabel* label) {
    connect(label, &ScrubLabel::scrubBegan, this, [this] {
        // a read-only field (the world is not editable, or the property never is) does not scrub
        if (!isEnabled() || !_spin->isEnabled()) {
            return;
        }
        _scrubbing = true;
        _scrubStart = _spin->value();
        _scrubAccumulated = _scrubStart;
        emit editBegan();
    });
    connect(label, &ScrubLabel::scrubbed, this, [this](int deltaPixels, Qt::KeyboardModifiers modifiers) {
        if (!_scrubbing) {
            return;
        }
        double step = _spec.step;
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            step *= SCRUB_COARSE;
        } else if (modifiers.testFlag(Qt::AltModifier)) {
            step *= SCRUB_FINE;
        }
        // held at the field's limits, so dragging back from past one moves the value at once, as in Unity
        _scrubAccumulated = qBound(_spin->minimum(), _scrubAccumulated + deltaPixels * step, _spin->maximum());
        _updating = true;
        _spin->setValue(_scrubAccumulated);
        _updating = false;
        emit liveValue(QVariant(_spin->value()));
    });
    connect(label, &ScrubLabel::scrubEnded, this, [this] {
        if (!_scrubbing) {
            return;
        }
        _scrubbing = false;
        emit committed(QVariant(_spin->value()));
    });
}

void NumberEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    // not set (a particle's Start or Finish, which then follows its Middle): the field says "auto", and typing or
    // dragging starts from 0, not from the end of the range an unset value clamped to (360, or a 32768 m particle that
    // filled the view: GitHub #49)
    const double number = value.toDouble();
    const bool unset = std::isnan(number);
    _spin->setValue(unset ? qBound(_spin->minimum(), 0.0, _spin->maximum()) : number);
    showOnSlider();
    if (!_spin->hasFocus()) {   // clicking in replaced the mark with a value to edit
        if (mixed) {
            _spin->showMixed();
        } else if (unset) {
            _spin->showMark(UNSET);
        }
    }
    _updating = false;
}

bool NumberEditor::isEditing() const {
    // a field that has only been clicked into still shows changes made elsewhere (a Scale edit
    // committed by clicking into Dimensions updates Dimensions); typing in it holds it
    return _scrubbing || (_spin->hasFocus() && _spin->isTyping());
}

void NumberEditor::revertTyping() {
    _spin->setValue(_spin->value());   // the text goes back to the value, which is unchanged until Enter
}

// --- VectorEditor -------------------------------------------------------------------------------

VectorEditor::VectorEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    QStringList labels = componentLabels(spec.kind);
    for (const auto& text : labels) {
        PropertySpec component = spec;
        component.label = text;
        component.kind = spec.kind == PropertyKind::Rect ? PropertyKind::Int : PropertyKind::Float;
        component.decimals = std::min(spec.decimals, 3);
        auto editor = new NumberEditor(component, this, true);
        auto label = new ScrubLabel(text, this);
        label->setObjectName("EditorComponentLabel");
        editor->attachScrubLabel(label);
        layout->addWidget(label);
        layout->addWidget(editor, 1);
        connect(editor, &PropertyEditor::editBegan, this, &PropertyEditor::editBegan);
        connect(editor, &PropertyEditor::editCancelled, this, &PropertyEditor::editCancelled);
        connect(editor, &PropertyEditor::liveValue, this, [this] { emit liveValue(currentValue()); });
        connect(editor, &PropertyEditor::committed, this, [this] { emit committed(currentValue()); });
        _components << editor;
    }
}

QVariant VectorEditor::currentValue() const {
    switch (_spec.kind) {
        case PropertyKind::Vec2:
        case PropertyKind::Polar:
            return QVariant(QVector2D((float)_components[0]->value(), (float)_components[1]->value()));
        case PropertyKind::Rect:
            return QVariant(QRect((int)_components[0]->value(), (int)_components[1]->value(),
                                  (int)_components[2]->value(), (int)_components[3]->value()));
        default:
            return QVariant(QVector3D((float)_components[0]->value(), (float)_components[1]->value(), (float)_components[2]->value()));
    }
}

void VectorEditor::setValue(const QVariant& value, bool mixed) {
    QVector<double> parts;
    switch (_spec.kind) {
        case PropertyKind::Vec2:
        case PropertyKind::Polar: {
            QVector2D v = value.value<QVector2D>();
            parts = { v.x(), v.y() };
            break;
        }
        case PropertyKind::Rect: {
            QRect r = value.toRect();
            parts = { (double)r.x(), (double)r.y(), (double)r.width(), (double)r.height() };
            break;
        }
        default: {
            QVector3D v = value.value<QVector3D>();
            parts = { v.x(), v.y(), v.z() };
            break;
        }
    }
    for (int i = 0; i < _components.size() && i < parts.size(); ++i) {
        if (!_components[i]->isEditing()) {
            _components[i]->setValue(QVariant(parts[i]), mixed);
        }
    }
}

bool VectorEditor::isEditing() const {
    for (auto component : _components) {
        if (component->isEditing()) {
            return true;
        }
    }
    return false;
}

bool VectorEditor::isMixed() const {
    for (auto component : _components) {
        if (component->isMixed()) {
            return true;
        }
    }
    return false;
}

// --- BoolEditor ---------------------------------------------------------------------------------

BoolEditor::BoolEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _check = new QCheckBox(this);
    _check->setEnabled(!spec.readOnly);
    layout->addWidget(_check);
    layout->addStretch(1);
    connect(_check, &QCheckBox::clicked, this, [this](bool checked) {
        _check->setTristate(false);
        emit committed(QVariant(checked));
    });
}

void BoolEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    if (mixed) {
        _check->setTristate(true);
        _check->setCheckState(Qt::PartiallyChecked);
    } else {
        _check->setTristate(false);
        _check->setChecked(value.toBool());
    }
    _updating = false;
}

// --- TextEditor ---------------------------------------------------------------------------------

namespace {
    // the link in what is dragged, if it is a web address: a file from the Assets panel, or from a browser
    QUrl droppedWebAddress(const QMimeData* mime) {
        for (const auto& url : mime->urls()) {
            if (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) {
                return url;
            }
        }
        return QUrl();
    }
}

TextEditor::TextEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _edit = new QLineEdit(this);
    _edit->setReadOnly(spec.readOnly);
    if (spec.kind == PropertyKind::Url) {
        _edit->setPlaceholderText("URL");
        // the field takes the link itself, rather than letting the line edit paste it as typing
        _edit->setAcceptDrops(false);
        setAcceptDrops(!spec.readOnly);
    }
    layout->addWidget(_edit);
    connect(_edit, &QLineEdit::editingFinished, this, [this] {
        // committed, the field takes refreshes again (an undo of this very edit) though it keeps the keyboard
        _edit->setModified(false);
        if (!_updating && _edit->text() != _shown) {
            _shown = _edit->text();
            emit committed(QVariant(_shown));
        }
    });
}

void TextEditor::revertTyping() {
    _edit->setText(_shown);
    _edit->setModified(false);
}

void TextEditor::dragEnterEvent(QDragEnterEvent* event) {
    if (!_spec.readOnly && _spec.kind == PropertyKind::Url && !droppedWebAddress(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void TextEditor::dropEvent(QDropEvent* event) {
    QUrl url = droppedWebAddress(event->mimeData());
    if (url.isEmpty()) {
        return;
    }
    _shown = url.toString();
    _edit->setText(_shown);
    _edit->setModified(false);
    emit committed(QVariant(_shown));
    event->acceptProposedAction();
}

void TextEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    QString text = mixed ? QString() : value.toString();
    // the Inspector refreshes often: a field that shows this already is left alone, or a selection made in it
    // (to copy the ID) would go with every tick, since setting the text, even the same, drops it
    if (mixed != _mixed || text != _shown || _edit->text() != text) {
        _mixed = mixed;
        _shown = text;
        _edit->setText(text);
        _edit->setPlaceholderText(mixed ? MIXED : (_spec.kind == PropertyKind::Url ? "URL" : QString()));
    }
    _updating = false;
}

bool TextEditor::isEditing() const {
    // typing holds the field; a field only clicked into, or committed with Enter, still shows changes made elsewhere
    return _edit->hasFocus() && _edit->isModified();
}

// --- ImageEditor --------------------------------------------------------------------------------

namespace {
    const int THUMBNAIL_PIXELS = 28;
    const int HOLD_MS = 400;   // the left button held this long in a picture field opens its menu
    const QStringList PICTURE_SUFFIXES { "png", "jpg", "jpeg", "webp", "bmp", "gif", "tga", "tif", "tiff", "ktx", "exr", "hdr" };

    bool isPictureFile(const QString& path) {
        return PICTURE_SUFFIXES.contains(QFileInfo(path).suffix().toLower());
    }

    bool isWebAddress(const QUrl& url) {
        return url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https");
    }

    // a web address that may name a picture: its file has a picture's suffix, or none (some links have none); a
    // model, a sound or a script dragged from the Assets panel is not one
    bool mayBePicture(const QUrl& url) {
        const QString name = url.fileName();
        const int dot = name.lastIndexOf('.');
        return dot < 0 || PICTURE_SUFFIXES.contains(name.mid(dot + 1).toLower());
    }

    // the picture in what is dragged: a picture file on this computer, or a web address that may be one
    QUrl draggedPicture(const QMimeData* mime) {
        if (!mime->hasUrls()) {
            return QUrl();
        }
        for (const auto& url : mime->urls()) {
            if ((url.isLocalFile() && isPictureFile(url.toLocalFile())) || (isWebAddress(url) && mayBePicture(url))) {
                return url;
            }
        }
        return QUrl();
    }
}

ImageEditor::ImageEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    setAcceptDrops(!spec.readOnly);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    _thumbnail = new QLabel(this);
    _thumbnail->setObjectName("EditorImageThumbnail");
    _thumbnail->setFixedSize(THUMBNAIL_PIXELS, THUMBNAIL_PIXELS);
    _thumbnail->setAlignment(Qt::AlignCenter);
    _thumbnail->setFrameShape(QFrame::StyledPanel);
    if (!spec.readOnly) {
        _thumbnail->setCursor(Qt::PointingHandCursor);
        _thumbnail->setToolTip("Click to choose an image on this computer, or to clear the field");
        _thumbnail->installEventFilter(this);
    }
    layout->addWidget(_thumbnail);
    _url = new QLineEdit(this);
    _url->setPlaceholderText("None");
    _url->setReadOnly(spec.readOnly);
    // the field takes a dropped picture itself: the line edit would paste its link in as typing, which commits nothing
    _url->setAcceptDrops(false);
    if (!spec.readOnly) {
        _url->setToolTip("An image by its link. Drag one here from the Assets panel or Finder, or hold the left button down "
                         "in the field to choose one on this computer");
        _url->installEventFilter(this);
    }
    layout->addWidget(_url, 1);
    _holdTimer = new QTimer(this);
    _holdTimer->setSingleShot(true);
    _holdTimer->setInterval(HOLD_MS);
    connect(_holdTimer, &QTimer::timeout, this, [this] { showMenu(); });
    auto clear = new QToolButton(this);
    clear->setText(QStringLiteral("×"));
    clear->setToolTip("No image");
    clear->setEnabled(!spec.readOnly);
    layout->addWidget(clear);

    connect(_url, &QLineEdit::editingFinished, this, [this] {
        _url->setModified(false);
        QString url = _url->text().trimmed();
        if (!_updating && url != _shown) {
            _shown = url;
            showThumbnail(url);
            emit committed(QVariant(url));
        }
    });
    connect(clear, &QToolButton::clicked, this, [this] { clearPicture(); });
}

bool ImageEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _thumbnail && event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        showMenu();
        return true;
    }
    if (watched == _url) {
        switch (event->type()) {
            case QEvent::MouseButtonPress: {
                auto mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    _holdPos = mouse->pos();
                    _holdTimer->start();   // a click still places the cursor; held still, it opens the menu
                }
                break;
            }
            case QEvent::MouseMove:
                if (_holdTimer->isActive() && (static_cast<QMouseEvent*>(event)->pos() - _holdPos).manhattanLength()
                        > QApplication::startDragDistance()) {
                    _holdTimer->stop();   // a drag that selects text
                }
                break;
            case QEvent::MouseButtonRelease:
            case QEvent::FocusOut:
                _holdTimer->stop();
                break;
            default:
                break;
        }
    }
    return PropertyEditor::eventFilter(watched, event);
}

void ImageEditor::showMenu() {
    _holdTimer->stop();
    if (_spec.readOnly) {
        return;
    }
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction* choose = menu.addAction("Choose File…");
    choose->setToolTip("An image on this computer, uploaded to your files");
    QAction* clear = menu.addAction("Clear");
    clear->setEnabled(!_shown.isEmpty());
    // the choice is acted on once the menu has gone, so that the file dialog does not open inside it
    QAction* chosen = menu.exec(_url->mapToGlobal(QPoint(0, _url->height())));
    if (chosen == choose) {
        chooseFile();
    } else if (chosen == clear) {
        clearPicture();
    }
}

void ImageEditor::chooseFile() {
    QString path = QFileDialog::getOpenFileName(this, "Choose an Image", QDir::homePath(),
        "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga *.tif *.tiff *.ktx *.exr *.hdr)");
    if (!path.isEmpty()) {
        takeFile(path);
    }
}

void ImageEditor::clearPicture() {
    if (!_shown.isEmpty()) {
        _shown.clear();
        _url->clear();
        showThumbnail(QString());
        emit committed(QVariant(QString()));
    }
}

void ImageEditor::revertTyping() {
    _url->setText(_shown);
    _url->setModified(false);
}

void ImageEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    _mixed = mixed;
    QString url = mixed ? QString() : value.toString();
    _url->setPlaceholderText(mixed ? MIXED : QStringLiteral("None"));
    // the Inspector refreshes often: the thumbnail is fetched again only for a new picture
    if (url != _shown || _url->text() != url) {
        _shown = url;
        _url->setText(url);
        showThumbnail(url);
    }
    _updating = false;
}

bool ImageEditor::isEditing() const {
    return _url->hasFocus() && _url->isModified();
}

void ImageEditor::takeFile(const QString& localPath) {
    if (isPictureFile(localPath)) {
        emit uploadRequested(localPath);   // the toolbar shows its progress; its URL then comes back as the value
    }
}

void ImageEditor::showThumbnail(const QString& url) {
    if (_reply) {
        QPointer<QNetworkReply> previous = _reply;
        _reply = nullptr;   // so that its end, which aborting brings at once, is not taken for this picture's
        previous->abort();
    }
    _thumbnail->setPixmap(QPixmap());
    _thumbnail->setText(QString());
    if (url.isEmpty()) {
        return;
    }
    QUrl address(url);
    if (address.isLocalFile()) {
        QPixmap pixmap(address.toLocalFile());
        if (pixmap.isNull()) {
            _thumbnail->setText(QStringLiteral("?"));
        } else {
            _thumbnail->setPixmap(pixmap.scaled(THUMBNAIL_PIXELS - 2, THUMBNAIL_PIXELS - 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        return;
    }
    if (!isWebAddress(address)) {
        _thumbnail->setText(QStringLiteral("?"));   // an address the thumbnail can't fetch (atp:, a typo)
        return;
    }
    _thumbnail->setText(QStringLiteral("…"));
    QNetworkRequest request(address);
    _reply = NetworkAccessManager::getInstance().get(request);
    QPointer<QNetworkReply> reply = _reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (!reply) {
            return;
        }
        reply->deleteLater();
        if (reply != _reply) {
            return;   // another picture has taken its place
        }
        _reply = nullptr;
        QPixmap pixmap;
        if (reply->error() == QNetworkReply::NoError && pixmap.loadFromData(reply->readAll())) {
            _thumbnail->setText(QString());
            _thumbnail->setPixmap(pixmap.scaled(THUMBNAIL_PIXELS - 2, THUMBNAIL_PIXELS - 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            _thumbnail->setText(QStringLiteral("?"));
        }
    });
}

void ImageEditor::dragEnterEvent(QDragEnterEvent* event) {
    if (!_spec.readOnly && !draggedPicture(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void ImageEditor::dropEvent(QDropEvent* event) {
    const QUrl url = draggedPicture(event->mimeData());
    if (_spec.readOnly || url.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    if (url.isLocalFile()) {
        takeFile(url.toLocalFile());
        return;
    }
    _shown = url.toString();
    _url->setText(_shown);
    _url->setModified(false);
    showThumbnail(_shown);
    emit committed(QVariant(_shown));
}

// --- JsonEditor ---------------------------------------------------------------------------------

JsonEditor::JsonEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _edit = new QPlainTextEdit(this);
    _edit->setMaximumHeight(110);
    _edit->setTabStopDistance(20);
    _edit->setReadOnly(spec.readOnly);
    _edit->installEventFilter(this);
    layout->addWidget(_edit);
}

bool JsonEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _edit && event->type() == QEvent::FocusOut) {
        commitIfChanged();
    }
    return PropertyEditor::eventFilter(watched, event);
}

void JsonEditor::commitIfChanged() {
    QString text = _edit->toPlainText();
    if (_updating || text == _shown) {
        return;
    }
    QString trimmed = text.trimmed();
    if (!trimmed.isEmpty()) {
        QJsonParseError error;
        QJsonDocument::fromJson(trimmed.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            _edit->setStyleSheet("border: 1px solid #c0392b;");
            return;   // keep the text so it can be fixed; nothing is sent
        }
    }
    _edit->setStyleSheet(QString());
    _shown = text;
    _edit->document()->setModified(false);   // committed: refreshes reach the box again
    emit committed(QVariant(text));
}

void JsonEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    QString text = mixed ? QString() : value.toString();
    // left alone while it shows this already (see TextEditor::setValue)
    if (mixed != _mixed || text != _shown || _edit->toPlainText() != text) {
        _mixed = mixed;
        _shown = text;
        _edit->setPlainText(text);
        _edit->document()->setModified(false);
        _edit->setPlaceholderText(mixed ? MIXED : QString());
        _edit->setStyleSheet(QString());
    }
    _updating = false;
}

bool JsonEditor::isEditing() const {
    // text being typed or fixed holds the box; a box only clicked into still shows changes made elsewhere
    return _edit->hasFocus() && _edit->document()->isModified();
}

// --- ColorEditor --------------------------------------------------------------------------------

ColorEditor::ColorEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _button = new QToolButton(this);
    _button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _button->setAutoRaise(false);
    _button->setEnabled(!spec.readOnly);
    _button->setMinimumWidth(120);
    layout->addWidget(_button);
    layout->addStretch(1);
    connect(_button, &QToolButton::clicked, this, &ColorEditor::openPicker);
    updateSwatch();
}

void ColorEditor::openPicker() {
    if (_dialog) {
        _dialog->raise();
        _dialog->activateWindow();
        return;
    }
    // Qt's own picker rather than macOS's colour panel: the native panel's OK button did not
    // register on this macOS, so every pick came back as a cancel and nothing changed
    auto dialog = new QColorDialog(_mixed ? QColor(Qt::white) : _color, this);
    dialog->setOption(QColorDialog::DontUseNativeDialog, true);
    dialog->setWindowTitle(_spec.label);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // the selection cannot change under an open picker
    dialog->setWindowModality(Qt::ApplicationModal);
    _dialog = dialog;
    _original = _color;
    _originalMixed = _mixed;
    emit editBegan();
    connect(dialog, &QColorDialog::currentColorChanged, this, [this](const QColor& color) {
        if (!color.isValid()) {
            return;
        }
        _color = color;
        _mixed = false;
        updateSwatch();
        emit liveValue(QVariant(color));
    });
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (result == QDialog::Accepted) {
            QColor chosen = dialog->selectedColor();
            if (chosen.isValid()) {
                _color = chosen;
                _mixed = false;
            }
            updateSwatch();
            emit committed(QVariant(_color));
        } else {
            _color = _original;
            _mixed = _originalMixed;
            updateSwatch();
            emit editCancelled();
        }
    });
    dialog->show();
}

void ColorEditor::updateSwatch() {
    QPixmap swatch(18, 18);
    swatch.fill(_mixed ? QColor("#666666") : _color);
    QPainter painter(&swatch);
    painter.setPen(QColor("#1b1b1b"));
    painter.drawRect(0, 0, 17, 17);
    painter.end();
    _button->setIcon(QIcon(swatch));
    _button->setText(_mixed ? MIXED : _color.name().toUpper());
}

void ColorEditor::setValue(const QVariant& value, bool mixed) {
    if (_dialog) {
        return;   // the picker is open: what it shows is the truth until it closes
    }
    _color = value.value<QColor>();
    _mixed = mixed;
    updateSwatch();
}

// --- EnumEditor ---------------------------------------------------------------------------------

EnumEditor::EnumEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _combo = new InspectorComboBox(this);
    _combo->setEnabled(!spec.readOnly);
    for (const auto& entry : spec.enumValues) {
        int colon = entry.indexOf(':');
        QString value = colon < 0 ? entry : entry.left(colon);
        QString label = colon < 0 ? entry : entry.mid(colon + 1);
        _combo->addItem(label, value);
    }
    layout->addWidget(_combo, 1);
    connect(_combo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (!_updating && index >= 0) {
            emit committed(_combo->itemData(index));
        }
    });
}

void EnumEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    if (mixed) {
        _combo->setCurrentIndex(-1);
    } else {
        QString text = value.toString();
        int index = _combo->findData(text);
        if (index < 0) {
            index = _combo->findData(text, Qt::UserRole, Qt::MatchFixedString);   // case-blind
        }
        _combo->setCurrentIndex(index);
    }
    _updating = false;
}

// --- CollisionMaskEditor ------------------------------------------------------------------------

CollisionMaskEditor::CollisionMaskEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    for (int i = 0; i < 5; ++i) {
        auto check = new QCheckBox(COLLISION_LABELS[i], this);
        layout->addWidget(check);
        connect(check, &QCheckBox::clicked, this, [this] { emit committed(QVariant(currentMask())); });
        _checks << check;
    }
}

QString CollisionMaskEditor::currentMask() const {
    QStringList groups;
    for (int i = 0; i < _checks.size(); ++i) {
        if (_checks[i]->isChecked()) {
            groups << COLLISION_GROUPS[i];
        }
    }
    return groups.join(",");
}

void CollisionMaskEditor::setValue(const QVariant& value, bool mixed) {
    _updating = true;
    _mixed = mixed;
    QStringList groups = value.toString().split(',', Qt::SkipEmptyParts);
    for (int i = 0; i < _checks.size(); ++i) {
        _checks[i]->setTristate(mixed);
        if (mixed) {
            _checks[i]->setCheckState(Qt::PartiallyChecked);
        } else {
            _checks[i]->setChecked(groups.contains(COLLISION_GROUPS[i]));
        }
    }
    _updating = false;
}
