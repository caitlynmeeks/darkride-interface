//
//  TerrainToolBar.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TerrainToolBar.h"

#include <cmath>
#include <memory>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QAction>   // QtWidgets in Qt 5, QtGui in Qt 6
#include <QActionGroup>   // QtWidgets in Qt 5, QtGui in Qt 6
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>

#include <RenderableTerrainEntityItem.h>

#include "PropertyEditors.h"

namespace {
    const float MIN_RADIUS = 0.1f;      // metres
    const float MAX_RADIUS = 100.0f;
    const char* BRUSHES_FOLDER = ":/terrain/brushes/";
    const char* BRUSHES_LIST = "brushes.json";
    const char* ROUND_THUMBNAIL = "round.png";
    const int THUMBNAIL_PIXELS = 18;
    const int ERASE_FLAG = 0x100;   // in a mode action's data, above the mode: the Paint mode's eraser

    // The Layer field: its list is filled afresh as it opens, so it names the layers of whatever terrain is
    // selected or under the pointer now.
    class LayerCombo : public QComboBox {
    public:
        explicit LayerCombo(std::function<void()> refresh, QWidget* parent) : QComboBox(parent), _refresh(std::move(refresh)) {}
        void showPopup() override {
            _refresh();
            QComboBox::showPopup();
        }
    private:
        std::function<void()> _refresh;
    };

    QIcon thumbnailIcon(const QString& path) {
        QPixmap picture(path);
        if (picture.isNull()) {
            return QIcon();
        }
        return QIcon(picture.scaled(THUMBNAIL_PIXELS, THUMBNAIL_PIXELS, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

TerrainToolBar::TerrainToolBar(QWidget* parent) : QToolBar("Terrain", parent) {
    setObjectName("EditorTerrainBar");
    setMovable(false);
    setFloatable(false);
    setAllowedAreas(Qt::TopToolBarArea);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setIconSize(QSize(THUMBNAIL_PIXELS, THUMBNAIL_PIXELS));   // the Shape button's thumbnail

    _modes = new QActionGroup(this);
    _modes->setExclusive(true);
    addMode("Raise", "Raise the ground under the brush; hold Shift to lower it", terrain::SculptMode::Raise)->setChecked(true);
    addMode("Lower", "Lower the ground under the brush; hold Shift to raise it", terrain::SculptMode::Lower);
    addMode("Smooth", "Smooth the ground under the brush", terrain::SculptMode::Smooth);
    addMode("Flatten", "Pull the ground to the height where the stroke began", terrain::SculptMode::Flatten);
    addMode("Dig", "Cut the brush's sphere into the ground at the Strength pace, boring on while held; hold Shift to fill instead",
            terrain::SculptMode::Dig);
    addMode("Fill", "Add the brush's sphere to the ground at the Strength pace, building on while held; hold Shift to dig instead",
            terrain::SculptMode::Fill);
    addSeparator();
    addMode("Paint", "Paint the layer onto the ground where it faces up, over the layers under it; hold Shift to erase it instead",
            terrain::SculptMode::Paint);
    addMode("Erase", "Erase the layer's paint under the brush, showing the layers under it (the voxels' own material where "
            "nothing is painted); hold Shift to paint instead", terrain::SculptMode::Paint, true);
    addMode("Paint Voxels", "Set the material of the ground voxels under the brush: what cliffs, caves and a dig show; hold Shift, "
            "or tick Erase, to set the bottom layer's material instead, the ground's own", terrain::SculptMode::PaintVoxels);
    addSeparator();

    auto addField = [this](const QString& label, QAbstractSpinBox* box, double scrubStep, bool proportional, const QString& toolTip) {
        auto name = new ScrubLabel(label, this);
        name->setObjectName("EditorFieldName");
        name->setToolTip("Drag sideways to change it (Shift: faster, Alt: finer)");
        addWidget(name);
        box->setToolTip(toolTip);
        box->setFocusPolicy(Qt::ClickFocus);   // typing elsewhere never lands here; a click does
        addWidget(box);
        attachScrub(name, box, scrubStep, proportional);
    };
    _size = new QDoubleSpinBox(this);
    _size->setRange(MIN_RADIUS, MAX_RADIUS);
    _size->setDecimals(1);
    _size->setSingleStep(0.5);
    _size->setSuffix(" m");
    _size->setValue(3.0);
    addField("Size", _size, 0.01, true, "The brush's radius ([ and ] change it)");

    _strength = new QDoubleSpinBox(this);
    _strength->setRange(1.0, 100.0);
    _strength->setDecimals(0);
    _strength->setSingleStep(5.0);
    _strength->setSuffix(" %");
    _strength->setValue(50.0);
    addField("Strength", _strength, 0.5, false, "How fast Raise, Lower, Smooth and Flatten work and how fast Dig and Fill go in, and how "
             "much Paint and Paint Voxels lay down in one pass (a voxel holds one material: that share of them takes it)");

    _hardness = new QDoubleSpinBox(this);
    _hardness->setRange(0.0, 100.0);
    _hardness->setDecimals(0);
    _hardness->setSingleStep(5.0);
    _hardness->setSuffix(" %");
    _hardness->setValue(50.0);
    addField("Hardness", _hardness, 0.5, false, "The part of the brush with its full effect, before the soft edge; with a shape "
             "image, pushes its greys toward white (harder) or black (softer), the image as it is at 50 %");

    // the brush's shape: round, or a picture from the client's brushes or a file, turned by the angle
    auto shapeName = new QLabel("Shape", this);
    shapeName->setObjectName("EditorFieldName");
    addWidget(shapeName);
    _shape = new QToolButton(this);
    _shape->setPopupMode(QToolButton::InstantPopup);
    _shape->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _shape->setFocusPolicy(Qt::NoFocus);
    _shape->setToolTip("The brush's shape: round, or an image laid over the brush seen from above, white where the brush has its "
                       "full effect and black where none; Dig and Fill keep their sphere. Load Image… takes any image from disk");
    _shapeMenu = new QMenu(_shape);
    const QString roundThumbnail = QString(BRUSHES_FOLDER) + ROUND_THUMBNAIL;
    addShape("Round", QString(), roundThumbnail);
    QFile list(QString(BRUSHES_FOLDER) + BRUSHES_LIST);
    if (list.open(QIODevice::ReadOnly)) {
        for (const QJsonValue& value : QJsonDocument::fromJson(list.readAll()).array()) {
            const QJsonObject entry = value.toObject();
            const QString name = entry.value("name").toString();
            const QString file = entry.value("file").toString();
            if (!name.isEmpty() && !file.isEmpty()) {
                const QString path = QString(BRUSHES_FOLDER) + file;
                addShape(name, path, path);
            }
        }
    }
    _shapeMenu->addSeparator();
    _loadShape = _shapeMenu->addAction("Load Image…");
    connect(_loadShape, &QAction::triggered, this, &TerrainToolBar::loadShapePicture);
    _shape->setMenu(_shapeMenu);
    addWidget(_shape);
    chooseShape("Round", QString(), roundThumbnail);

    _angle = new QDoubleSpinBox(this);
    _angle->setObjectName("EditorTerrainAngle");
    _angle->setRange(0.0, 360.0);
    _angle->setWrapping(true);
    _angle->setDecimals(0);
    _angle->setSingleStep(15.0);
    _angle->setSuffix(QString::fromUtf8("°"));
    _angle->setValue(0.0);
    addField("Angle", _angle, 0.5, false, "Turns the shape image about the brush's centre");
    // a new angle at random for each stroke, so a stamped picture doesn't repeat itself (Piper's ask through Caity,
    // 2026-10-26). Per stroke, not per step: a stroke applies a step every frame, and a new turn each time would blur
    // the picture into a round smudge
    _randomAngle = new QCheckBox("Random", this);
    _randomAngle->setObjectName("EditorTerrainRandomAngle");
    _randomAngle->setFocusPolicy(Qt::NoFocus);
    _randomAngle->setToolTip("A new angle at random for each stroke: the shape image lands turned a different way each "
                             "time you press, and the outline shows the next one. For a shape image, not Round");
    addWidget(_randomAngle);
    connect(_randomAngle, &QCheckBox::toggled, this, [this](bool on) {
        _angle->setEnabled(!on);
        emit brushChanged();
    });

    auto layerName = new QLabel("Layer", this);
    layerName->setObjectName("EditorFieldName");
    addWidget(layerName);
    _layer = new LayerCombo([this] { refreshLayerNames(); }, this);
    _layer->setFocusPolicy(Qt::ClickFocus);
    _layer->setToolTip("The layer Paint and Erase work on, Paint Voxels paints, and new ground gets (its slot, 0 to 7, and its "
                       "name on the selected terrain, or the one under the pointer); selecting a layer in the Inspector's Layers "
                       "section sets it too");
    addWidget(_layer);
    refreshLayerNames();
    _eraseVoxels = new QCheckBox("Erase", this);
    _eraseVoxels->setFocusPolicy(Qt::NoFocus);
    _eraseVoxels->setToolTip("Paint Voxels: set the bottom layer's material, the ground's own, instead of the chosen layer's, taking "
                             "painted voxels back; Shift does the same while held");
    addWidget(_eraseVoxels);
    connect(_eraseVoxels, &QCheckBox::toggled, this, &TerrainToolBar::brushChanged);
    connect(_modes, &QActionGroup::triggered, this, [this] { updateEraseVoxels(); });
    updateEraseVoxels();

    for (auto box : { _size, _strength, _hardness, _angle }) {
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &TerrainToolBar::brushChanged);
    }
    connect(_layer, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TerrainToolBar::brushChanged);

    addSeparator();
    auto resample = addAction("Resample");
    resample->setToolTip("Remake the selected terrain's voxels at the Inspector's Voxel Size over its current size: after the terrain "
                         "was scaled, or its Voxel Size changed (the shape stays, the voxels get finer or coarser)");
    connect(resample, &QAction::triggered, this, &TerrainToolBar::resampleRequested);

    addSeparator();
    _detail = new QDoubleSpinBox(this);
    _detail->setRange(0.0, 16.0);
    _detail->setDecimals(1);
    _detail->setSingleStep(0.5);
    _detail->setSpecialValueText("Full");
    _detail->setValue(RenderableTerrainEntityItem::getLevelOfDetail());
    addField("Detail", _detail, 0.02, false,
             "How far out the ground is drawn at full detail: each part of a terrain while the camera is within this many times "
             "its own size of it, coarser further away (every 2nd, 4th... voxel). Full: full detail everywhere. For every "
             "terrain on this client; walking and picking always use full detail");
    connect(_detail, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [](double detail) {
        RenderableTerrainEntityItem::setLevelOfDetail((float)detail);
    });

    auto hint = new QLabel("Drag on the ground to sculpt it  ·  Shift inverts  ·  Esc cancels the stroke", this);
    hint->setObjectName("EditorHint");
    addWidget(hint);
}

void TerrainToolBar::attachScrub(ScrubLabel* label, QAbstractSpinBox* box, double step, bool proportional) {
    const double COARSE = 10.0;
    const double FINE = 0.1;
    auto doubleBox = qobject_cast<QDoubleSpinBox*>(box);
    auto intBox = qobject_cast<QSpinBox*>(box);
    auto accumulated = std::make_shared<double>(0.0);   // the value as dragged, before the field rounds it
    connect(label, &ScrubLabel::scrubBegan, this, [=] {
        box->interpretText();   // anything half-typed counts first
        *accumulated = doubleBox ? doubleBox->value() : (double)intBox->value();
    });
    connect(label, &ScrubLabel::scrubbed, this, [=](int deltaPixels, Qt::KeyboardModifiers modifiers) {
        double perPixel = step;
        if (proportional) {
            perPixel *= std::max(std::abs(*accumulated), 0.5);
        }
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            perPixel *= COARSE;
        } else if (modifiers.testFlag(Qt::AltModifier)) {
            perPixel *= FINE;
        }
        if (doubleBox) {
            *accumulated = qBound(doubleBox->minimum(), *accumulated + deltaPixels * perPixel, doubleBox->maximum());
            doubleBox->setValue(*accumulated);
        } else if (intBox) {
            *accumulated = qBound((double)intBox->minimum(), *accumulated + deltaPixels * perPixel, (double)intBox->maximum());
            intBox->setValue((int)std::lround(*accumulated));
        }
    });
}

QAction* TerrainToolBar::addMode(const QString& text, const QString& toolTip, terrain::SculptMode mode, bool erase) {
    auto action = addAction(text);
    action->setCheckable(true);
    action->setToolTip(toolTip);
    action->setData((int)mode | (erase ? ERASE_FLAG : 0));
    _modes->addAction(action);
    connect(action, &QAction::triggered, this, &TerrainToolBar::brushChanged);
    return action;
}

void TerrainToolBar::setLayerNameSource(std::function<QStringList()> source) {
    _layerNames = std::move(source);
    refreshLayerNames();
}

void TerrainToolBar::refreshLayerNames() {
    const QStringList names = _layerNames ? _layerNames() : QStringList();
    const int slot = _layer->count() > 0 ? _layer->currentData().toInt() : 0;
    const QSignalBlocker blocker(_layer);   // the slot does not change here: nothing to announce
    _layer->clear();
    for (int i = 0; i < terrain::MAX_LAYERS; ++i) {
        const QString name = i < names.size() ? names[i] : QString();
        _layer->addItem(QString("%1  %2").arg(i).arg(name.isEmpty() ? QStringLiteral("(unused)") : name), i);
    }
    _layer->setCurrentIndex(glm::clamp(slot, 0, terrain::MAX_LAYERS - 1));
}

QAction* TerrainToolBar::addShape(const QString& name, const QString& path, const QString& thumbnail) {
    auto action = new QAction(thumbnailIcon(thumbnail), name, _shapeMenu);
    connect(action, &QAction::triggered, this, [this, name, path, thumbnail] { chooseShape(name, path, thumbnail); });
    if (_loadShape) {
        _shapeMenu->insertAction(_loadShape, action);   // before the separator and Load Picture…
    } else {
        _shapeMenu->addAction(action);
    }
    return action;
}

void TerrainToolBar::chooseShape(const QString& name, const QString& path, const QString& thumbnail) {
    _shapePath = path;
    _shape->setText(name);
    _shape->setIcon(thumbnailIcon(thumbnail));
    emit brushChanged();
}

void TerrainToolBar::loadShapePicture() {
    const QString path = QFileDialog::getOpenFileName(this, "Choose a Brush Image", QDir::homePath(),
        "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga *.tif *.tiff)");
    if (path.isEmpty()) {
        return;
    }
    const QString name = QFileInfo(path).completeBaseName();
    addShape(name, path, path);
    chooseShape(name, path, path);
}

EditorTerrainBrush TerrainToolBar::brush() const {
    EditorTerrainBrush brush;
    if (auto checked = _modes->checkedAction()) {
        const int data = checked->data().toInt();
        brush.mode = (terrain::SculptMode)(data & ~ERASE_FLAG);
        brush.erase = (data & ERASE_FLAG) != 0 ||
                      (brush.mode == terrain::SculptMode::PaintVoxels && _eraseVoxels && _eraseVoxels->isChecked());
    }
    brush.radius = (float)_size->value();
    brush.strength = (float)_strength->value() / 100.0f;
    brush.hardness = (float)_hardness->value() / 100.0f;
    brush.material = _layer->count() > 0 ? _layer->currentData().toInt() : 0;
    brush.shape = _shapePath;
    brush.angle = (float)_angle->value();
    brush.randomAngle = _randomAngle->isChecked();
    return brush;
}

void TerrainToolBar::setMode(terrain::SculptMode mode) {
    for (auto action : _modes->actions()) {
        if (action->data().toInt() == (int)mode) {   // the mode itself, not its eraser
            action->setChecked(true);
            updateEraseVoxels();
            emit brushChanged();
            return;
        }
    }
}

void TerrainToolBar::updateEraseVoxels() {
    auto checked = _modes->checkedAction();
    const bool voxels = checked && (terrain::SculptMode)(checked->data().toInt() & ~ERASE_FLAG) == terrain::SculptMode::PaintVoxels;
    _eraseVoxels->setEnabled(voxels);
}

void TerrainToolBar::setLayer(int slot) {
    refreshLayerNames();   // the Inspector's list may just have renamed or reordered them
    _layer->setCurrentIndex(glm::clamp(slot, 0, terrain::MAX_LAYERS - 1));   // currentIndexChanged announces it
}

void TerrainToolBar::setRadius(float metres) {
    _size->setValue(glm::clamp(metres, MIN_RADIUS, MAX_RADIUS));   // valueChanged announces it
}
