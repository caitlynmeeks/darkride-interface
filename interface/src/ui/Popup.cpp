//
//  Popup.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "Popup.h"

#include <algorithm>

#include <QBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QtWidgets/qdrawutil.h>

#include <DependencyManager.h>
#include <PathUtils.h>

#include "PanelTheme.h"
#include "RightsLine.h"

namespace popup {

namespace {

const int ALERT_WIDTH = 260;
const int BANNER_WIDTH = 300;
#ifdef Q_OS_MAC
const int SHADOW = 0;  // macOS draws a frameless window's shadow from its opaque pixels
#else
const int SHADOW = 24;  // elsewhere the popup paints its own
#endif

// What a popup is drawn in: Darkride's own dark look, or while a skin is on (SKINS.md, stage 2) the skin's colours
// where it gives them, its font, and its "popup" frame in place of the rounded panel
std::shared_ptr<const Look> currentLook() {
    auto look = std::make_shared<Look>();
    const QVariantMap skin = DependencyManager::get<PanelTheme>()->getSkin();
    if (skin.isEmpty()) {
        return look;
    }
    const QVariantMap colors = skin.value("colors").toMap();
    auto take = [&colors](QColor& target, const char* key) {
        const QColor color(colors.value(key).toString());
        if (color.isValid()) {
            target = color;
        }
    };
    take(look->panel, "card");
    take(look->border, "separator");
    take(look->title, "text");
    take(look->text, "secondaryText");
    take(look->well, "fill");
    take(look->button, "fill");
    take(look->buttonPressed, "cardPressed");
    take(look->buttonText, "text");
    take(look->tint, "tint");
    take(look->destructive, "destructive");
    look->font = skin.value("fontFamily").toString();
    const QVariantMap frame = skin.value("frames").toMap().value("popup").toMap();
    if (!frame.isEmpty()) {
        auto margins = [](const QVariant& value) {
            const QVariantList list = value.toList();
            return list.size() == 4 ? QMargins(list[0].toInt(), list[1].toInt(), list[2].toInt(), list[3].toInt()) : QMargins();
        };
        look->frame = QPixmap(QUrl(frame.value("source").toString()).toLocalFile());
        look->slice = margins(frame.value("slice"));
        look->padding = margins(frame.value("padding"));
    }
    return look;
}

QString css(const QColor& color) {
    return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor faded(QColor color, int alpha) {
    color.setAlpha(color.alpha() * alpha / 255);
    return color;
}

QString styleSheetFor(const Look& look) {
    QString sheet = look.font.isEmpty() ? QString() : QString("* { font-family: \"%1\"; }\n").arg(look.font);
    sheet += QString(R"(
QLabel { background: transparent; }
QLabel#title { color: %1; font-size: 13px; font-weight: bold; }
QLabel#text { color: %2; font-size: 11px; }
QLabel#rightsLine { color: %2; font-size: 10px; }
QLabel#bannerTitle { color: %1; font-size: 13px; font-weight: 600; }
QLabel#bannerText { color: %2; font-size: 12px; }
QToolButton#details { color: %2; font-size: 11px; border: none; background: transparent; padding: 2px 4px; }
QPlainTextEdit#detailsText { color: %1; font-size: 11px; background: %3; border: 1px solid %4; border-radius: 6px; padding: 4px 6px; }
QLineEdit#field { color: %1; font-size: 12px; background: %3; border: 1px solid %4; border-radius: 5px; padding: 3px 6px;
    selection-background-color: %5; }
QLineEdit#field:focus { border: 1px solid %5; }
QPushButton { color: %6; font-size: 13px; border: none; border-radius: 7px; background: %7; padding: 0 10px; }
QPushButton:pressed { background: %8; }
QPushButton[role="default"] { color: #ffffff; background: %5; }
QPushButton[role="default"]:pressed { background: %9; }
QPushButton[role="default"]:disabled { background: %7; color: %10; }
QPushButton[role="destructive"] { color: %11; }
QPushButton:focus { border: 2px solid %12; }
)")
        .arg(css(look.title), css(look.text), css(look.well), css(look.border), css(look.tint), css(look.buttonText),
             css(look.button), css(look.buttonPressed), css(look.tint.darker(115)))
        .arg(css(faded(look.buttonText, 77)), css(look.destructive), css(faded(look.tint, 150)));
    return sheet;
}

// The panel every popup is drawn on: the skin's popup frame, or a rounded panel with its shadow where the system
// draws none.
void paintPanel(QWidget* widget, qreal radius, const Look& look) {
    QPainter painter(widget);
    if (!look.frame.isNull()) {
        qDrawBorderPixmap(&painter, widget->rect().adjusted(SHADOW, SHADOW, -SHADOW, -SHADOW), look.slice, look.frame);
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF panel = QRectF(widget->rect()).adjusted(SHADOW + 0.5, SHADOW + 0.5, -SHADOW - 0.5, -SHADOW - 0.5);
    painter.setPen(Qt::NoPen);
    for (int spread = SHADOW; spread > 0; spread -= 2) {
        const qreal fade = 1.0 - (qreal)spread / SHADOW;
        painter.setBrush(QColor(0, 0, 0, (int)(10 * fade * fade)));
        painter.drawRoundedRect(panel.adjusted(-spread, -spread + SHADOW / 3, spread, spread + SHADOW / 3),
                                radius + spread, radius + spread);
    }
    painter.setBrush(look.panel);
    painter.setPen(QPen(look.border, 1.0));
    painter.drawRoundedRect(panel, radius, radius);
}

// a popup's room inside its edge: its own, or more where the skin's frame asks for it
QMargins inside(const Look& look, int left, int top, int right, int bottom) {
    return QMargins(SHADOW + std::max(left, look.padding.left()), SHADOW + std::max(top, look.padding.top()),
                    SHADOW + std::max(right, look.padding.right()), SHADOW + std::max(bottom, look.padding.bottom()));
}

// A popup's height for its fixed width. (adjustSize asks the layout for its height at the layout's own preferred
// width, narrower than the popup, so wrapped lines got counted twice and left gaps under them.)
void fitHeight(QWidget* popup) {
    popup->ensurePolished();
    QLayout* layout = popup->layout();
    layout->activate();
    popup->resize(popup->width(), layout->hasHeightForWidth() ? layout->totalHeightForWidth(popup->width())
                                                              : layout->totalSizeHint().height());
}

// A picture at a size in points, sharp on a high-density screen.
QPixmap sized(const QPixmap& picture, int points, qreal devicePixelRatio) {
    if (picture.isNull()) {
        return picture;
    }
    QPixmap result = picture.scaled(QSize(points, points) * devicePixelRatio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    result.setDevicePixelRatio(devicePixelRatio);
    return result;
}

// The Details fold-out's chevron, pointing right while closed and down while open.
QIcon chevron(bool open, const QColor& color) {
    const qreal ratio = 2.0;
    QPixmap pixmap(QSize(10, 10) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF line;
    if (open) {
        line << QPointF(2.0, 3.5) << QPointF(5.0, 6.5) << QPointF(8.0, 3.5);
    } else {
        line << QPointF(3.5, 2.0) << QPointF(6.5, 5.0) << QPointF(3.5, 8.0);
    }
    painter.drawPolyline(line);
    return QIcon(pixmap);
}

// a popup's words are plain text, never markup: a title or text often carries outside words (a thing's name, a file's
// name, someone's user name), and they are shown exactly as written. No caller passes markup
QLabel* label(const char* name, const QString& text) {
    auto result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName(name);
    result->setWordWrap(true);
    result->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    return result;
}

}

Alert::Alert(const QPixmap& picture, const QString& title, const QString& text, const QString& details,
             const std::vector<Button>& buttons, QWidget* parent, const Field* field) :
    QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setWindowTitle(title);
    setFocusPolicy(Qt::StrongFocus);  // the alert holds the focus, so Return and Escape reach it
    _look = currentLook();
    setStyleSheet(styleSheetFor(*_look));
    const QMargins margins = inside(*_look, 16, 20, 16, 16);
    setFixedWidth(ALERT_WIDTH + margins.left() + margins.right() - 32);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);

    auto pictureLabel = new QLabel();
    pictureLabel->setPixmap(sized(picture, 64, devicePixelRatioF()));
    pictureLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(pictureLabel);
    layout->addSpacing(12);
    layout->addWidget(label("title", title));
    if (!text.isEmpty()) {
        layout->addSpacing(6);
        layout->addWidget(label("text", text));
    }
    if (field) {
        layout->addSpacing(10);
        _field = new QLineEdit(field->text);
        _field->setObjectName("field");
        _field->setPlaceholderText(field->placeholder);
        _field->setAttribute(Qt::WA_MacShowFocusRect, false);
        layout->addWidget(_field);
    }
    if (!details.isEmpty()) {
        layout->addSpacing(6);
        auto toggle = new QToolButton();
        toggle->setObjectName("details");
        toggle->setText("Details");
        toggle->setIcon(chevron(false, _look->text));
        toggle->setIconSize(QSize(10, 10));
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setLayoutDirection(Qt::RightToLeft);  // the chevron after the word
        toggle->setFocusPolicy(Qt::TabFocus);
        toggle->setAttribute(Qt::WA_MacShowFocusRect, false);
        layout->addWidget(toggle, 0, Qt::AlignHCenter);
        auto box = new QPlainTextEdit(details);
        box->setObjectName("detailsText");
        box->setReadOnly(true);
        box->setFixedHeight(112);
        box->setFrameShape(QFrame::NoFrame);
        box->setAttribute(Qt::WA_MacShowFocusRect, false);
        box->setVisible(false);
        layout->addSpacing(4);
        layout->addWidget(box);
        QObject::connect(toggle, &QToolButton::clicked, this, [this, toggle, box]() {
            const bool open = !box->isVisible();
            box->setVisible(open);
            toggle->setIcon(chevron(open, _look->text));
            fitHeight(this);
        });
    }
    layout->addSpacing(details.isEmpty() ? 16 : 12);

    // Two buttons side by side, Cancel always on the left (Caity, 10-19) and the default on the right; more,
    // stacked with the default on top. A button that deletes for good is never the default: Cancel is (10-18).
    bool destructive = false;
    for (int i = 0; i < (int)buttons.size(); i++) {
        if (buttons[i].role == Role::Default && _default < 0) {
            _default = i;
        }
        if (buttons[i].role == Role::Cancel && _cancel < 0) {
            _cancel = i;
        }
        destructive = destructive || buttons[i].role == Role::Destructive;
    }
    if (_default < 0 && destructive) {
        _default = _cancel;
    }
    const bool sideBySide = buttons.size() == 2;
    auto row = sideBySide ? (QBoxLayout*)new QHBoxLayout() : (QBoxLayout*)new QVBoxLayout();
    row->setSpacing(8);
    std::vector<int> order;
    for (int i = 0; i < (int)buttons.size(); i++) {
        if (i != _default) {
            order.push_back(i);
        }
    }
    if (_default >= 0) {
        if (sideBySide) {
            order.push_back(_default);
        } else {
            order.insert(order.begin(), _default);
        }
    }
    if (sideBySide && _cancel >= 0 && order.front() != _cancel) {
        order.erase(std::find(order.begin(), order.end(), _cancel));
        order.insert(order.begin(), _cancel);
    }
    for (int i : order) {
        auto button = new QPushButton(buttons[i].label);
        button->setProperty("role", i == _default ? "default" : buttons[i].role == Role::Destructive ? "destructive" : "plain");
        button->setFixedHeight(28);
        button->setAutoDefault(false);
        button->setFocusPolicy(Qt::TabFocus);  // no focus ring until the keyboard moves between buttons
        button->setAttribute(Qt::WA_MacShowFocusRect, false);
        QObject::connect(button, &QPushButton::clicked, this, [this, i]() {
            _answer = i;
            accept();
        });
        if (i == _default) {
            _defaultButton = button;
        }
        row->addWidget(button, 1);
    }
    layout->addLayout(row);

    // the default button waits for the field to hold something (unless the default is Cancel)
    if (_field && _defaultButton && _default != _cancel) {
        auto update = [this]() { _defaultButton->setEnabled(!fieldText().isEmpty()); };
        QObject::connect(_field, &QLineEdit::textChanged, this, update);
        update();
    }
    fitHeight(this);
}

QString Alert::fieldText() const {
    return _field ? _field->text().trimmed() : QString();
}

void Alert::addRightsLine() {
    // the tint as the link, lighter on a dark panel (the default blue is too dark to read that small)
    const QColor link = _look->panel.lightness() < 128 ? _look->tint.lighter(135) : _look->tint;
    auto line = rights::label("rightsLine", link, this);
    line->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    auto column = static_cast<QVBoxLayout*>(layout());
    column->addSpacing(10);
    column->addWidget(line);
    fitHeight(this);
}

void Alert::paintEvent(QPaintEvent*) {
    paintPanel(this, 12.0, *_look);
}

// Return is the default button, Escape the cancel button, as in every Mac alert. (The field passes both on.)
void Alert::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (_defaultButton && _defaultButton->isEnabled()) {
            _answer = _default;
            accept();
        }
    } else if (event->key() == Qt::Key_Escape) {
        _answer = _cancel;
        reject();
    } else {
        QDialog::keyPressEvent(event);
    }
}

// Over the middle of the window, a third of the way down, where Mac alerts appear.
void Alert::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    QWidget* anchor = parentWidget() ? parentWidget()->window() : nullptr;
    const QRect area = anchor ? anchor->geometry() : screen()->availableGeometry();
    move(area.center().x() - width() / 2, area.top() + (area.height() - height()) / 3);
    if (_field) {
        _field->setFocus();
        _field->selectAll();
    } else {
        setFocus();
    }
}

Banner::Banner(const QPixmap& picture, const QString& title, const QString& text, QWidget* anchor, int milliseconds) :
    QWidget(anchor, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_DeleteOnClose);
    _look = currentLook();
    setStyleSheet(styleSheetFor(*_look));
    const QMargins margins = inside(*_look, 12, 10, 12, 10);
    setFixedWidth(BANNER_WIDTH + margins.left() + margins.right() - 24);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(margins);
    layout->setSpacing(10);
    auto pictureLabel = new QLabel();
    pictureLabel->setPixmap(sized(picture, 40, devicePixelRatioF()));
    pictureLabel->setFixedSize(40, 40);
    layout->addWidget(pictureLabel, 0, Qt::AlignVCenter);
    auto words = new QVBoxLayout();
    words->setSpacing(2);
    auto titleLabel = new QLabel(title);
    titleLabel->setTextFormat(Qt::PlainText);   // outside words stay words (label() above)
    titleLabel->setObjectName("bannerTitle");
    titleLabel->setWordWrap(true);
    auto textLabel = new QLabel(text);
    textLabel->setTextFormat(Qt::PlainText);
    textLabel->setObjectName("bannerText");
    textLabel->setWordWrap(true);
    words->addWidget(titleLabel);
    words->addWidget(textLabel);
    layout->addLayout(words, 1);
    fitHeight(this);
    if (anchor) {
        const QRect area = anchor->window()->geometry();
        move(area.right() - width() - 16 + SHADOW, area.top() + 16 - SHADOW);
        setWindowOpacity(0.0);
        show();
        fade(1.0, 180);
        QTimer::singleShot(milliseconds, this, [this]() { fade(0.0, 400, true); });
    }
}

void Banner::paintEvent(QPaintEvent*) {
    paintPanel(this, 14.0, *_look);
}

void Banner::mousePressEvent(QMouseEvent*) {
    close();
}

void Banner::fade(qreal to, int milliseconds, bool thenClose) {
    auto animation = new QPropertyAnimation(this, "windowOpacity", this);
    animation->setDuration(milliseconds);
    animation->setEndValue(to);
    if (thenClose) {
        QObject::connect(animation, &QPropertyAnimation::finished, this, &QWidget::close);
    }
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

Notice::Notice(const QPixmap& picture, const QString& title, const QString& text, QWidget* anchor) :
    QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    _look = currentLook();
    setStyleSheet(styleSheetFor(*_look));
    const QMargins margins = inside(*_look, 16, 20, 16, 20);
    setFixedWidth(ALERT_WIDTH + margins.left() + margins.right() - 32);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    auto pictureLabel = new QLabel();
    pictureLabel->setPixmap(sized(picture, 64, anchor ? anchor->devicePixelRatioF() : devicePixelRatioF()));
    pictureLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(pictureLabel);
    layout->addSpacing(12);
    layout->addWidget(label("title", title));
    if (!text.isEmpty()) {
        layout->addSpacing(6);
        layout->addWidget(label("text", text));
    }
    fitHeight(this);
    // where an alert would be
    if (anchor) {
        const QRect area = anchor->window()->geometry();
        move(area.center().x() - width() / 2, area.top() + (area.height() - height()) / 3);
    }
}

void Notice::paintEvent(QPaintEvent*) {
    paintPanel(this, 12.0, *_look);
}

QPixmap appIcon() {
    return QPixmap(PathUtils::resourcesPath() + "images/darkride-logo.png");
}

}
