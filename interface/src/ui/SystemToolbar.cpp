//
//  SystemToolbar.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "SystemToolbar.h"

#include <algorithm>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtSvg/QSvgRenderer>
#include <QAction>   // QtWidgets in Qt 5, QtGui in Qt 6
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>

#include <DependencyManager.h>
#include <PathUtils.h>
#include <ui/TabletScriptingInterface.h>
#include <ui/ToolbarScriptingInterface.h>

#include "ChatButton.h"
#include "InterfaceLogging.h"

namespace {
    const QString QML_TOOLBAR = "com.highfidelity.interface.toolbar.system";   // the floating bar (hifi/Desktop.qml)
    // look A, "plain icons" (Caity 10-28): one line icon per button on the dark bar, its label under it, every button
    // in the same cell; an open panel lights its button Darkride cyan, the pointer greys it, a muted microphone is red.
    // The floating bar draws the same (hifi/toolbars/BarButton.qml).
    const QSize ICON_AREA(44, 28);      // the icon with room at its top right for the badge
    const QRectF ICON(10, 4, 24, 24);
    const QSize SIDE_CELL(68, 58);      // in a column, on the left or the right
    const QSize ROW_CELL(64, 58);       // in a row, along the top or the bottom
    const qreal BADGE = 15.0;           // the count's red capsule, at the icon's top right (Chat, People)
    const QColor ICON_COLOR("#f4f6f8");
    const QColor OPEN_COLOR("#00afff");      // Darkride cyan
    const QColor MUTED_COLOR("#ff5555");
    const QStringList PLACES { "floating", "top", "left", "right", "bottom" };


    Qt::ToolBarArea areaOf(const QString& place) {
        if (place == "top") return Qt::TopToolBarArea;
        if (place == "right") return Qt::RightToolBarArea;
        if (place == "bottom") return Qt::BottomToolBarArea;
        return Qt::LeftToolBarArea;
    }
    QString placeOf(Qt::ToolBarArea area) {
        switch (area) {
            case Qt::TopToolBarArea: return "top";
            case Qt::RightToolBarArea: return "right";
            case Qt::BottomToolBarArea: return "bottom";
            default: return "left";
        }
    }

    // an icon's picture, fitted into rect: an SVG or an image in a data: address (the bar's own buttons draw theirs so,
    // as "data:image/svg;xml,<svg ...>"), a file, or a path in the app's resources (as the QML bar resolves it)
    void drawIcon(QPainter& painter, const QString& source, const QRectF& rect) {
        auto fit = [&rect](QSizeF size) {
            if (size.isEmpty()) {
                return rect;
            }
            size.scale(rect.size(), Qt::KeepAspectRatio);
            return QRectF(rect.center() - QPointF(size.width() / 2, size.height() / 2), size);
        };
        auto drawSvg = [&painter, &fit](QSvgRenderer& svg) {
            if (svg.isValid()) {
                svg.render(&painter, fit(svg.defaultSize()));
            }
        };
        auto drawImage = [&painter, &fit](const QImage& image) {
            if (!image.isNull()) {
                painter.drawImage(fit(image.size()), image);
            }
        };
        if (source.startsWith("data:")) {
            const int comma = source.indexOf(',');
            const QString header = source.left(comma);
            const QByteArray payload = source.mid(comma + 1).toUtf8();
            const QByteArray bytes = header.contains(";base64") ? QByteArray::fromBase64(payload)
                                                                 : QUrl::fromPercentEncoding(payload).toUtf8();
            if (header.contains("svg")) {
                QSvgRenderer svg(bytes);
                drawSvg(svg);
            } else {
                drawImage(QImage::fromData(bytes));
            }
            return;
        }
        QString path = source;
        const QUrl url(source);
        if (url.isLocalFile()) {
            path = url.toLocalFile();
        } else if (url.scheme() == "qrc") {
            path = ":" + url.path();
        } else if (!url.scheme().isEmpty() && url.scheme().length() > 1) {
            return;   // a web address: not fetched for a button
        } else if (QFileInfo(path).isRelative()) {
            path = PathUtils::resourcesPath() + source;
        }
        if (path.endsWith(".svg", Qt::CaseInsensitive)) {
            QSvgRenderer svg(path);
            drawSvg(svg);
        } else {
            drawImage(QImage(path));
        }
    }
}

SystemToolbar* SystemToolbar::instance() {
    static QPointer<SystemToolbar> theInstance;   // the main window owns it once attached
    if (!theInstance) {
        theInstance = new SystemToolbar();
    }
    return theInstance;
}

SystemToolbar::SystemToolbar() : QToolBar("Buttons") {
    setObjectName("systemToolbar");
    setMovable(true);
    setFloatable(false);
    setAllowedAreas(Qt::AllToolBarAreas);
    setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    setIconSize(ICON_AREA);
    toggleViewAction()->setEnabled(false);   // the main window's toolbar menu doesn't hide it: Settings places it
    setSkinned(false);
    connect(this, &QToolBar::orientationChanged, this, &SystemToolbar::fitCells);   // a column's cells are wider
    hide();
}

// by its label ("Enter VR": icons/bar/enter-vr.svg); a button the bar has none for (a script's own) shows its own icon
// in the bar's colours
QString SystemToolbar::iconFor(const QString& text, bool muted) const {
    QString name = text.trimmed().toLower().replace(' ', '-');
    if (name == "audio" && muted) {
        name = "audio-muted";
    }
    const QString path = "icons/bar/" + name + ".svg";
    return !name.isEmpty() && QFile::exists(PathUtils::resourcesPath() + path) ? path : QString();
}

void SystemToolbar::setSkinned(bool skinned) {
    _skinned = skinned;
    // its own look (A), until a skin gives one; the skin's style sheet can tell an open panel's button by
    // QToolButton[active="true"]
    setStyleSheet(skinned ? QString() :
        "QToolBar#systemToolbar { background: #171b2d; border: none; spacing: 2px; padding: 8px 6px; }"
        "QToolBar#systemToolbar QToolButton { color: #9aa1bd; font-size: 11px; border: none; border-radius: 9px;"
        " padding: 0px; }"
        "QToolBar#systemToolbar QToolButton:hover { background: #333a5a; }"
        "QToolBar#systemToolbar QToolButton:pressed { background: #3d4568; }"
        "QToolBar#systemToolbar QToolButton[active=\"true\"] { color: #00afff; background: rgba(0, 175, 255, 36); }");
    rebuild();   // the icons in the new colours
}

void SystemToolbar::attach(QMainWindow* window, TabletProxy* tablet) {
    if (_window || !window || !tablet) {
        return;
    }
    _window = window;
    _tablet = tablet;
    window->addToolBar(areaOf(getPlace()), this);
    ChatButton::add(tablet);   // the one button made here, not by a script: it shows chat's unread count

    auto model = tablet->getButtons();
    connect(model, &QAbstractItemModel::rowsInserted, this, &SystemToolbar::rebuild);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &SystemToolbar::rebuild);
    connect(model, &QAbstractItemModel::modelReset, this, &SystemToolbar::rebuild);
    connect(model, &QAbstractItemModel::layoutChanged, this, &SystemToolbar::rebuild);
    // after the QML bar has followed the change itself (its `shown`), so this has the last word
    connect(tablet, &TabletProxy::toolbarModeChanged, this, [this]() {
        QTimer::singleShot(0, this, &SystemToolbar::updateVisibility);
    });
    rebuild();
    updateVisibility();
}

QString SystemToolbar::getPlace() const {
    const QString place = _placeSetting.get();
    return PLACES.contains(place) ? place : "left";
}

void SystemToolbar::setPlace(const QString& place) {
    if (!PLACES.contains(place) || place == getPlace()) {
        return;
    }
    _placeSetting.set(place);
    if (_window && place != "floating") {
        _window->addToolBar(areaOf(place), this);   // a toolbar the window has already: moved to that side
    }
    emit placeChanged();
    updateVisibility();
    qCDebug(interfaceapp) << "The bar's place:" << place << (_window ? placeOf(_window->toolBarArea(this)) : QString())
                          << (isVisible() ? "shown" : "hidden");
}

void SystemToolbar::restorePlace() {
    if (_window && isDocked() && _window->toolBarArea(this) != areaOf(getPlace())) {
        _window->addToolBar(areaOf(getPlace()), this);
    }
    updateVisibility();
}

void SystemToolbar::setAllowed(bool allowed) {
    _allowed = allowed;
    updateVisibility();
}

void SystemToolbar::updateVisibility() {
    const bool desktopBar = _tablet && _tablet->getToolbarMode();   // not in VR
    const bool docked = isDocked();
    if (auto qmlToolbar = DependencyManager::get<ToolbarScriptingInterface>()->getToolbar(QML_TOOLBAR)) {
        // the sign-in gate's word, as SignInGate wrote it before; whether the QML bar is the one in use is its own
        // `shown` binding's (hifi/Desktop.qml: not while docked), which a C++ write would not remove, so both bars
        // showed when this wrote `shown` (10-24)
        qmlToolbar->writeProperty("visible", _allowed && desktopBar && !docked);
    }
    const bool show = _window && _allowed && desktopBar && docked;
    _wanted = show;
    if (show != isVisible()) {
        qCDebug(interfaceapp) << "The bar on a side:" << (show ? "shown" : "hidden") << getPlace()
                              << "(allowed" << _allowed << ", desktop" << desktopBar << ")";
    }
    setVisible(show);
}

void SystemToolbar::setVisible(bool) {
    QToolBar::setVisible(_wanted);   // whoever asks: Build mode's saved layout both showed and hid it (10-24)
}

bool SystemToolbar::event(QEvent* event) {
    const bool handled = QToolBar::event(event);
    // only the user's drag (the buttons take their own clicks): the window's own moves of the bar are setPlace's
    if (event->type() == QEvent::MouseButtonRelease) {
        QTimer::singleShot(0, this, &SystemToolbar::rememberArea);
    }
    return handled;
}

void SystemToolbar::rememberArea() {
    if (!_window || !isVisible() || !isDocked()) {
        return;
    }
    const QString place = placeOf(_window->toolBarArea(this));
    if (place != getPlace()) {
        _placeSetting.set(place);
        emit placeChanged();
    }
}

void SystemToolbar::rebuild() {
    // each button's action owns its button (QToolBar::addWidget), so deleting the action deletes the button
    for (QAction* action : actions()) {
        removeAction(action);
        delete action;
    }
    if (!_tablet) {
        return;
    }
    auto model = _tablet->getButtons();
    for (int row = 0; row < model->rowCount(); row++) {
        auto proxy = model->data(model->index(row), Qt::UserRole).value<TabletButtonProxy*>();
        if (!proxy) {
            continue;
        }
        auto button = new QToolButton(this);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(ICON_AREA);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        QPointer<TabletButtonProxy> weak(proxy);
        connect(button, &QToolButton::clicked, this, [weak]() {
            // a moment after the click, as the QML bar does: the button may open a panel that takes the mouse
            QTimer::singleShot(10, [weak]() {
                if (weak) {
                    emit weak->clicked();
                }
            });
        });
        connect(proxy, &TabletButtonProxy::propertiesChanged, button, [this, button, weak]() {
            if (weak) {
                refresh(button, weak);
            }
        });
        refresh(button, proxy);
        addWidget(button);
    }
    fitCells();
}

// every button in one cell, the widest's: icons on one line and labels centred under them, whatever their words (Caity
// 10-28: the bar's buttons sat out of line, each as wide as its own label)
void SystemToolbar::fitCells() {
    const auto buttons = findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
    QSize cell = orientation() == Qt::Vertical ? SIDE_CELL : ROW_CELL;
    for (QToolButton* button : buttons) {
        button->setMinimumSize(0, 0);
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        cell = cell.expandedTo(button->sizeHint());
    }
    for (QToolButton* button : buttons) {
        button->setFixedSize(cell);
    }
}

void SystemToolbar::refresh(QToolButton* button, TabletButtonProxy* proxy) {
    const QVariantMap properties = proxy->getProperties();
    const bool active = properties.value("isActive").toBool();
    // QIcon's own disabled and selected forms would grey or darken it: the one picture in every mode
    QIcon icon;
    const QPixmap picture = buttonPicture(properties, active);
    for (auto mode : { QIcon::Normal, QIcon::Active, QIcon::Selected }) {
        icon.addPixmap(picture, mode);
    }
    button->setIcon(icon);
    button->setText(properties.value(active ? "activeText" : "text", properties.value("text")).toString());
    // a skin's style sheet can tell an open app's button: QToolButton[active="true"]
    button->setProperty("active", active);
    button->setProperty("badge", properties.value("badge").toInt());   // for tests (Test.barButton)
    button->style()->unpolish(button);
    button->style()->polish(button);
    if (button->parent() == this) {
        fitCells();   // a new label (Enter VR, Leave VR) may want a wider cell
    }
}

// a button's picture: its line icon, white, cyan while its panel is open, red for a muted microphone (a skin's text
// colour under a skin), with the count waiting as a red capsule at the icon's top right
QPixmap SystemToolbar::buttonPicture(const QVariantMap& properties, bool active) const {
    const qreal ratio = devicePixelRatioF();
    QPixmap pixmap(ICON_AREA * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    const bool muted = properties.value("muted").toBool();
    const QColor color = muted ? MUTED_COLOR : active ? OPEN_COLOR
        : _skinned ? palette().color(QPalette::ButtonText) : ICON_COLOR;
    QString source = iconFor(properties.value("text").toString(), muted);
    if (source.isEmpty()) {
        source = active ? properties.value("activeIcon", properties.value("icon")).toString()
                        : properties.value("icon").toString();
    }
    // the icon alone, then filled with the colour where it's drawn
    QImage glyph((ICON.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    glyph.setDevicePixelRatio(ratio);
    glyph.fill(Qt::transparent);
    {
        QPainter painter(&glyph);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        drawIcon(painter, source, QRectF(QPointF(0, 0), ICON.size()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(QPointF(0, 0), ICON.size()), color);
    }

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawImage(ICON.topLeft(), glyph);

    // a count waiting (chat's unread mentions and direct messages, People's friend requests), as the Dock's
    const int badge = properties.value("badge").toInt();
    if (badge > 0) {
        const QString number = badge > 99 ? "99+" : QString::number(badge);
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);
        const qreal width = std::max(BADGE, QFontMetricsF(font).horizontalAdvance(number) + 7.0);
        const QRectF capsule(std::min<qreal>(ICON.right() - 5, ICON_AREA.width() - width), 0, width, BADGE);
        painter.setBrush(QColor(255, 59, 48));   // Apple's system red
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(capsule, BADGE / 2, BADGE / 2);
        painter.setPen(Qt::white);
        painter.drawText(capsule, Qt::AlignCenter, number);
    }
    return pixmap;
}
