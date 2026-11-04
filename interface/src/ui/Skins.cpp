//
//  Skins.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Skins.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QFontDatabase>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>
#include <QtWidgets/qdrawutil.h>

#include <zlib.h>

#include <PathUtils.h>

#include "InterfaceLogging.h"
#include "editor/ui/ViewportContainer.h"
#include "ui/PanelTheme.h"
#include "ui/SystemToolbar.h"

namespace {

// ---- what a skin may hold ------------------------------------------------------------------------------------------

const qint64 MAX_TOTAL_BYTES = 64 * 1024 * 1024;
const int MAX_FILES = 500;
const int MAX_PICTURE_SIDE = 4096;
const QStringList ALLOWED_SUFFIXES { "json", "png", "jpg", "jpeg", "ttf", "otf", "txt", "md" };
const QString SKIN_FILE = "skin.json";
const QStringList FRAME_PARTS { "window", "menuBar", "menu", "menuItem", "bar", "barButton", "toolbar", "toolButton",
                                "viewport", "panel", "panelTitle", "card", "button", "field", "tab", "scrollBar",
                                "scrollHandle", "statusBar", "popup" };
const QStringList FRAME_STATES { "image", "hover", "pressed", "checked", "disabled" };
const QStringList PLACES { "top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom",
                           "bottom-right" };
const QString OWN_VIEWPORT_LOOK = "#ViewportContainer { background: #000000; }";   // ViewportContainer's own

// a file name a skin may hold: inside the skin (no absolute path, no ".."), a known kind, not a hidden file
bool allowedName(const QString& name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':')) {
        return false;
    }
    for (const QString& part : name.split('/')) {
        if (part == ".." || part.startsWith('.')) {
            return false;
        }
    }
    return !name.startsWith("__MACOSX/") && ALLOWED_SUFFIXES.contains(QFileInfo(name).suffix().toLower());
}

// ---- a .zip, read here: stored and deflated entries only (zlib), the central directory's sizes -----------------------

quint16 u16(const QByteArray& data, qint64 at) {
    return quint16(quint8(data[int(at)])) | quint16(quint8(data[int(at + 1)])) << 8;
}
quint32 u32(const QByteArray& data, qint64 at) {
    return quint32(u16(data, at)) | quint32(u16(data, at + 2)) << 16;
}

// the files in the zip a skin may hold, by name; false with the reason when the zip can't be read or is too big
bool readZip(const QString& path, QMap<QString, QByteArray>& files, QString& error) {
    QFile file(path);
    if (file.size() > MAX_TOTAL_BYTES || !file.open(QIODevice::ReadOnly)) {
        error = file.size() > MAX_TOTAL_BYTES ? "the zip is bigger than 64 MB" : "couldn't open the zip";
        return false;
    }
    const QByteArray data = file.readAll();
    // the end of the central directory: its signature, searched back from the end over the longest comment
    qint64 end = -1;
    for (qint64 at = data.size() - 22; at >= 0 && at >= data.size() - 22 - 65535; at--) {
        if (u32(data, at) == 0x06054b50) {
            end = at;
            break;
        }
    }
    if (end < 0) {
        error = "it isn't a zip";
        return false;
    }
    const int count = u16(data, end + 10);
    qint64 at = u32(data, end + 16);
    if (count > MAX_FILES * 4) {
        error = "the zip holds too many files";
        return false;
    }
    qint64 total = 0;
    for (int i = 0; i < count; i++) {
        if (at + 46 > data.size() || u32(data, at) != 0x02014b50) {
            error = "the zip's directory is damaged";
            return false;
        }
        const quint16 flags = u16(data, at + 8);
        const quint16 method = u16(data, at + 10);
        const quint32 crc = u32(data, at + 16);
        const quint32 packedSize = u32(data, at + 20);
        const quint32 size = u32(data, at + 24);
        const quint16 nameLength = u16(data, at + 28);
        const quint16 extraLength = u16(data, at + 30);
        const quint16 commentLength = u16(data, at + 32);
        const quint32 localAt = u32(data, at + 42);
        const QString name = QString::fromUtf8(data.mid(int(at + 46), nameLength));
        at += 46 + nameLength + extraLength + commentLength;

        if (name.endsWith('/') || !allowedName(name)) {
            continue;   // a folder, or a file a skin doesn't use
        }
        if ((flags & 1) || packedSize == 0xFFFFFFFF || size == 0xFFFFFFFF) {
            error = name + " is encrypted or too big";
            return false;
        }
        total += size;
        if (total > MAX_TOTAL_BYTES || files.size() >= MAX_FILES) {
            error = "the skin is bigger than 64 MB or 500 files";
            return false;
        }
        if (qint64(localAt) + 30 > data.size() || u32(data, localAt) != 0x04034b50) {
            error = "the zip is damaged at " + name;
            return false;
        }
        const qint64 start = qint64(localAt) + 30 + u16(data, localAt + 26) + u16(data, localAt + 28);
        if (start + packedSize > data.size()) {
            error = "the zip is cut short at " + name;
            return false;
        }
        QByteArray bytes;
        if (method == 0) {
            bytes = data.mid(int(start), int(packedSize));
        } else if (method == 8) {
            bytes.resize(int(size));
            z_stream stream {};
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData() + start));
            stream.avail_in = packedSize;
            stream.next_out = reinterpret_cast<Bytef*>(bytes.data());
            stream.avail_out = size;
            const bool inflated = inflateInit2(&stream, -MAX_WBITS) == Z_OK && inflate(&stream, Z_FINISH) == Z_STREAM_END &&
                                  stream.total_out == size;
            inflateEnd(&stream);
            if (!inflated) {
                error = "couldn't unpack " + name;
                return false;
            }
        } else {
            error = name + " is packed a way this can't read";
            return false;
        }
        if (crc32(0, reinterpret_cast<const Bytef*>(bytes.constData()), bytes.size()) != crc) {
            error = name + " is damaged";
            return false;
        }
        files.insert(name, bytes);
    }
    return true;
}

// ---- a skin, read from its folder --------------------------------------------------------------------------------------

struct Frame {
    QMap<QString, QString> images;   // state → absolute path
    int slice[4] { 0, 0, 0, 0 };     // left, top, right, bottom
    int padding[4] { 0, 0, 0, 0 };
};

struct Decoration {
    QString image;
    QString on;
    QString at;
    QPoint offset;
    QSize size;
    bool stretch { false };
};

struct Skin {
    QString id;
    QString folder;
    QString name;
    QString author;
    QVariantMap colors;
    QString fontFamily;
    QStringList fontFiles;
    qreal fontSize { 0 };
    QString backdrop;
    QString backdropFill { "cover" };
    QString backdropColor;
    QMap<QString, Frame> frames;
    QList<Decoration> decorations;
};

// four numbers from a JSON array
bool fourNumbers(const QJsonValue& value, int out[4]) {
    const QJsonArray array = value.toArray();
    if (array.size() != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = qBound(0, array[i].toInt(), 512);
    }
    return true;
}

// a picture of the skin's: its absolute path, if it is one the skin holds and isn't too big; else the problem
QString picture(const Skin& skin, const QString& name, QString& error) {
    if (!allowedName(name) || QFileInfo(name).suffix().toLower() == "json") {
        error = "\"" + name + "\" isn't a picture in the skin";
        return QString();
    }
    const QString path = QDir(skin.folder).filePath(name);
    const QSize size = QImageReader(path).size();
    if (!size.isValid()) {
        error = "the skin has no picture \"" + name + "\"";
        return QString();
    }
    if (size.width() > MAX_PICTURE_SIDE || size.height() > MAX_PICTURE_SIDE) {
        error = "\"" + name + "\" is bigger than 4096 pixels a side";
        return QString();
    }
    return path;
}

bool readSkin(const QString& folder, Skin& skin, QString& error) {
    QFile file(QDir(folder).filePath(SKIN_FILE));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "there's no skin.json";
        return false;
    }
    QJsonParseError parseError;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        error = "skin.json isn't JSON: " + parseError.errorString();
        return false;
    }
    if (json["format"].toInt() != Skins::FORMAT) {
        error = "skin.json's format isn't 1";
        return false;
    }
    skin.folder = folder;
    skin.id = QFileInfo(folder).fileName();
    skin.name = json["name"].toString().trimmed().left(60);
    skin.author = json["author"].toString().trimmed().left(60);
    if (skin.name.isEmpty()) {
        error = "skin.json has no name";
        return false;
    }

    const QJsonObject colors = json["colors"].toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const QString key = it.key();
        if ((PanelTheme::COLOR_KEYS.contains(key) || key == "menuText" || key == "titleText") &&
            QColor::isValidColor(it.value().toString())) {
            skin.colors[key] = it.value().toString();
        }
    }

    const QJsonObject font = json["font"].toObject();
    skin.fontFamily = font["family"].toString().left(80);
    skin.fontSize = qBound(0.0, font["size"].toDouble(), 32.0);
    for (const QJsonValue& value : font["files"].toArray()) {
        const QString name = value.toString();
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (allowedName(name) && (suffix == "ttf" || suffix == "otf") && QFileInfo::exists(QDir(folder).filePath(name))) {
            skin.fontFiles.append(QDir(folder).filePath(name));
        }
    }

    const QJsonObject backdrop = json["backdrop"].toObject();
    if (backdrop.contains("image")) {
        skin.backdrop = picture(skin, backdrop["image"].toString(), error);
        if (skin.backdrop.isEmpty()) {
            return false;
        }
    }
    const QString fill = backdrop["fill"].toString("cover");
    skin.backdropFill = QStringList { "cover", "stretch", "tile", "center" }.contains(fill) ? fill : "cover";
    if (QColor::isValidColor(backdrop["color"].toString())) {
        skin.backdropColor = backdrop["color"].toString();
    }

    const QJsonObject frames = json["frames"].toObject();
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        if (!FRAME_PARTS.contains(it.key())) {
            continue;
        }
        const QJsonObject object = it.value().toObject();
        Frame frame;
        for (const QString& state : FRAME_STATES) {
            if (object.contains(state)) {
                const QString path = picture(skin, object[state].toString(), error);
                if (path.isEmpty()) {
                    return false;
                }
                frame.images[state] = path;
            }
        }
        if (!frame.images.contains("image")) {
            error = "the frame \"" + it.key() + "\" has no image";
            return false;
        }
        fourNumbers(object["slice"], frame.slice);
        if (!fourNumbers(object["padding"], frame.padding)) {
            std::copy(frame.slice, frame.slice + 4, frame.padding);
        }
        skin.frames[it.key()] = frame;
    }

    for (const QJsonValue& value : json["decorations"].toArray()) {
        const QJsonObject object = value.toObject();
        Decoration decoration;
        decoration.image = picture(skin, object["image"].toString(), error);
        if (decoration.image.isEmpty()) {
            return false;
        }
        decoration.on = object["on"].toString("window");
        decoration.at = object["at"].toString("top-left");
        if ((decoration.on != "window" && !FRAME_PARTS.contains(decoration.on)) || !PLACES.contains(decoration.at)) {
            error = "a decoration is on \"" + decoration.on + "\" at \"" + decoration.at + "\", which isn't a place";
            return false;
        }
        const QJsonArray offset = object["offset"].toArray();
        decoration.offset = QPoint(qBound(-512, offset[0].toInt(), 512), qBound(-512, offset[1].toInt(), 512));
        const QJsonArray size = object["size"].toArray();
        if (size.size() == 2) {
            decoration.size = QSize(qBound(1, size[0].toInt(), MAX_PICTURE_SIDE), qBound(1, size[1].toInt(), MAX_PICTURE_SIDE));
        }
        decoration.stretch = object["stretch"].toBool();
        skin.decorations.append(decoration);
    }
    return true;
}

// ---- the style sheet: every widget's frames ----------------------------------------------------------------------------

QString url(const QString& path) {
    return "url(\"" + QDir::fromNativeSeparators(path) + "\")";
}

// a frame's picture for one state as a border-image, its slice the border's widths and the rest of its padding inside
// (a style sheet's order is top, right, bottom, left; a skin's is left, top, right, bottom)
QString borderImage(const Frame& frame, const QString& state) {
    const QString image = frame.images.value(state);
    if (image.isEmpty()) {
        return QString();
    }
    const int left = frame.slice[0], top = frame.slice[1], right = frame.slice[2], bottom = frame.slice[3];
    return QString("border-image: %1 %2 %3 %4 %5 stretch stretch; border-width: %2px %3px %4px %5px; ")
               .arg(url(image)).arg(top).arg(right).arg(bottom).arg(left) +
           QString("padding: %1px %2px %3px %4px; ")
               .arg(qMax(0, frame.padding[1] - top)).arg(qMax(0, frame.padding[2] - right))
               .arg(qMax(0, frame.padding[3] - bottom)).arg(qMax(0, frame.padding[0] - left));
}

// a part's rules: its frame in each state it has a picture for, and any more declarations
QString rules(const Skin& skin, const QString& part, const QString& selector, const QMap<QString, QString>& states,
              const QString& more = QString()) {
    if (!skin.frames.contains(part)) {
        return more.isEmpty() ? QString() : selector + " { " + more + " }\n";
    }
    const Frame& frame = skin.frames[part];
    QString sheet = selector + " { " + borderImage(frame, "image") + more + " }\n";
    for (auto it = states.begin(); it != states.end(); ++it) {
        if (frame.images.contains(it.key())) {
            sheet += it.value() + " { " + borderImage(frame, it.key()) + " }\n";
        }
    }
    return sheet;
}

QString color(const Skin& skin, const QString& key) {
    return skin.colors.value(key).toString();
}

// the docked bar takes the skin's look only when the skin gives it something to sit on, a "bar" frame or a background
// colour; otherwise it keeps its own dark strip and white captions. A colours-only skin used to drop the bar's look
// and give its captions the skin's dark text on the system's dark toolbar, so they vanished (editor-renderer, 10-24).
QString barBackground(const Skin& skin) {
    return color(skin, "background").isEmpty() ? skin.backdropColor : color(skin, "background");
}
bool skinsTheBar(const Skin& skin) {
    return skin.frames.contains("bar") || !barBackground(skin).isEmpty();
}

QString styleSheet(const Skin& skin) {
    QString sheet;
    const QString text = color(skin, "text");
    const QString menuText = color(skin, "menuText").isEmpty() ? text : color(skin, "menuText");
    const QString titleText = color(skin, "titleText").isEmpty() ? text : color(skin, "titleText");
    auto colorOf = [](const QString& value) { return value.isEmpty() ? QString() : "color: " + value + "; "; };

    // the backdrop: behind everything, around and between the panels
    QString backdrop;
    if (!skin.backdropColor.isEmpty()) {
        backdrop += "background-color: " + skin.backdropColor + "; ";
    }
    if (!skin.backdrop.isEmpty()) {
        if (skin.backdropFill == "tile") {
            backdrop += "background-image: " + url(skin.backdrop) + "; background-repeat: repeat-xy; ";
        } else if (skin.backdropFill == "center") {
            backdrop += "background-image: " + url(skin.backdrop) + "; background-repeat: no-repeat; "
                        "background-position: center; ";
        } else {
            backdrop += "border-image: " + url(skin.backdrop) + " 0 0 0 0 stretch stretch; ";
        }
    }
    if (!backdrop.isEmpty()) {
        sheet += "QMainWindow { " + backdrop + "}\n";
        sheet += "QMainWindow::separator { background: transparent; width: 6px; height: 6px; }\n";
    }

    sheet += rules(skin, "menuBar", "QMenuBar", {}, colorOf(menuText) + (skin.frames.contains("menuBar") ? "" : "background: transparent; "));
    sheet += "QMenuBar::item { background: transparent; " + colorOf(menuText) + "padding: 4px 10px; }\n";
    sheet += rules(skin, "menuItem", "QMenuBar::item:selected", {});
    sheet += rules(skin, "menu", "QMenu", {}, colorOf(menuText));
    if (skin.frames.contains("menu")) {
        sheet += "QMenu::item { background: transparent; " + colorOf(menuText) + "padding: 5px 24px 5px 20px; }\n";
    }
    sheet += rules(skin, "menuItem", "QMenu::item:selected", {}, colorOf(menuText));

    if (skinsTheBar(skin)) {
        sheet += rules(skin, "bar", "QToolBar#systemToolbar", {},
                       "spacing: 4px; " + (skin.frames.contains("bar") ? QString()
                                           : "background: " + barBackground(skin) + "; border: none; padding: 6px; "));
        sheet += rules(skin, "barButton", "QToolBar#systemToolbar QToolButton",
                       { { "hover", "QToolBar#systemToolbar QToolButton:hover" },
                         { "pressed", "QToolBar#systemToolbar QToolButton:pressed" },
                         { "checked", "QToolBar#systemToolbar QToolButton[active=\"true\"]" } },
                       colorOf(text) + "font-size: 12px; ");
    }
    sheet += rules(skin, "toolbar", "QToolBar", {});
    sheet += rules(skin, "toolButton", "QToolBar QToolButton",
                   { { "hover", "QToolBar QToolButton:hover" }, { "pressed", "QToolBar QToolButton:pressed" },
                     { "checked", "QToolBar QToolButton:checked" } },
                   colorOf(text));

    sheet += rules(skin, "panelTitle", "QDockWidget::title", {}, "padding-left: 8px; ");
    // the title bar of a panel that floats as well as docks, chat's (ui/DockTitleBar.h): the same frame
    sheet += rules(skin, "panelTitle", "QWidget#DockTitleBar", {});
    if (!titleText.isEmpty()) {
        sheet += "QDockWidget { color: " + titleText + "; }\n";
    }
    sheet += rules(skin, "panel", "QDockWidget > QWidget", {});
    if (skin.frames.contains("panel") && !text.isEmpty()) {
        sheet += "QDockWidget QLabel, QDockWidget QCheckBox, QDockWidget QRadioButton { color: " + text + "; }\n";
    }
    sheet += rules(skin, "card", "QGroupBox", {});

    // checkboxes: the system's box is drawn for Darkride's dark panels, and its white tick all but vanished on a light
    // skin (Sunny Yogurt's Inspector, 10-24); with a skin's colours they are drawn in them: an empty box on the card
    // colour, a ticked one filled with the tint and a white tick
    const QString tint = color(skin, "tint");
    if (!tint.isEmpty()) {
        auto first = [&skin](std::initializer_list<const char*> keys, const QString& otherwise) {
            for (const char* key : keys) {
                const QString value = color(skin, key);
                if (!value.isEmpty()) {
                    return value;
                }
            }
            return otherwise;
        };
        const QString box = first({ "card", "fill", "background" }, "#ffffff");
        const QString edge = first({ "tertiaryText", "secondaryText", "text" }, "#8a8a8a");
        const QString faint = first({ "fill", "separator" }, box);
        sheet += "QCheckBox::indicator { width: 13px; height: 13px; border-radius: 3px; border: 1px solid " + edge +
                 "; background: " + box + "; }\n";
        sheet += "QCheckBox::indicator:checked { background: " + tint + "; border-color: " + tint +
                 "; image: url(:/styles/skin-check.svg); }\n";
        sheet += "QCheckBox::indicator:disabled { background: " + faint + "; }\n";
        sheet += "QCheckBox::indicator:checked:disabled { background: " + edge + "; border-color: " + edge + "; }\n";
    }

    sheet += rules(skin, "button", "QPushButton",
                   { { "hover", "QPushButton:hover" }, { "pressed", "QPushButton:pressed" },
                     { "checked", "QPushButton:checked" }, { "disabled", "QPushButton:disabled" } },
                   colorOf(text));
    sheet += rules(skin, "field", "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit",
                   { { "hover", "QLineEdit:hover, QComboBox:hover" }, { "checked", "QLineEdit:focus" },
                     { "disabled", "QLineEdit:disabled, QComboBox:disabled" } },
                   colorOf(text));
    sheet += rules(skin, "tab", "QTabBar::tab",
                   { { "hover", "QTabBar::tab:hover" }, { "checked", "QTabBar::tab:selected" } }, colorOf(text));
    sheet += rules(skin, "scrollBar", "QScrollBar:vertical", {}, "width: 12px; ");
    sheet += rules(skin, "scrollBar", "QScrollBar:horizontal", {}, "height: 12px; ");
    sheet += rules(skin, "scrollHandle", "QScrollBar::handle:vertical", { { "hover", "QScrollBar::handle:vertical:hover" } },
                   "min-height: 24px; ");
    sheet += rules(skin, "scrollHandle", "QScrollBar::handle:horizontal", { { "hover", "QScrollBar::handle:horizontal:hover" } },
                   "min-width: 24px; ");
    if (skin.frames.contains("scrollBar")) {
        sheet += "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
                 "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n";
    }
    sheet += rules(skin, "statusBar", "QStatusBar", {}, colorOf(text));
    return sheet;
}

// Build mode's panels take the skin's colours by role from the editor itself (Editor::applyStyle, editor-renderer,
// 10-24). One thing it leaves to the skin: a dock's title over the skin's "panelTitle" frame. The title's text takes
// the dock's own colour, and editor.qss's rule on the main window beats the skin's application sheet, so the skin's
// titleText goes on the window too, one step more specific ("QMainWindow" in front).
QString editorSheet(const Skin& skin) {
    const QString titleText = color(skin, "titleText").isEmpty() ? color(skin, "text") : color(skin, "titleText");
    if (titleText.isEmpty() || !skin.frames.contains("panelTitle")) {
        return QString();
    }
    QStringList titles;
    for (const QString panel : { "#EditorInspector", "#EditorHierarchy", "#EditorAssets" }) {
        titles << "QMainWindow QDockWidget" + panel << "QMainWindow QDockWidget" + panel + "::title";
    }
    return titles.join(", ") + " { color: " + titleText + "; }\n";
}

const QString WINDOW_SHEET_BEGIN { "/* skin: begin */" };
const QString WINDOW_SHEET_END { "/* skin: end */" };

// the main window's sheet (Build mode's is in it too) with the skin's part replaced
void setWindowSheet(QMainWindow* window, const QString& part) {
    QString sheet = window->styleSheet();
    const int begin = sheet.indexOf(WINDOW_SHEET_BEGIN);
    const int end = sheet.indexOf(WINDOW_SHEET_END);
    if (begin >= 0 && end > begin) {
        sheet.remove(begin, end + WINDOW_SHEET_END.length() - begin);
    }
    if (!part.isEmpty()) {
        sheet += "\n" + WINDOW_SHEET_BEGIN + "\n" + part + WINDOW_SHEET_END;
    }
    if (sheet != window->styleSheet()) {
        window->setStyleSheet(sheet);
    }
}

// the QML panels' share: colours, each frame's pictures as file: addresses with its slice and padding, and the
// decorations on a panel
QVariantMap panelSkin(const Skin& skin) {
    QVariantMap frames;
    for (auto it = skin.frames.begin(); it != skin.frames.end(); ++it) {
        QVariantMap frame;
        for (auto image = it->images.begin(); image != it->images.end(); ++image) {
            frame[image.key() == "image" ? "source" : image.key()] = QUrl::fromLocalFile(image.value()).toString();
        }
        frame["slice"] = QVariantList { it->slice[0], it->slice[1], it->slice[2], it->slice[3] };
        frame["padding"] = QVariantList { it->padding[0], it->padding[1], it->padding[2], it->padding[3] };
        frames[it.key()] = frame;
    }
    // the decorations on a panel, which the QML panels draw over their frame (qml/darkride/panel/SkinDecorations.qml)
    QVariantList decorations;
    for (const auto& decoration : skin.decorations) {
        if (decoration.on != "panel") {
            continue;
        }
        decorations.append(QVariantMap {
            { "source", QUrl::fromLocalFile(decoration.image).toString() },
            { "at", decoration.at },
            { "offset", QVariantList { decoration.offset.x(), decoration.offset.y() } },
            { "size", decoration.size.isValid() ? QVariantList { decoration.size.width(), decoration.size.height() } : QVariantList() },
            { "stretch", decoration.stretch } });
    }
    return QVariantMap { { "name", skin.name }, { "colors", skin.colors }, { "frames", frames },
                         { "fontFamily", skin.fontFamily }, { "decorations", decorations } };
}

}   // namespace

// ---- the window's frame and the decorations: one click-through layer over the main window -----------------------------

class SkinLayer : public QWidget {
public:
    SkinLayer(QMainWindow* window, const Skin& skin) : QWidget(window), _window(window) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        if (skin.frames.contains("window")) {
            const Frame& frame = skin.frames["window"];
            _windowFrame = QPixmap(frame.images["image"]);
            _windowSlice = QMargins(frame.slice[0], frame.slice[1], frame.slice[2], frame.slice[3]);
        }
        for (const Decoration& decoration : skin.decorations) {
            _decorations.append({ decoration, QPixmap(decoration.image) });
        }
        window->installEventFilter(this);
        for (QObject* child : window->children()) {
            watch(child);
        }
        setGeometry(window->rect());
        cutAwayViewport();
        raise();
        show();
        // Qt makes this layer a native view beside the native 3D view (10-24), and a native view on top took the
        // clicks meant for the sign-in screen and the overlays though the layer is WA_TransparentForMouseEvents: so
        // its window is transparent for input too, and it is cut away over the 3D view
        if (windowHandle()) {
            windowHandle()->setFlag(Qt::WindowTransparentForInput, true);
        }
        qCDebug(interfaceapp) << "Skin layer over the main window:" << (internalWinId() ? "a native surface" : "drawn in the window")
                              << ", transparent for input, the 3D view cut away";
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
            case QEvent::Resize:
                if (watched == _window) {
                    setGeometry(_window->rect());
                }
                cutAwayViewport();
                update();
                break;
            case QEvent::ChildAdded:
                if (watched == _window) {
                    watch(static_cast<QChildEvent*>(event)->child());
                    raise();
                }
                break;
            case QEvent::Move:
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::LayoutRequest:
                cutAwayViewport();
                update();
                break;
            default:
                break;
        }
        return false;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!_windowFrame.isNull()) {
            qDrawBorderPixmap(&painter, rect(), _windowSlice, _windowFrame);
        }
        for (const auto& placed : _decorations) {
            for (const QRect& area : areas(placed.first.on)) {
                draw(painter, placed.first, placed.second, area);
            }
        }
    }

private:
    // the layer never lies over the 3D view (the central widget): not drawn there, and no clicks of its to catch
    void cutAwayViewport() {
        QRegion region(rect());
        if (QWidget* viewport = _window->centralWidget()) {
            region -= QRegion(viewport->geometry());
        }
        setMask(region);
    }

    void watch(QObject* child) {
        if (qobject_cast<QDockWidget*>(child) || qobject_cast<QToolBar*>(child) || qobject_cast<QMenuBar*>(child) ||
            qobject_cast<QStatusBar*>(child) || child == _window->centralWidget()) {
            child->installEventFilter(this);
        }
    }

    // where on the window a part is now: each shown dock for "panel", each shown toolbar for "toolbar", and so on
    QList<QRect> areas(const QString& part) const {
        QList<QRect> found;
        auto add = [&found](QWidget* widget) {
            if (widget && widget->isVisible() && !widget->isWindow()) {
                found.append(widget->geometry());
            }
        };
        if (part == "window") {
            found.append(rect());
        } else if (part == "menuBar") {
            add(_window->menuWidget());
        } else if (part == "viewport") {
            add(_window->centralWidget());
        } else if (part == "statusBar") {
            add(_window->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly));
        } else {
            for (QObject* child : _window->children()) {
                auto dock = qobject_cast<QDockWidget*>(child);
                auto toolbar = qobject_cast<QToolBar*>(child);
                if ((part == "panel" && dock) || (part == "bar" && toolbar && toolbar->objectName() == "systemToolbar") ||
                    (part == "toolbar" && toolbar && toolbar->objectName() != "systemToolbar")) {
                    add(static_cast<QWidget*>(child));
                }
            }
        }
        return found;
    }

    void draw(QPainter& painter, const Decoration& decoration, const QPixmap& pixmap, const QRect& area) const {
        QSize size = decoration.size.isValid() ? decoration.size : pixmap.size();
        const QString& at = decoration.at;
        if (decoration.stretch) {
            if (at == "top" || at == "bottom") {
                size.setWidth(area.width());
            } else if (at == "left" || at == "right") {
                size.setHeight(area.height());
            }
        }
        int x = area.left() + (area.width() - size.width()) / 2;
        int y = area.top() + (area.height() - size.height()) / 2;
        if (at.endsWith("left")) {
            x = area.left();
        } else if (at.endsWith("right")) {
            x = area.right() + 1 - size.width();
        }
        if (at.startsWith("top")) {
            y = area.top();
        } else if (at.startsWith("bottom")) {
            y = area.bottom() + 1 - size.height();
        }
        painter.drawPixmap(QRect(QPoint(x, y) + decoration.offset, size), pixmap);
    }

    QMainWindow* _window;
    QPixmap _windowFrame;
    QMargins _windowSlice;
    QList<QPair<Decoration, QPixmap>> _decorations;
};

// ---- Skins -------------------------------------------------------------------------------------------------------------

QString Skins::folder() const {
    return PathUtils::getAppDataPath() + "skins";
}

void Skins::start(QMainWindow* window) {
    _window = window;
    _ownFont = QApplication::font();
    if (!_held) {
        _held = true;
        setHeld(false);
    }
}

void Skins::setHeld(bool held) {
    if (held == _held) {
        return;
    }
    _held = held;
    if (held) {
        if (!_current.isEmpty()) {
            takeOff();   // the choice stays saved
            emit changed();
        }
        return;
    }
    const QString saved = _currentSetting.get();
    if (!_window || saved.isEmpty() || saved == _current) {
        return;
    }
    const QString problem = apply(saved);
    if (!problem.isEmpty()) {
        qCWarning(interfaceapp) << "Skin" << saved << "not put on:" << problem;
        _currentSetting.set(QString());
    }
    emit changed();
}

QVariantList Skins::getInstalled() const {
    QVariantList installed;
    for (const QFileInfo& info : QDir(folder()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        Skin skin;
        QString error;
        if (readSkin(info.absoluteFilePath(), skin, error)) {
            installed.append(QVariantMap { { "id", skin.id }, { "name", skin.name }, { "author", skin.author } });
        }
    }
    return installed;
}

QString Skins::setCurrent(const QString& id) {
    if (id == _current) {
        return QString();
    }
    QString problem;
    if (id.isEmpty()) {
        takeOff();
    } else {
        problem = apply(id);
    }
    if (problem.isEmpty()) {
        _currentSetting.set(_current);
        emit changed();
    }
    return problem;
}

void Skins::takeOff() {
    qApp->setStyleSheet(QString());
    for (int id : _fontIds) {
        QFontDatabase::removeApplicationFont(id);
    }
    _fontIds.clear();
    QApplication::setFont(_ownFont);
    SystemToolbar::instance()->setSkinned(false);
    if (_window) {
        setWindowSheet(_window, QString());
        if (auto viewport = qobject_cast<ViewportContainer*>(_window->centralWidget())) {
            viewport->setStyleSheet(OWN_VIEWPORT_LOOK);
            viewport->setInset(QMargins());
        }
    }
    delete _layer;
    DependencyManager::get<PanelTheme>()->setSkin(QVariantMap());
    _current.clear();
}

QString Skins::apply(const QString& id) {
    Skin skin;
    QString error;
    if (id.contains('/') || id.contains('\\') || id.startsWith('.') || !readSkin(QDir(folder()).filePath(id), skin, error)) {
        return error.isEmpty() ? "there's no skin called " + id : error;
    }
    takeOff();

    for (const QString& file : skin.fontFiles) {
        const int fontId = QFontDatabase::addApplicationFont(file);
        if (fontId >= 0) {
            _fontIds.append(fontId);
        }
    }
    if (!skin.fontFamily.isEmpty()) {
        QFont font(skin.fontFamily);
        font.setPointSizeF(skin.fontSize > 0 ? skin.fontSize : _ownFont.pointSizeF());
        QApplication::setFont(font);
    }
    SystemToolbar::instance()->setSkinned(skinsTheBar(skin));
    qApp->setStyleSheet(styleSheet(skin));

    if (_window) {
        setWindowSheet(_window, editorSheet(skin));
        if (auto viewport = qobject_cast<ViewportContainer*>(_window->centralWidget())) {
            if (skin.frames.contains("viewport")) {
                const Frame& frame = skin.frames["viewport"];
                viewport->setAttribute(Qt::WA_StyledBackground);   // a plain widget paints a style sheet's frame only so
                // behind the frame's rounded corners: the backdrop's colour (the container is a native surface, so the
                // backdrop's picture can't show through it)
                const QString behind = skin.backdropColor.isEmpty() ? QString("#000000") : skin.backdropColor;
                viewport->setStyleSheet("#ViewportContainer { background: " + behind + "; " + borderImage(frame, "image") + "}");
                viewport->setInset(QMargins(frame.padding[0], frame.padding[1], frame.padding[2], frame.padding[3]));
            }
        }
        if (skin.frames.contains("window") || !skin.decorations.isEmpty()) {
            _layer = new SkinLayer(_window, skin);
        }
    }
    DependencyManager::get<PanelTheme>()->setSkin(panelSkin(skin));
    _current = skin.id;
    qCDebug(interfaceapp) << "Skin on:" << skin.name << "from" << skin.folder << "," << skin.frames.size() << "frames,"
                          << skin.decorations.size() << "decorations";
    return QString();
}

QString Skins::install(const QString& pathOrUrl) {
    const QUrl asUrl(pathOrUrl);
    const QString path = asUrl.isLocalFile() ? asUrl.toLocalFile() : pathOrUrl;
    const QFileInfo source(path);

    // the files a skin may hold, by their names inside it
    QMap<QString, QByteArray> files;
    QString error;
    if (source.suffix().toLower() == "zip") {
        if (!readZip(path, files, error)) {
            return error;
        }
    } else {
        const QDir from = source.isDir() ? QDir(path) : source.dir();
        qint64 total = 0;
        QDirIterator it(from.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            const QString name = from.relativeFilePath(file);
            if (!allowedName(name)) {
                continue;
            }
            total += QFileInfo(file).size();
            if (total > MAX_TOTAL_BYTES || files.size() >= MAX_FILES) {
                return "the skin is bigger than 64 MB or 500 files";
            }
            QFile in(file);
            if (in.open(QIODevice::ReadOnly)) {
                files.insert(name, in.readAll());
            }
        }
    }

    // skin.json at the top, or inside the one folder a zip often wraps it in
    QString prefix;
    if (!files.contains(SKIN_FILE)) {
        for (const QString& name : files.keys()) {
            if (name.count('/') == 1 && name.endsWith("/" + SKIN_FILE)) {
                prefix = name.left(name.size() - SKIN_FILE.size());
                break;
            }
        }
        if (prefix.isEmpty()) {
            return "there's no skin.json in it";
        }
    }
    const QJsonObject json = QJsonDocument::fromJson(files.value(prefix + SKIN_FILE)).object();
    QString id = json["name"].toString().trimmed().toLower();
    id.replace(QRegularExpression("[^a-z0-9]+"), "-");
    id = id.left(40).remove(QRegularExpression("^-+|-+$"));
    if (id.isEmpty()) {
        return "skin.json has no name";
    }

    // written beside the skins, checked, then put in place of any skin of the same name
    QDir skins(folder());
    skins.mkpath(".");
    const QString staging = skins.filePath(".installing-" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!it.key().startsWith(prefix)) {
            continue;
        }
        const QString target = QDir(staging).filePath(it.key().mid(prefix.size()));
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly) || out.write(it.value()) != it.value().size()) {
            QDir(staging).removeRecursively();
            return "couldn't write the skin into " + skins.absolutePath();
        }
    }
    Skin skin;
    if (!readSkin(staging, skin, error)) {
        QDir(staging).removeRecursively();
        return error;
    }
    if (_current == id) {
        takeOff();
    }
    QDir(skins.filePath(id)).removeRecursively();
    if (!skins.rename(QFileInfo(staging).fileName(), id)) {
        QDir(staging).removeRecursively();
        return "couldn't put the skin in place";
    }
    qCDebug(interfaceapp) << "Skin installed:" << skin.name << "as" << id;
    const QString problem = apply(id);   // on now, even if it was on before the new copy came
    if (problem.isEmpty()) {
        _currentSetting.set(_current);
    }
    emit changed();
    return problem;
}

QString Skins::remove(const QString& id) {
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id.startsWith('.')) {
        return "there's no skin called " + id;
    }
    if (_current == id) {
        setCurrent(QString());
    }
    if (!QDir(QDir(folder()).filePath(id)).removeRecursively()) {
        return "couldn't remove " + id;
    }
    emit changed();
    return QString();
}
