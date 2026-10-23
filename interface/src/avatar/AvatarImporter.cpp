//
//  AvatarImporter.cpp
//  interface/src/avatar
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AvatarImporter.h"

#include <memory>
#include <vector>

#include <QBoxLayout>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegExp>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <AssimpSerializer.h>
#include <AutoRig.h>
#include <AvatarWriter.h>
#include <FstBake.h>
#include <FBXSerializer.h>
#include <GLMHelpers.h>
#include <GLTFSerializer.h>
#include <HumanoidJointMap.h>
#include <MainWindow.h>
#include <NetworkAccessManager.h>
#include <OBJSerializer.h>
#include <PathUtils.h>
#include <StandardSkeleton.h>
#include <UnityAvatar.h>
#include <UnityPackage.h>
#include <VRMReader.h>

#include "Application.h"
#include "AvatarManager.h"
#include "InterfaceLogging.h"
#include "MyAvatar.h"
#include "editor/EditorCreate.h"

namespace {

const float USUAL_HEIGHT = 1.70f;

// The joints a rig needs before Darkride's animations can drive it. Not Spine1: Second Life's and some VRM skeletons
// have two spine joints, and the animations drive them well enough.
const char* const NEEDED_JOINTS[] = { "Hips", "Spine", "Neck", "Head",
                                      "LeftArm", "LeftForeArm", "LeftHand", "RightArm", "RightForeArm", "RightHand",
                                      "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot" };

enum class RigKind { None, Standard, Other };

struct Import {
    QString path;
    QString name;
    HFMModel::Pointer model;
    autorig::Body body;
    RigKind rig { RigKind::None };
    QString why;  // for RigKind::Other: what keeps the animations from driving it
    VRMInfo vrm;
    // For keeping its own rig: what Darkride works out when it wears the model, for its .fst
    QStringList fstLines;
    QStringList found;  // what they say, for the question
};
using ImportPointer = std::shared_ptr<Import>;

HFMModel::Pointer readModel(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    const QByteArray data = file.readAll();
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());
    const QVariantHash mapping;
    if (suffix == "fbx") {
        return FBXSerializer().read(data, mapping, url);
    }
    if (suffix == "gltf" || suffix == "glb") {
        return GLTFSerializer().read(data, mapping, url);
    }
    if (suffix == "obj") {
        return OBJSerializer().read(data, mapping, url);
    }
    return AssimpSerializer().read(data, mapping, url);
}

// None: no skin. Standard: the needed joints, by their names or by the names Darkride gives them when it's worn (a
// VRM's humanoid bones, else HumanoidJointMap), so Darkride's animations drive it; joints whose axes aren't Mixamo's
// get offsets when it's worn (HumanoidRotationOffsets). Other: a skeleton in which the needed joints can't be found.
RigKind classify(const HFMModel& model, const VRMInfo& vrm, QString& why) {
    if (model.skinDeformers.empty()) {
        return RigKind::None;
    }
    QStringList names = model.getJointNames();
    for (QString& name : names) {
        name = vrm.jointNames.value(name, name);
    }
    if (!hasMainHumanoidJoints(names)) {
        std::vector<HumanoidJointInfo> joints;
        for (int i = 0; i < (int)model.joints.size(); i++) {
            const HFMJoint& joint = model.joints[i];
            joints.push_back({ names[i], joint.parentIndex, joint.isSkeletonJoint, extractTranslation(joint.globalTransform) });
        }
        const QHash<int, QString> humanoidNames = mapHumanoidJoints(joints);
        for (auto itr = humanoidNames.begin(); itr != humanoidNames.end(); itr++) {
            names[itr.key()] = itr.value();
        }
    }
    for (const char* name : NEEDED_JOINTS) {
        if (!names.contains(name)) {
            why = QString("Darkride can't tell which of its joints is the %1").arg(name);
            return RigKind::Other;
        }
    }
    return RigKind::Standard;
}

// A VRM's maker and terms, to show with the questions about it
QString credit(const VRMInfo& vrm, const QString& name) {
    if (!vrm.isVRM()) {
        return QString();
    }
    return QString("\n\n%1 is by %2. Its licence: %3.").arg(vrm.title.isEmpty() ? name : vrm.title,
        vrm.authors.isEmpty() ? QString("an unnamed author") : vrm.authors, vrm.licence);
}

QString avatarName(const QString& path) {
    QString name = QFileInfo(path).completeBaseName();
    name.replace(QRegExp("[^A-Za-z0-9_-]+"), "_");
    return name.isEmpty() ? QString("avatar") : name;
}

// A new folder for each import (name, name-2, name-3...), so the client never shows a cached copy of an earlier one.
QString avatarFolder(const QString& name) {
    const QString base = PathUtils::getAppDataPath() + "Avatars/" + name;
    QString folder = base;
    for (int n = 2; QDir(folder).exists(); n++) {
        folder = base + "-" + QString::number(n);
    }
    return folder;
}

// The importer's alerts and banner, in the design language Caity chose for all of Darkride's popups (10-18): a narrow
// centred alert in the manner of macOS, with a picture on top, a bold title, at most one short line, and full-width
// buttons, side by side when there are two (the default on the right) and stacked when there are more (the default on
// top), the default in blue; further detail sits behind a small Details fold-out, in a scrolling field. A success gets
// no alert but a banner in the window's corner that fades by itself.
namespace popup {

const int ALERT_WIDTH = 260;
const int BANNER_WIDTH = 300;
#ifdef Q_OS_MAC
const int SHADOW = 0;  // macOS draws a frameless window's shadow from its opaque pixels
#else
const int SHADOW = 24;  // elsewhere the popup paints its own
#endif

enum class Role { Default, Cancel, Plain };

struct Button {
    QString label;  // "&&" for an ampersand: a button reads "&" as a keyboard shortcut mark
    Role role;
};

const char* const STYLE = R"(
QLabel { background: transparent; }
QLabel#title { color: #ffffff; font-size: 13px; font-weight: bold; }
QLabel#text { color: rgba(255, 255, 255, 189); font-size: 11px; }
QLabel#bannerTitle { color: #ffffff; font-size: 13px; font-weight: 600; }
QLabel#bannerText { color: rgba(255, 255, 255, 189); font-size: 12px; }
QToolButton#details { color: rgba(255, 255, 255, 189); font-size: 11px; border: none; background: transparent; padding: 2px 4px; }
QPlainTextEdit#detailsText { color: rgba(255, 255, 255, 199); font-size: 11px; background: rgba(0, 0, 0, 71);
    border: 1px solid rgba(255, 255, 255, 20); border-radius: 6px; padding: 4px 6px; }
QPushButton { color: #ffffff; font-size: 13px; border: none; border-radius: 7px; background: rgba(255, 255, 255, 36); padding: 0 10px; }
QPushButton:pressed { background: rgba(255, 255, 255, 56); }
QPushButton[role="default"] { background: #0b6bd8; }
QPushButton[role="default"]:pressed { background: #0a5bb8; }
QPushButton:focus { border: 2px solid rgba(10, 132, 255, 150); }
)";

// The rounded panel every popup is drawn on, with its shadow where the system draws none.
void paintPanel(QWidget* widget, qreal radius) {
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF panel = QRectF(widget->rect()).adjusted(SHADOW + 0.5, SHADOW + 0.5, -SHADOW - 0.5, -SHADOW - 0.5);
    painter.setPen(Qt::NoPen);
    for (int spread = SHADOW; spread > 0; spread -= 2) {
        const qreal fade = 1.0 - (qreal)spread / SHADOW;
        painter.setBrush(QColor(0, 0, 0, (int)(10 * fade * fade)));
        painter.drawRoundedRect(panel.adjusted(-spread, -spread + SHADOW / 3, spread, spread + SHADOW / 3),
                                radius + spread, radius + spread);
    }
    painter.setBrush(QColor(43, 43, 46));
    painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
    painter.drawRoundedRect(panel, radius, radius);
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
QIcon chevron(bool open) {
    const qreal ratio = 2.0;
    QPixmap pixmap(QSize(10, 10) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 255, 255, 189), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF line;
    if (open) {
        line << QPointF(2.0, 3.5) << QPointF(5.0, 6.5) << QPointF(8.0, 3.5);
    } else {
        line << QPointF(3.5, 2.0) << QPointF(6.5, 5.0) << QPointF(3.5, 8.0);
    }
    painter.drawPolyline(line);
    return QIcon(pixmap);
}

class Alert : public QDialog {
public:
    Alert(const QPixmap& picture, const QString& title, const QString& text, const QString& details,
          const std::vector<Button>& buttons, QWidget* parent) :
        QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setWindowModality(Qt::ApplicationModal);
        setWindowTitle(title);
        setFocusPolicy(Qt::StrongFocus);  // the alert holds the focus, so Return and Escape reach it
        setStyleSheet(STYLE);
        setFixedWidth(ALERT_WIDTH + 2 * SHADOW);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(SHADOW + 16, SHADOW + 20, SHADOW + 16, SHADOW + 16);
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
        if (!details.isEmpty()) {
            layout->addSpacing(6);
            auto toggle = new QToolButton();
            toggle->setObjectName("details");
            toggle->setText("Details");
            toggle->setIcon(chevron(false));
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
                toggle->setIcon(chevron(open));
                fitHeight(this);
            });
        }
        layout->addSpacing(details.isEmpty() ? 16 : 12);

        // Two buttons side by side with the default on the right; more, stacked with the default on top.
        const bool sideBySide = buttons.size() == 2;
        auto row = sideBySide ? (QBoxLayout*)new QHBoxLayout() : (QBoxLayout*)new QVBoxLayout();
        row->setSpacing(8);
        std::vector<int> order;
        for (int i = 0; i < (int)buttons.size(); i++) {
            if (buttons[i].role == Role::Default && _default < 0) {
                _default = i;
            } else {
                order.push_back(i);
            }
            if (buttons[i].role == Role::Cancel && _cancel < 0) {
                _cancel = i;
            }
        }
        if (_default >= 0) {
            if (sideBySide) {
                order.push_back(_default);
            } else {
                order.insert(order.begin(), _default);
            }
        }
        for (int i : order) {
            auto button = new QPushButton(buttons[i].label);
            button->setProperty("role", i == _default ? "default" : "plain");
            button->setFixedHeight(28);
            button->setAutoDefault(false);
            button->setFocusPolicy(Qt::TabFocus);  // no focus ring until the keyboard moves between buttons
            button->setAttribute(Qt::WA_MacShowFocusRect, false);
            QObject::connect(button, &QPushButton::clicked, this, [this, i]() {
                _answer = i;
                accept();
            });
            row->addWidget(button, 1);
        }
        layout->addLayout(row);
        fitHeight(this);
    }

    int answer() const { return _answer; }

protected:
    void paintEvent(QPaintEvent*) override { paintPanel(this, 12.0); }

    // Return is the default button, Escape the cancel button, as in every Mac alert.
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && _default >= 0) {
            _answer = _default;
            accept();
        } else if (event->key() == Qt::Key_Escape) {
            _answer = _cancel;
            reject();
        } else {
            QDialog::keyPressEvent(event);
        }
    }

    // Over the middle of the window, a third of the way down, where Mac alerts appear.
    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        QWidget* anchor = parentWidget() ? parentWidget()->window() : nullptr;
        const QRect area = anchor ? anchor->geometry() : screen()->availableGeometry();
        move(area.center().x() - width() / 2, area.top() + (area.height() - height()) / 3);
        setFocus();
    }

private:
    QLabel* label(const char* name, const QString& text) {
        auto result = new QLabel(text);
        result->setObjectName(name);
        result->setWordWrap(true);
        result->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        return result;
    }

    int _answer { -1 };
    int _default { -1 };
    int _cancel { -1 };
};

// A note that comes and goes in the top right corner of the window: clicked, it goes at once.
class Banner : public QWidget {
public:
    Banner(const QPixmap& picture, const QString& title, const QString& text, QWidget* anchor, int milliseconds = 4500) :
        QWidget(anchor, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose);
        setStyleSheet(STYLE);
        setFixedWidth(BANNER_WIDTH + 2 * SHADOW);
        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(SHADOW + 12, SHADOW + 10, SHADOW + 12, SHADOW + 10);
        layout->setSpacing(10);
        auto pictureLabel = new QLabel();
        pictureLabel->setPixmap(sized(picture, 40, devicePixelRatioF()));
        pictureLabel->setFixedSize(40, 40);
        layout->addWidget(pictureLabel, 0, Qt::AlignVCenter);
        auto words = new QVBoxLayout();
        words->setSpacing(2);
        auto titleLabel = new QLabel(title);
        titleLabel->setObjectName("bannerTitle");
        titleLabel->setWordWrap(true);
        auto textLabel = new QLabel(text);
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

protected:
    void paintEvent(QPaintEvent*) override { paintPanel(this, 14.0); }
    void mousePressEvent(QMouseEvent*) override { close(); }

private:
    void fade(qreal to, int milliseconds, bool thenClose = false) {
        auto animation = new QPropertyAnimation(this, "windowOpacity", this);
        animation->setDuration(milliseconds);
        animation->setEndValue(to);
        if (thenClose) {
            QObject::connect(animation, &QPropertyAnimation::finished, this, &QWidget::close);
        }
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }
};

}

QPixmap appIcon() {
    return QPixmap(PathUtils::resourcesPath() + "images/darkride-logo.png");
}

// An error from below as a sentence of its own: "it was cut" becomes "It was cut."
QString sentence(QString text) {
    text = text.trimmed();
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
        if (!text.endsWith('.')) {
            text += '.';
        }
    }
    return text;
}

// One of the importer's questions. Returns the chosen button's place in the list, or -1.
int ask(const QString& title, const QString& text, const std::vector<popup::Button>& buttons,
        const QString& details = QString()) {
    qCDebug(interfaceapp) << "Import Avatar: asking" << title;
    popup::Alert alert(appIcon(), title, text, details, buttons, qApp->getWindow());
    alert.exec();
    const int answer = alert.answer();
    qCDebug(interfaceapp) << "Import Avatar: answered" << (answer < 0 ? QString("nothing") : buttons[answer].label);
    return answer;
}

void warn(const QString& title, const QString& text = QString()) {
    qCDebug(interfaceapp) << "Import Avatar: warning" << title << text;
    popup::Alert alert(appIcon(), title, text, QString(), { { "OK", popup::Role::Default } }, qApp->getWindow());
    alert.exec();
}

// What went right needs no answer: a banner says it and goes.
void announce(const QString& title, const QString& text) {
    qCDebug(interfaceapp) << "Import Avatar: telling" << title << text;
    new popup::Banner(appIcon(), title, text, qApp->getWindow());
}

// A line before and after each step, so a client that stops here shows in its log the step it stopped in (a freeze
// right after "Wear it", 10-13, left no trace).
void wearUrl(const QString& url, const QString& name) {
    qCDebug(interfaceapp) << "Import Avatar: putting on" << url;
    DependencyManager::get<AvatarManager>()->getMyAvatar()->useFullAvatarURL(url, name);
    qCDebug(interfaceapp) << "Import Avatar: avatar change started";
    emit qApp->fullAvatarURLChanged(url, name);
    qCDebug(interfaceapp) << "Import Avatar: wear done";
}

void wear(const QString& fstPath, const QString& name) {
    wearUrl(QUrl::fromLocalFile(fstPath).toString(), name);
}

// An avatar on this computer goes out to others as a blank address (AvatarData::getWireSafeSkeletonModelURL), so they
// see the default. In My Files it has a web address everyone loads: the model goes up with the files it names beside
// it (game rips name their textures; anything else in the folder stays here), then the .fst with its filename line
// pointing beside it, and the client wears that copy. One file at a time, through the editor's upload.
struct Share {
    QString name;
    QString fstName;
    QString remoteFolder;
    QStringList fstLines;
    int modelLine { -1 };
    bool modelIsLocal { false };
    QStringList files;  // local paths, the model first
    int next { 0 };
};
using SharePointer = std::shared_ptr<Share>;

// The web copy is worn only once its .fst reads back from the web: a failed avatar load falls back to the default
// without a word, and the files host sent every .fst labelled gzip over plain text until metaverse 19f3c7d (10-13).
void wearOnceItReadsBack(const SharePointer& share, const QString& url) {
    QNetworkRequest request { QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // A check, so it neither reads nor writes the disk cache: Qt unpacks the gzip itself here and would cache the
    // unpacked body under the gzip label, which the avatar's own load (HTTPResourceRequest) then fails to unpack.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    QNetworkReply* reply = NetworkAccessManager::getInstance().get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, share, url]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || !body.contains("filename")) {
            const QString why = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                                         : QString("it came back different");
            warn("Couldn't wear " + share->name + " from My Files",
                 "Its file doesn't read back from the web (" + why + "). You're still wearing the copy on this computer.");
            return;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "reads back from" << url;
        wearUrl(url, share->name);
        announce(share->name + " is in My Files", "You're wearing it, and everyone can see it.");
    });
}

void shareNext(const SharePointer& share) {
    const bool fstNow = share->next >= share->files.size();
    auto upload = new EditorUpload();
    QString error;
    bool started;
    if (!fstNow) {
        started = upload->start(share->files[share->next], share->remoteFolder, error);
        if (started && share->next == 0 && share->modelIsLocal) {
            share->fstLines[share->modelLine] = "filename = " + upload->fileName();
        }
        share->next++;
    } else {
        started = upload->startData(share->fstLines.join('\n').toUtf8(), share->fstName, share->remoteFolder,
                                    "text/plain", error);
    }
    if (!started) {
        delete upload;
        warn("Couldn't put " + share->name + " in My Files", sentence(error));
        return;
    }
    QObject::connect(upload, &EditorUpload::failed, upload, [upload, share](const QString& error) {
        upload->deleteLater();
        warn("Couldn't put " + share->name + " in My Files", sentence(error));
    });
    QObject::connect(upload, &EditorUpload::finished, upload, [upload, share, fstNow](const QString& url) {
        upload->deleteLater();
        if (!fstNow) {
            shareNext(share);
            return;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "is in My Files at" << url;
        wearOnceItReadsBack(share, url);
    });
}

void shareAvatar(const QString& fstPath, const QString& name) {
    QFile fst(fstPath);
    if (!fst.open(QIODevice::ReadOnly)) {
        warn("Couldn't read " + QFileInfo(fstPath).fileName(), fstPath);
        return;
    }
    auto share = std::make_shared<Share>();
    share->name = name;
    share->fstName = QFileInfo(fstPath).fileName();
    share->remoteFolder = "/avatars/" + name + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString modelPath;
    VRMInfo vrm;
    const QString fstText = QString::fromUtf8(fst.readAll());
    for (const QString& line : fstText.split('\n')) {
        const QString key = line.section('=', 0, 0).trimmed();
        if (key == "texdir") {
            continue;  // the textures go up beside the model, where it looks for them anyway
        }
        if (key == "filename" && share->modelLine < 0) {
            const QString value = line.section('=', 1).trimmed();
            const QUrl url(value);
            if (url.isLocalFile()) {
                modelPath = url.toLocalFile();
            } else if (url.isRelative()) {
                modelPath = QFileInfo(fstPath).dir().absoluteFilePath(value);
            }
            share->modelLine = share->fstLines.size();
        }
        share->fstLines.append(line);
    }
    if (!modelPath.isEmpty()) {
        QFile model(modelPath);
        if (!model.open(QIODevice::ReadOnly)) {
            warn("Couldn't read " + QFileInfo(modelPath).fileName(), modelPath);
            return;
        }
        const QByteArray bytes = model.readAll();
        vrm = readVRM(bytes);
        const QFileInfo modelInfo(modelPath);
        share->files.append(modelInfo.absoluteFilePath());
        share->modelIsLocal = true;
        // The files beside the model that it names (game rips name their textures) or that the .fst names (a
        // converted avatar's material map names its textures).
        for (const QFileInfo& file : modelInfo.dir().entryInfoList(QDir::Files)) {
            if (file != modelInfo && file.suffix().toLower() != "fst" &&
                (bytes.contains(file.fileName().toUtf8()) || fstText.contains("\"" + file.fileName() + "\""))) {
                share->files.append(file.absoluteFilePath());
            }
        }
    }
    // Everyone can see what's in My Files and download it, so only what the user may share goes there
    const QString details = "Only put an avatar there if you made it or its licence lets you share it." + credit(vrm, name);
    QString text = "Everyone in Darkride will see it and can download it.";
    if (vrm.isVRM() && (vrm.onlyAuthorMayWear || !vrm.redistributionAllowed)) {
        text = QString("Its licence says %1: unless you made it, keep it on this computer.")
                   .arg(vrm.onlyAuthorMayWear && !vrm.redistributionAllowed ? "only its author may wear it or share it"
                        : vrm.onlyAuthorMayWear ? "only its author may wear it" : "it may not be shared");
    }
    if (ask("Put " + name + " in My Files?", text,
            { { "I May Share It", popup::Role::Default }, { "Cancel", popup::Role::Cancel } }, details) != 0) {
        return;
    }
    qCDebug(interfaceapp) << "Import Avatar: putting" << name << "in My Files," << share->files.size()
                          << "files and the .fst, into" << share->remoteFolder;
    shareNext(share);
}

// Asked once an avatar is ready. Wear & Keep (wear it and put it in My Files) is the default; the importer's notes
// go in the details.
void offerToWear(const QString& fstPath, const QString& name, const QString& notes) {
    const int answer = ask(name + " is ready",
                           "Keep it in My Files so everyone can see it, or try it on this computer first.",
                           { { "Wear && Keep", popup::Role::Default },
                             { "Test Locally", popup::Role::Plain },
                             { "Not Now", popup::Role::Cancel } },
                           notes);
    if (answer == 0 || answer == 1) {
        wear(fstPath, name);
    }
    if (answer == 0) {
        shareAvatar(fstPath, name);
    }
}

// The rigging and saving happen off the main thread; the answer comes back to it.
void autoRig(const ImportPointer& import, float scale) {
    QtConcurrent::run([import, scale]() {
        autorig::Body body = import->body;
        if (scale != 1.0f) {
            body.scale(scale);
        }
        const autorig::Result result = autorig::rig(body);
        autorig::WrittenAvatar written;
        if (result.ok) {
            written = autorig::writeAvatar(*import->model, body, result, import->path, avatarFolder(import->name), import->name);
        }
        for (const QString& note : written.notes) {
            qCDebug(interfaceapp) << "Import Avatar:" << import->name << "note:" << note;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << import->name
                              << (!result.ok ? "not rigged:" : !written.ok ? "rigged, not saved:" : "rigged and saved as")
                              << (!result.ok ? result.error : !written.ok ? written.error : written.fstPath);
        QMetaObject::invokeMethod(qApp, [import, result, written]() {
            if (!result.ok) {
                warn("Couldn't rig " + import->name, sentence(result.error));
            } else if (!written.ok) {
                warn("Couldn't save " + import->name, sentence(written.error));
            } else {
                offerToWear(written.fstPath, import->name,
                            "Rigged with Darkride's standard skeleton.\n\nSaved in " +
                                QFileInfo(written.fstPath).absolutePath() + ".");
            }
        }, Qt::QueuedConnection);
    });
}

// The scale to wear it at: 1, unless it's implausibly tall or short, as a file whose units are wrong (the 100Avatars Devil's
// FBX says centimetres but is 12 of them tall), when they're asked. False if they cancel.
bool askScale(const ImportPointer& import, float& scale) {
    scale = 1.0f;
    const float height = import->body.maximum.y - import->body.minimum.y;
    if (height > 2.4f || height < 0.6f) {
        const int answer = ask(QString("%1 is %2 m tall").arg(import->name).arg(height, 0, 'f', 2),
                               "That looks like a units mistake in its file.",
                               { { QString("Make It %1 m").arg(USUAL_HEIGHT, 0, 'f', 2), popup::Role::Default },
                                 { QString("Keep %1 m").arg(height, 0, 'f', 2), popup::Role::Plain },
                                 { "Cancel", popup::Role::Cancel } });
        if (answer == 0) {
            scale = USUAL_HEIGHT / height;
        } else if (answer != 1) {
            return false;
        }
    }
    return true;
}

void checkHeightThenRig(const ImportPointer& import) {
    float scale;
    if (askScale(import, scale)) {
        autoRig(import, scale);
    }
}

void decide(const ImportPointer& import) {
    if (!import->model || import->body.triangles.empty()) {
        warn("Couldn't read " + QFileInfo(import->path).fileName(), "Darkride found no character in it.");
        return;
    }
    switch (import->rig) {
        case RigKind::Standard: {
            // what Darkride worked out, and a VRM's maker and terms, in the details
            const QString details = (import->found.isEmpty() ? QString()
                : "Keeping it, Darkride saves what it worked out in the avatar's settings: " + import->found.join(", ") + ".") +
                credit(import->vrm, import->name);
            const int answer = ask(import->name + " already has Darkride's skeleton",
                                   "Keep its own rig, or replace it with an automatic one. The file itself doesn't change.",
                                   { { "Keep Rig", popup::Role::Default },
                                     { "Replace Rig", popup::Role::Plain },
                                     { "Cancel", popup::Role::Cancel } },
                                   details.trimmed());
            float scale;
            if (answer == 0 && askScale(import, scale)) {
                const QString folder = avatarFolder(import->name);
                QDir().mkpath(folder);
                const QString fstPath = QDir(folder).filePath(import->name + ".fst");
                bool written = autorig::writeFst(fstPath, import->name, QUrl::fromLocalFile(import->path).toString(), scale);
                if (written && !import->fstLines.isEmpty()) {
                    QFile fst(fstPath);
                    written = fst.open(QIODevice::Append | QIODevice::Text) &&
                              fst.write((import->fstLines.join("\n") + "\n").toUtf8()) > 0;
                    qCDebug(interfaceapp) << "Import Avatar:" << import->name << "fst written with" << import->fstLines.size()
                                          << "lines of what Darkride worked out:" << import->found;
                }
                if (written) {
                    wear(fstPath, import->name);
                } else {
                    warn("Couldn't write " + QFileInfo(fstPath).fileName(), fstPath);
                }
            } else if (answer == 1) {
                checkHeightThenRig(import);
            }
            break;
        }
        case RigKind::Other:
            if (ask(import->name + "'s skeleton won't move right",
                    sentence("because " + import->why) + " Darkride can replace its rig; the file itself doesn't change.",
                    { { "Replace Rig", popup::Role::Default }, { "Cancel", popup::Role::Cancel } },
                    credit(import->vrm, import->name).trimmed()) == 0) {
                checkHeightThenRig(import);
            }
            break;
        case RigKind::None:
            if (ask(import->name + " has no skeleton yet", "Darkride can rig it with its standard skeleton.",
                    { { "Auto Rig", popup::Role::Default }, { "Cancel", popup::Role::Cancel } },
                    credit(import->vrm, import->name).trimmed()) == 0) {
                checkHeightThenRig(import);
            }
            break;
    }
}

// A .unitypackage (as avatar shops sell avatars): the avatars in it, converted with libraries/unity-import. The package is
// read and the avatar written off the main thread; the questions come back to it.
void convertPackageAvatar(const std::shared_ptr<unity::Package>& package, const avatarimport::AvatarSource& avatar,
                         const QString& packageName) {
    QtConcurrent::run([package, avatar, packageName]() {
        const QString name = avatarName(avatar.name);
        const avatarimport::WrittenAvatar written = avatarimport::writeAvatar(*package, avatar, avatarFolder(name), name);
        for (const QString& note : written.notes) {
            qCDebug(interfaceapp) << "Import Avatar:" << name << "note:" << note;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << name << (written.ok ? "converted and saved as" : "not converted:")
                              << (written.ok ? written.fstPath : written.error);
        QMetaObject::invokeMethod(qApp, [written, name, packageName]() {
            if (!written.ok) {
                warn("Couldn't convert " + name, sentence(written.error));
                return;
            }
            QString notes = "From " + packageName + ".\n\n";
            QStringList notYet;
            for (const QString& note : written.notes) {
                if (note.startsWith("not yet: ")) {
                    notYet << "- " + note.mid(9);
                }
            }
            if (!notYet.isEmpty()) {
                notes += "Not carried over yet:\n" + notYet.join("\n") + "\n\n";
            }
            offerToWear(written.fstPath, name, notes + "Saved in " + QFileInfo(written.fstPath).absolutePath() + ".");
        }, Qt::QueuedConnection);
    });
}

void importUnityPackage(const QString& path) {
    QtConcurrent::run([path]() {
        auto package = std::make_shared<unity::Package>();
        QString error;
        const bool opened = package->open(path, &error);
        const QVector<avatarimport::AvatarSource> avatars = opened ? avatarimport::findAvatars(*package) : QVector<avatarimport::AvatarSource>();
        qCDebug(interfaceapp) << "Import Avatar: read" << path << (opened ? QString("%1 avatars").arg(avatars.size()) : error);
        QMetaObject::invokeMethod(qApp, [package, avatars, path, opened, error]() {
            const QString packageName = QFileInfo(path).fileName();
            if (!opened) {
                warn("Couldn't read " + packageName, sentence(error));
                return;
            }
            if (avatars.isEmpty()) {
                warn(packageName + " holds no avatar",
                     "An outfit or hair package goes onto its avatar, which Darkride can't do yet.");
                return;
            }
            int chosen = 0;
            if (avatars.size() > 1) {
                // findAvatars puts a PC version before the phone ones: two such are versions of one avatar
                bool firstIsPC = true;
                for (int i = 0; i < avatars.size(); i++) {
                    const QString lower = avatars[i].name.toLower();
                    const bool forPhones = lower.contains("mobile") || lower.contains("quest") || lower.contains("android");
                    if (forPhones == (i == 0)) {
                        firstIsPC = false;  // the first is for phones, or another one isn't
                    }
                }
                QString title;
                std::vector<popup::Button> buttons;
                if (avatars.size() == 2 && firstIsPC) {
                    title = avatars[0].name + " comes in two versions";
                    buttons = { { "PC Version", popup::Role::Default }, { "Mobile Version", popup::Role::Plain } };
                } else {
                    title = QString("%1 holds %2 avatars").arg(packageName).arg(avatars.size());
                    for (int i = 0; i < avatars.size() && i < 3; i++) {
                        buttons.push_back({ QString(avatars[i].name).replace("&", "&&"),
                                            i == 0 ? popup::Role::Default : popup::Role::Plain });
                    }
                }
                buttons.push_back({ "Cancel", popup::Role::Cancel });
                const int answer = ask(title, "Which one do you want to import?", buttons);
                if (answer < 0 || answer >= (int)buttons.size() - 1) {
                    return;
                }
                chosen = answer;
            }
            convertPackageAvatar(package, avatars[chosen], packageName);
        }, Qt::QueuedConnection);
    });
}

}

void AvatarImporter::importFile(const QString& path) {
    const QString name = avatarName(path);
    if (QFileInfo(path).suffix().toLower() == "fst") {
        offerToWear(path, name, "From " + path + ".");
        return;
    }
    if (QFileInfo(path).suffix().toLower() == "unitypackage") {
        importUnityPackage(path);
        return;
    }
    auto import = std::make_shared<Import>();
    import->path = path;
    import->name = name;
    QtConcurrent::run([import]() {
        const QString suffix = QFileInfo(import->path).suffix().toLower();
        const bool isGltf = suffix == "vrm" || suffix == "glb" || suffix == "gltf";
        QByteArray data;
        if (isGltf) {
            QFile file(import->path);
            if (file.open(QIODevice::ReadOnly)) {
                data = file.readAll();
                import->vrm = readVRM(data);
            }
        }
        import->model = readModel(import->path);
        // as the client reads it when it's worn: glTF by the Assimp loader, which knows a VRM's bones, face and springs
        HFMModel::Pointer wornModel = import->model;
        if (isGltf && suffix != "vrm" && !data.isEmpty()) {
            HFMModel::Pointer assimpModel = AssimpSerializer().read(data, QVariantHash(), QUrl::fromLocalFile(import->path));
            if (assimpModel) {
                wornModel = assimpModel;
            }
        }
        if (wornModel) {
            const QStringList morphTargetNames = isGltf ? autorig::gltfMorphTargetNames(data) : QStringList(wornModel->blendshapeChannelNames);
            autorig::bakeFst(*wornModel, import->vrm, morphTargetNames, import->fstLines, import->found);
        }
        if (import->model) {
            import->body = autorig::gatherBody(*import->model);
            const QString eyelids = autorig::openEyelids(import->body, *import->model);
            if (!eyelids.isEmpty()) {
                qCDebug(interfaceapp) << "Import Avatar:" << eyelids;
            }
            import->rig = classify(wornModel ? *wornModel : *import->model, import->vrm, import->why);
        }
        qCDebug(interfaceapp) << "Import Avatar: read" << import->path << (import->model ? "" : "(not readable)");
        QMetaObject::invokeMethod(qApp, [import]() {
            decide(import);
        }, Qt::QueuedConnection);
    });
}

void AvatarImporter::putInMyFiles(const QUrl& fstUrl) {
    if (!fstUrl.isLocalFile()) {
        if (fstUrl.scheme() == "qrc") {
            announce("Nothing to put in My Files", "The default avatar is in every viewer already.");
        } else {
            announce("Your avatar is on the web already", "Everyone can see it.");
        }
        return;
    }
    shareAvatar(fstUrl.toLocalFile(), avatarName(fstUrl.toLocalFile()));
}
