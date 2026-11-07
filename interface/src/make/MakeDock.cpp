//
//  MakeDock.cpp
//  interface/src/make
//
//  The Make dock's one object: see MakeDock.h and team/editor-ui/MAKE-DOCK-CONTRACT.md.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "MakeDock.h"

#include <string>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QRunnable>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <QtGui/QClipboard>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickImageProvider>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>

#include <MainWindow.h>
#include <MakeBlender.h>
#include <MakeClaude.h>
#include <MakeKey.h>
#include <AccountManager.h>
#include <SettingHandle.h>

#include "Application.h"
#include "InterfaceLogging.h"
#include "MakeLink.h"
#include "editor/Editor.h"
#include "ui/CodeEditor.h"
#include "ui/Panels.h"

namespace {
    const int FOUND_REREAD_MS = 2000;        // the Connect view's rows read again on show or activation, no more often
    const int REAL_INPUT_MS = 500;           // a real-input call must follow the person's own click or key this closely
    const int MAX_SHEET_BYTES = 8 * 1024 * 1024;   // a model's sheet of views (1024 px square), as MakeLink gives it
    const int TERMINAL_QUIET_MS = 45000;     // the Terminal route: a request is over after this long with no activity
    const qint64 ASK_TIMEOUT_MS = 100000;    // MakeLink refuses a question after 100 s
    const qint64 PICTURE_MAX_BYTES = 20 * 1024 * 1024;
    const int PICTURE_MAX_SIDE = 8192;       // checked before decoding
    const int PICTURE_KEEP_SIDE = 2048;      // the clean copy's longest side at most
    const int PICTURES_PER_ADD = 6;
    const QString PROVIDER = QStringLiteral("make");
    const QRegularExpression PICTURE_ID(QStringLiteral("^[0-9a-f]{16}$"));
    const QRegularExpression MODEL_ID(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));

    // under private/: a world script's Settings can neither read nor write them (ScriptSettingsInterface's protected
    // prefixes; security R1, 11-07). The keys before that, under Make/, are dropped once, never read.
    Setting::Handle<QString> routeSetting { QStringList { "private", "Make", "route" }, QString("key") };
    Setting::Handle<QString> allowedSetupSetting { QStringList { "private", "Make", "allowedSetup" }, QString() };
    // the Claude-plan route's chat in the panel (true) or typing in Terminal (the fallback); private as the rest
    Setting::Handle<bool> chatSetting { QStringList { "private", "Make", "chat" }, true };
    const int NOT_GOT_IT_MS = 30000;   // the first message of a Connect not taken up by Claude this long: not got
    const QString SENT_TO_CLAUDE = QStringLiteral("sent to Claude");   // MakeLink's activity when an event goes out
    Setting::Handle<QString> oldRouteSetting { QStringList { "Make", "route" }, QString() };
    Setting::Handle<QString> oldAllowedSetupSetting { QStringList { "Make", "allowedSetup" }, QString() };
    // the Allow's words as ConnectView shows them: a new version whenever they change, so changed words ask again
    // (security R2)
    const QString ALLOW_WORDS = QStringLiteral("allow words 2026-11-07 channels");
    // the link's socket or pipe in the set-up text: a fresh pipe name at each Connect on Windows, so masked in the hash
    const QRegularExpression LINK_SOCKET(QStringLiteral("(\"--socket\",\\s*\")[^\"]*(\")"));

    QString hashOf(const QString& text) {
        return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    QString homeShortened(const QString& path) {
        const QString home = QDir::homePath();
        return path.startsWith(home + "/") ? "~" + path.mid(home.length()) : path;
    }

    bool insideFolder(const QString& path, const QString& folder) {
        const QString root = QFileInfo(folder).canonicalFilePath();
        const QString file = QFileInfo(path).canonicalFilePath();
        return !root.isEmpty() && !file.isEmpty() && file.startsWith(root + "/");
    }

    // the dock's pictures, from MakeDock's own files only, decoded off the main thread
    QMutex servedMutex;   // guards MakeDock's model id -> sheet map for the provider's thread

    class MakeImageResponse : public QQuickImageResponse, public QRunnable {
    public:
        MakeImageResponse(const QString& file, bool frontOnly, const QSize& requested) :
            _file(file), _frontOnly(frontOnly), _requested(requested) {
            setAutoDelete(false);
        }
        QQuickTextureFactory* textureFactory() const override {
            return QQuickTextureFactory::textureFactoryForImage(_image);
        }
        QString errorString() const override { return _error; }
        void run() override {
            if (_file.isEmpty()) {
                _error = "not Make's";
            } else {
                QImageReader reader(_file);
                reader.setDecideFormatFromContent(true);
                const QSize size = reader.size();
                if (!size.isValid() || size.width() > PICTURE_MAX_SIDE || size.height() > PICTURE_MAX_SIDE) {
                    _error = "not a picture Make can show";
                } else {
                    if (_frontOnly) {
                        // a model's sheet is 2x2 views: the front is the top left one
                        reader.setClipRect(QRect(0, 0, size.width() / 2, size.height() / 2));
                    }
                    _image = reader.read();
                    if (_image.isNull()) {
                        _error = reader.errorString();
                    } else if (_requested.isValid() && !_requested.isEmpty() &&
                               (_image.width() > _requested.width() || _image.height() > _requested.height())) {
                        _image = _image.scaled(_requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                }
            }
            emit finished();
        }

    private:
        QString _file;
        bool _frontOnly;
        QSize _requested;
        QImage _image;
        QString _error;
    };

    class MakeImageProvider : public QQuickAsyncImageProvider {
    public:
        // id: "sheet/<model id>", "thumb/<model id>" or "picture/<picture id>"
        QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override {
            const QString what = id.section('/', 0, 0);
            const QString which = id.section('/', 1);
            QString file;
            if (auto dock = DependencyManager::get<MakeDock>()) {
                file = dock->servedFile(what, which);
            }
            auto response = new MakeImageResponse(file, what == "thumb", requestedSize);
            QThreadPool::globalInstance()->start(response, -1);   // after the engine's loading jobs (performance)
            return response;
        }
    };
}

// ---- MakeDockRows ----

int MakeDockRows::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : _rows.size();
}

QVariant MakeDockRows::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= _rows.size()) {
        return QVariant();
    }
    const Row& row = _rows.at(index.row());
    switch (role) {
        case KindRole: return row.kind;
        case RequestRole: return row.request;
        case TextRole: return row.text;
        case DetailRole: return row.detail;
        case ItemIdRole: return row.itemId;
        case AnsweredRole: return row.answered;
        case ViewsRole: return row.views;
        case ThumbRole: return row.thumb;
        case CostRole: return row.cost;
        case FoldedRole: return row.folded;
        case UndoneRole: return row.undone;
        case TimeRole: return row.time;
        default: return QVariant();
    }
}

QHash<int, QByteArray> MakeDockRows::roleNames() const {
    return {
        { KindRole, "kind" }, { RequestRole, "request" }, { TextRole, "text" }, { DetailRole, "detail" },
        { ItemIdRole, "itemId" }, { AnsweredRole, "answered" }, { ViewsRole, "views" }, { ThumbRole, "thumb" },
        { CostRole, "cost" }, { FoldedRole, "folded" }, { UndoneRole, "undone" }, { TimeRole, "time" }
    };
}

void MakeDockRows::append(const Row& row) {
    beginInsertRows(QModelIndex(), _rows.size(), _rows.size());
    _rows.append(row);
    endInsertRows();
    emit countChanged();
}

void MakeDockRows::prepend(const Row& row) {
    beginInsertRows(QModelIndex(), 0, 0);
    _rows.prepend(row);
    endInsertRows();
    emit countChanged();
}

void MakeDockRows::clear() {
    beginResetModel();
    _rows.clear();
    endResetModel();
    emit countChanged();
}

void MakeDockRows::update(const std::function<bool(const Row&)>& match, const std::function<void(Row&)>& change) {
    for (int i = 0; i < _rows.size(); i++) {
        if (match(_rows.at(i))) {
            change(_rows[i]);
            emit dataChanged(index(i), index(i));
        }
    }
}

// ---- MakeDock ----

bool MakeDock::isAvailable() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return CodeEditor::isAvailable();
#else
    return false;
#endif
}

MakeDock::MakeDock() {
    // nothing looked up until the dock first opens (watchWindow): no Keychain, Claude Code or Blender look-ups at start
    oldRouteSetting.remove();
    oldAllowedSetupSetting.remove();
    _route = routeSetting.get() == "terminal" ? QString("terminal") : QString("key");

    auto link = DependencyManager::get<MakeLink>();
    QObject::connect(link.data(), &MakeLink::stateChanged, this, &MakeDock::linkStateChanged);
    // an Allow covers one signed-in account (allowHash)
    QObject::connect(DependencyManager::get<AccountManager>().data(), &AccountManager::usernameChanged, this,
                     [this] { emit setupTextChanged(); });
    QObject::connect(link.data(), &MakeLink::activity, this, [this](int request, const QString& line) {
        if (line == SENT_TO_CLAUDE) {
            _sentWaiting = true;
            _sentRequest = request;
            if (!_channelProven && !_notGotItTimer.isActive()) {
                _notGotItTimer.start();
            }
            emit channelChanged();
        } else {
            channelTakenUp(request);
        }
        if (request > _doneRequest && _working == false) {
            _costSoFar = -1.0;
        }
        // the Terminal route: a new number is a new request; the one before it is over
        for (const auto& row : _timeline.rows()) {
            if (row.request < request && row.request > _doneRequest && row.kind != "done") {
                requestDone(row.request, -1.0);
                break;
            }
        }
        MakeDockRows::Row row;
        row.kind = "activity";
        row.request = request;
        row.text = clean(line);
        addRow(row);
        setWorking(true);
        if (_route == "terminal") {
            _terminalQuiet.start();
        }
    });
    QObject::connect(link.data(), &MakeLink::askPerson, this, [this](int id, const QString& what, const QString& exactly) {
        MakeDockRows::Row row;
        row.kind = "question";
        row.request = DependencyManager::get<MakeLink>()->currentRequest();
        row.text = clean(what);
        row.detail = clean(exactly);
        row.itemId = QString::number(id);
        if (_testRefusesAsks) {
            // a test's connection: no at once, shown as the person's no would be
            qCWarning(interfaceapp) << "Make: a test answered no to question" << id;
            row.answered = "refused";
            addRow(row);
            DependencyManager::get<MakeLink>()->answer(id, false);
            return;
        }
        _askTimes.insert(id, QDateTime::currentMSecsSinceEpoch());
        _askTimeouts.start();
        addRow(row);
    });
    QObject::connect(link.data(), &MakeLink::made, this, [this](int request, const QVariantMap& card) {
        static int cards = 0;
        MakeDockRows::Row row;
        row.request = request;
        const QString kind = card.value("kind").toString();
        const QString name = clean(card.value("name").toString()).left(80);
        if (kind == "script") {
            row.kind = "script";
            row.entityId = card.value("id").toString();
            row.which = card.value("side").toString() == "serverScripts" ? QString("serverScripts") : QString("script");
            row.text = (row.which == "script" ? QString("Entity script") : QString("Server script")) +
                (name.isEmpty() ? QString() : " on " + name);
            row.detail = clean(QFileInfo(card.value("file").toString()).fileName()).left(80);
            row.itemId = "card-" + QString::number(++cards);
        } else if (kind == "model") {
            const QString model = card.value("model").toString();
            const QString sheet = keepSheet(model, card.value("views"));
            row.kind = "model";
            row.text = "3D model";
            row.detail = name;
            row.itemId = MODEL_ID.match(model).hasMatch() ? model : "card-" + QString::number(++cards);
            if (MODEL_ID.match(model).hasMatch() && insideFolder(sheet, DependencyManager::get<MakeLink>()->folder())) {
                QMutexLocker lock(&servedMutex);
                _sheets.insert(model, QFileInfo(sheet).canonicalFilePath());
                row.views = QStringList { "image://make/sheet/" + model };
                row.thumb = "image://make/thumb/" + model;
            }
        } else {
            row.kind = "thing";
            row.text = name.isEmpty() ? QString("A new thing") : name;
            row.entityId = card.value("id").toString();
            row.itemId = row.entityId;
        }
        addRow(row);
        addMade(row);
    });
    QObject::connect(link.data(), &MakeLink::reply, this, [this](int request, const QString& text) {
        channelTakenUp(request);
        MakeDockRows::Row row;
        row.kind = "reply";
        row.request = request;
        row.text = clean(MakeClaude::redact(text));
        addRow(row);
    });
    QObject::connect(link.data(), &MakeLink::requestCost, this, [this](int request, double usd) {
        Q_UNUSED(request);
        _costSoFar = usd;
        emit workingChanged();
    });

    auto claude = link->claude();
    QObject::connect(claude, &MakeClaude::started, this, [this](const QString& model) {
        Q_UNUSED(model);
        _starting = false;
        emit stateChanged();
    });
    // MakeLink's own handler runs first (connected first), so the request's cost has arrived
    QObject::connect(claude, &MakeClaude::turnEnded, this,
                     [this](double totalCostUsd, bool error, const QString& result, const QString& subtype) {
        Q_UNUSED(totalCostUsd);
        const int request = DependencyManager::get<MakeLink>()->currentRequest();
        if (error && subtype != "error_max_budget_usd" && !result.isEmpty()) {
            MakeDockRows::Row row;
            row.kind = "activity";
            row.request = request;
            row.text = "Stopped: " + clean(MakeClaude::redact(result)).left(300);
            addRow(row);
        }
        requestDone(request, _costSoFar);
        setWorking(false);
    });
    QObject::connect(claude, &MakeClaude::ended, this, [this](const QString& why) {
        _starting = false;
        if (!why.isEmpty()) {
            _reason = clean(MakeClaude::redact(why)).left(300);
        }
        setWorking(false);
        emit stateChanged();
    });

    _notGotItTimer.setSingleShot(true);
    _notGotItTimer.setInterval(NOT_GOT_IT_MS);
    QObject::connect(&_notGotItTimer, &QTimer::timeout, this, [this] {
        if (_sentWaiting && !_channelProven) {
            _notGotIt = true;
            qCWarning(interfaceapp) << "Make: Claude didn't take up the first message in" << NOT_GOT_IT_MS / 1000 << "s"
                                    << "(channel" << channelFlag() << ")";
            emit channelChanged();
        }
    });
    _terminalQuiet.setSingleShot(true);
    _terminalQuiet.setInterval(TERMINAL_QUIET_MS);
    QObject::connect(&_terminalQuiet, &QTimer::timeout, this, [this] {
        const int request = DependencyManager::get<MakeLink>()->currentRequest();
        requestDone(request, -1.0);
        setWorking(false);
    });
    _askTimeouts.setInterval(5000);
    QObject::connect(&_askTimeouts, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = _askTimes.begin(); it != _askTimes.end();) {
            if (now - it.value() > ASK_TIMEOUT_MS) {
                const QString id = QString::number(it.key());
                _timeline.update([&](const MakeDockRows::Row& row) { return row.kind == "question" && row.itemId == id &&
                                                                            row.answered.isEmpty(); },
                                 [](MakeDockRows::Row& row) { row.answered = "timed out"; });
                it = _askTimes.erase(it);
            } else {
                ++it;
            }
        }
        if (_askTimes.isEmpty()) {
            _askTimeouts.stop();
        }
    });
}

MakeDock::~MakeDock() {
}

QString MakeDock::state() const {
    auto link = DependencyManager::get<MakeLink>();
    const QString linkState = link->stateName();   // off, listening, connected, failed, limit
    if (linkState == "failed" || linkState == "limit" || linkState == "off") {
        return linkState;
    }
    if (_route == "key") {
        if (!_reason.isEmpty() && !link->claude()->isRunning()) {
            return "failed";   // Claude Code ended by itself, or couldn't start
        }
        return _starting || linkState == "listening" ? QString("starting") : QString("connected");
    }
    return linkState;
}

QString MakeDock::reason() const {
    auto link = DependencyManager::get<MakeLink>();
    const QString linkState = link->stateName();
    if (linkState == "failed" || linkState == "limit") {
        return clean(MakeClaude::redact(link->reason())).left(300);
    }
    // listening after the link refused a Claude Code started with another line (security C9): its plain line
    if (linkState == "listening" && !link->reason().isEmpty()) {
        return clean(link->reason()).left(300);
    }
    return _reason;
}

QString MakeDock::keyStoreName() const {
#if defined(Q_OS_MAC)
    return "your Keychain";
#elif defined(Q_OS_WIN)
    return "Windows Credential Manager";
#else
    return "your keyring";
#endif
}

QString MakeDock::setupText() const {
    auto link = DependencyManager::get<MakeLink>();
    return "Make folder: " + homeShortened(link->folder()) + "\n\n" +
        "Claude Code's tools (Darkride's only):\n" + link->mcpConfigText() + "\n\n" +
        "What Claude Code may do (no commands, no web, no files):\n" + link->settingsText();
}

QString MakeDock::allowHash() const {
    // what one Allow covers (security R2, R4): the Allow's words, the route, what Make sets up and the start line, the
    // per-Connect link name masked, and the signed-in account; any change asks again with the whole card
    QString setup = setupText();
    setup.replace(LINK_SOCKET, QStringLiteral("\\1<link>\\2"));
    const QString account = DependencyManager::get<AccountManager>()->getAccountInfo().getUsername();
    return hashOf(ALLOW_WORDS + "\n" + _route + "\n" + account + "\n" + setup + "\n" + startLine());
}

bool MakeDock::isSetupChanged() const {
    return lastAllowedSetupHash() != allowHash();
}

void MakeDock::stopAllowing() {
    allowedSetupSetting.remove();
    emit setupTextChanged();
}

QString MakeDock::lastAllowedSetupHash() const {
    return allowedSetupSetting.get();
}

QString MakeDock::startLine() const {
    return DependencyManager::get<MakeLink>()->startLine(chat());
}

bool MakeDock::chat() const {
    return chatSetting.get();
}

QString MakeDock::channelFlag() const {
    return DependencyManager::get<MakeLink>()->channelFlag();
}

QString MakeDock::claudeVersion() const {
    return clean(DependencyManager::get<MakeLink>()->claudeVersion()).left(40);
}

bool MakeDock::setChat(bool chat) {
    if (!takeRealInput("setChat")) {
        return false;
    }
    if (chat != chatSetting.get()) {
        chatSetting.set(chat);
        emit routeChanged();
        emit setupTextChanged();   // another start line: the next Connect asks with the whole card (allowHash)
    }
    return true;
}

void MakeDock::channelTakenUp(int request) {
    if (_sentWaiting && request >= _sentRequest) {
        _sentWaiting = false;
        _notGotIt = false;
        _channelProven = true;
        _notGotItTimer.stop();
        emit channelChanged();
    }
}

void MakeDock::resetChannel() {
    _notGotItTimer.stop();
    if (_sentWaiting || _notGotIt || _channelProven) {
        _sentWaiting = false;
        _notGotIt = false;
        _channelProven = false;
        emit channelChanged();
    }
}

void MakeDock::readFound() {
    auto link = DependencyManager::get<MakeLink>();
    QString why;
    const bool claudeFound = !MakeClaude::findClaude().isEmpty();
    const bool blenderFound = !MakeBlender::findBlender(why).isEmpty();
    const bool sealed = link->blenderOffered();
    const QString blenderWhy = clean(link->blenderWhy());
    if (claudeFound != _claudeFound || blenderFound != _blenderFound || sealed != _blenderSealed || blenderWhy != _blenderWhy) {
        _claudeFound = claudeFound;
        _blenderFound = blenderFound;
        _blenderSealed = sealed;
        _blenderWhy = blenderWhy;
        emit foundChanged();
    }
}

void MakeDock::linkStateChanged() {
    readFound();   // while connected, blenderSealed is what that Connect decided
    if (DependencyManager::get<MakeLink>()->stateName() != "connected") {
        resetChannel();   // a new Claude Code proves its channel again
    }
    if (DependencyManager::get<MakeLink>()->stateName() == "off") {
        _starting = false;
        setWorking(false);
    }
    emit stateChanged();
    emit setupTextChanged();
}

// ---- the person's calls ----

bool MakeDock::takeRealInput(const char* call) {
    if (_inputUnused && _sinceInput.isValid() && _sinceInput.elapsed() <= REAL_INPUT_MS) {
        _inputUnused = false;
        return true;
    }
    qCWarning(interfaceapp) << "Make: refused" << call << "(no real click or key in the dock just now)";
    return false;
}

bool MakeDock::connect() {
    if (!takeRealInput("connect")) {
        return false;
    }
    return connectNow();
}

bool MakeDock::testConnectTerminal() {
    // tests only (Test.makeDockConnect, which exists only with --testScript): never the key route, and every question
    // answered no from now on, so a kit never gets a silent yes (scripting's rule)
    _testRefusesAsks = true;
    if (_route != "terminal") {
        _route = "terminal";   // not saved: the person's own route setting stays as it was
        emit routeChanged();
    }
    qCWarning(interfaceapp) << "Make: connected by a test (the Terminal route; every question is answered no)";
    return connectNow();
}

namespace {
    // the shown one of the items by that name (the start line is in two places, one shown at a time), else the first
    QQuickItem* testFind(QQuickWindow* window, const QString& objectName) {
        // only the dock's own named parts a kit needs: never "" (every unnamed item, the key's field among them)
        static const QStringList NAMES { "makeStartLine", "makeCopy", "makeComposer" };
        if (!window || !NAMES.contains(objectName)) {
            return nullptr;
        }
        const auto items = window->contentItem()->findChildren<QQuickItem*>(objectName);
        for (QQuickItem* item : items) {
            if (item->isVisible()) {
                return item;
            }
        }
        return items.isEmpty() ? nullptr : items.first();
    }
}

QVariantMap MakeDock::testItem(const QString& objectName) const {
    QQuickItem* item = testFind(_window, objectName);
    if (!item) {
        return { { "found", false } };
    }
    const QRectF place = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return { { "found", true }, { "visible", item->isVisible() }, { "x", place.x() }, { "y", place.y() },
             { "width", place.width() }, { "height", place.height() }, { "text", item->property("text") },
             { "selectedText", item->property("selectedText") } };
}

bool MakeDock::testMouse(const QString& objectName, const QPointF& from, const QPointF& to) {
    QQuickItem* item = testFind(_window, objectName);
    if (!item || !item->isVisible()) {
        return false;
    }
    auto at = [item](const QPointF& fraction) {
        return item->mapToScene(QPointF(fraction.x() * item->width(), fraction.y() * item->height()));
    };
    auto send = [this](QEvent::Type type, const QPointF& point, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, point, _window->mapToGlobal(point), Qt::LeftButton, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(_window, &event);
    };
    const QPointF start = at(from);
    const QPointF end = at(to);
    send(QEvent::MouseButtonPress, start, Qt::LeftButton);
    const int steps = start == end ? 0 : 8;
    for (int i = 1; i <= steps; ++i) {
        send(QEvent::MouseMove, start + (end - start) * (double(i) / steps), Qt::LeftButton);
    }
    send(QEvent::MouseButtonRelease, end, Qt::NoButton);
    return true;
}

bool MakeDock::connectNow() {
    auto link = DependencyManager::get<MakeLink>();
    _reason.clear();
    _starting = false;
    if (!link->connectLink()) {
        readFound();
        emit stateChanged();
        return false;
    }
    allowedSetupSetting.set(allowHash());   // what the person allowed, as written
    if (_route == "key") {
        QString why;
        if (link->claude()->isRunning()) {
            _starting = false;   // already running (a Connect again while connected)
        } else if (link->startClaude(why)) {
            _starting = true;    // until MakeClaude says started
        } else {
            _reason = clean(MakeClaude::redact(why)).left(300);
        }
    }
    readFound();
    emit stateChanged();
    emit setupTextChanged();
    return true;
}

void MakeDock::disconnect() {
    DependencyManager::get<MakeLink>()->disconnectLink();
    _starting = false;
    _terminalQuiet.stop();
    setWorking(false);
    emit stateChanged();
}

bool MakeDock::setRoute(const QString& route) {
    if (route != "key" && route != "terminal") {
        return false;
    }
    if (!takeRealInput("setRoute")) {
        return false;
    }
    if (route != _route) {
        _route = route;
        routeSetting.set(route);
        emit routeChanged();
        emit stateChanged();
        emit setupTextChanged();   // an Allow covers one route (allowHash)
    }
    return true;
}

bool MakeDock::storeKey(const QString& text) {
    if (!takeRealInput("storeKey")) {
        return false;
    }
    QString why;
    const bool stored = MakeKey::store(text.trimmed(), why);
    if (!stored) {
        _reason = clean(why).left(300);
        emit stateChanged();
    } else if (!_reason.isEmpty() && state() != "failed") {
        _reason.clear();
        emit stateChanged();
    }
    const bool hasKey = MakeKey::isStored();
    if (hasKey != _hasKey) {
        _hasKey = hasKey;
        emit keyChanged();
    }
    return stored;
}

bool MakeDock::forgetKey() {
    if (!takeRealInput("forgetKey")) {
        return false;
    }
    const bool forgot = MakeKey::forget();
    const bool hasKey = MakeKey::isStored();
    if (hasKey != _hasKey) {
        _hasKey = hasKey;
        emit keyChanged();
    }
    return forgot;
}

int MakeDock::send(const QString& text, const QStringList& pictureIds) {
    // the key route; or the Claude-plan route's chat, while a Claude Code started with the chat line is connected
    const bool chatting = _route == "terminal" && chat() && state() == "connected" && channelFlag() != "off";
    if ((_route != "key" && !chatting) || text.trimmed().isEmpty() || _working) {
        return 0;
    }
    if (!takeRealInput("send")) {
        return 0;
    }
    QStringList ids;
    QStringList views;
    for (const auto& id : pictureIds) {
        if (!servedFile("picture", id).isEmpty() && !ids.contains(id)) {
            ids << id;
            views << pictureView(id);
        }
    }
    const int request = DependencyManager::get<MakeLink>()->send(text, ids);
    if (request <= 0) {
        return 0;
    }
    _costSoFar = -1.0;
    MakeDockRows::Row row;
    row.kind = "ask";
    row.request = request;
    row.text = clean(text);
    row.views = views;
    addRow(row);
    setWorking(true);
    return request;
}

bool MakeDock::answer(const QString& askId, bool allow) {
    bool isNumber = false;
    const int id = askId.toInt(&isNumber);
    if (!isNumber || !_askTimes.contains(id)) {
        return false;   // not a question waiting for an answer
    }
    if (!takeRealInput("answer")) {
        return false;
    }
    DependencyManager::get<MakeLink>()->answer(id, allow);
    _askTimes.remove(id);
    _timeline.update([&](const MakeDockRows::Row& row) { return row.kind == "question" && row.itemId == askId; },
                     [&](MakeDockRows::Row& row) { row.answered = allow ? "allowed" : "refused"; });
    return true;
}

void MakeDock::stop() {
    DependencyManager::get<MakeLink>()->stop();
    if (_route == "terminal") {
        _terminalQuiet.stop();
        requestDone(DependencyManager::get<MakeLink>()->currentRequest(), -1.0);
        setWorking(false);
    }
}

bool MakeDock::undoRequest(int request) {
    if (_working || request <= 0 || request != _doneRequest) {
        return false;   // only the newest finished request, never while Claude works
    }
    bool undone = false;
    for (const auto& row : _timeline.rows()) {
        if (row.request == request && row.undone) {
            undone = true;
        }
    }
    if (undone || !takeRealInput("undoRequest")) {
        return false;
    }
    DependencyManager::get<MakeLink>()->undoRequest(request);
    _timeline.update([&](const MakeDockRows::Row& row) { return row.request == request; },
                     [](MakeDockRows::Row& row) { row.undone = true; });
    _made.update([&](const MakeDockRows::Row& row) { return row.request == request; },
                 [](MakeDockRows::Row& row) { row.undone = true; });
    return true;
}

void MakeDock::openInCode(const QString& cardId) {
    for (const auto& row : _made.rows()) {
        if (row.itemId == cardId && row.kind == "script" && !row.entityId.isEmpty()) {
            if (auto code = DependencyManager::get<CodeEditor>()) {
                code->open(QUuid(row.entityId), row.which);
            }
            return;
        }
    }
}

void MakeDock::showThing(const QString& itemId) {
    QString entityId;
    for (const auto& row : _made.rows()) {
        if (row.itemId == itemId && !row.entityId.isEmpty()) {
            entityId = row.entityId;
        }
    }
    const QUuid id(entityId);
    if (id.isNull()) {
        return;
    }
    if (auto editor = DependencyManager::get<Editor>()) {
        if (!editor->isActive()) {
            editor->setActive(true);
        }
        editor->select({ id });   // a deleted thing selects nothing, and nothing is framed
        editor->frameSelection();
    }
}

QString MakeDock::pictureView(const QString& id) const {
    return PICTURE_ID.match(id).hasMatch() ? "image://make/picture/" + id : QString();
}

void MakeDock::giveBackKeyboard() {
    if (auto viewport = qApp->getPrimaryWidget()) {
        viewport->activateWindow();
        viewport->setFocus(Qt::OtherFocusReason);
    }
}

bool MakeDock::copyLine(const QString& which) {
    // a whole line of Make's own, read here: never a part (a part of the start line could leave out --restricted) and
    // never text from QML (security, 11-07)
    const QString text = which == "start" ? startLine() : which == "setup" ? setupText() : QString();
    if (text.isEmpty()) {
        qCWarning(interfaceapp) << "Make: refused copyLine" << which.left(20);
        return false;
    }
    if (!takeRealInput("copyLine")) {
        return false;
    }
    QGuiApplication::clipboard()->setText(text);
    return true;
}

QString MakeDock::testCopyLine(const QString& which) {
    // tests only (Test.makeDockCopy, with --testScript): copyLine's own clipboard write without the real-input check,
    // so a kit can compare the system's clipboard with the start line, byte for byte (security C6); only in pm's
    // clipboard slot, as the clipboard is the person's
    const QString text = which == "start" ? startLine() : which == "setup" ? setupText() : QString();
    if (text.isEmpty()) {
        return QString();
    }
    qCWarning(interfaceapp) << "Make: a test copied" << which << "to the clipboard";
    QGuiApplication::clipboard()->setText(text);
    return text;
}

void MakeDock::showMoreMenu(qreal x, qreal y) {
    auto menu = new QMenu(qApp->getWindow());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto disconnectAction = menu->addAction("Disconnect");
    disconnectAction->setEnabled(DependencyManager::get<MakeLink>()->stateName() != "off");
    QObject::connect(disconnectAction, &QAction::triggered, this, &MakeDock::disconnect);
    menu->popup(QPoint(qRound(x), qRound(y)));
}

// ---- pictures ----

QString MakeDock::keepSheet(const QString& model, const QVariant& views) const {
    // MakeLink gives a model's views as the sheet's own bytes (MakeBlender::drawViews: one JPEG or PNG); MakeDock keeps
    // them in the Make folder's sheets/, by the model's id, and serves them from there. A path is taken as it is (the
    // caller checks it's inside the Make folder). One write per made model, a few hundred KB.
    if (views.userType() != QMetaType::QByteArray) {
        return views.toString();
    }
    const QByteArray bytes = views.toByteArray();
    if (!MODEL_ID.match(model).hasMatch() || bytes.isEmpty() || bytes.size() > MAX_SHEET_BYTES) {
        return QString();
    }
    const QString folder = DependencyManager::get<MakeLink>()->folder() + "/sheets";
    if (!QDir().mkpath(folder)) {
        return QString();
    }
    const QString file = folder + "/" + model + (bytes.startsWith("\x89PNG") ? ".png" : ".jpg");
    QFile::remove(file);   // written fresh: never through a link left in its place
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(bytes) != bytes.size()) {
        out.remove();
        return QString();
    }
    return file;
}

QString MakeDock::picturesFolder() const {
    return DependencyManager::get<MakeLink>()->folder() + "/pictures";
}

QString MakeDock::servedFile(const QString& what, const QString& id) const {
    auto link = DependencyManager::get<MakeLink>();
    const QString folder = link ? link->folder() : QString();
    if (folder.isEmpty()) {
        return QString();
    }
    QString file;
    if (what == "sheet" || what == "thumb") {
        QMutexLocker lock(&servedMutex);
        file = _sheets.value(id);
    } else if (what == "picture" && PICTURE_ID.match(id).hasMatch()) {
        for (const auto& extension : { ".png", ".jpg" }) {
            const QString candidate = folder + "/pictures/" + id + extension;
            if (QFileInfo::exists(candidate)) {
                file = candidate;
                break;
            }
        }
    }
    return !file.isEmpty() && insideFolder(file, folder) ? QFileInfo(file).canonicalFilePath() : QString();
}

void MakeDock::addPictures() {
    if (!takeRealInput("addPictures") || _chooser) {
        return;
    }
    auto chooser = new QFileDialog(qApp->getWindow(), "Add Pictures", QDir::homePath(),
                                   "Pictures (*.png *.jpg *.jpeg *.webp)");
    chooser->setFileMode(QFileDialog::ExistingFiles);
    chooser->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(chooser, &QFileDialog::filesSelected, this, [this](const QStringList& files) {
        cleanPictures(files);
    });
    _chooser = chooser;
    chooser->open();
}

bool MakeDock::dropPictures(const QVariantList& urls) {
    QList<QUrl> given;
    for (const auto& url : urls) {
        given << url.toUrl();
    }
    const bool real = _sinceDrop.isValid() && _sinceDrop.elapsed() <= REAL_INPUT_MS && !_droppedUrls.isEmpty() &&
        given == _droppedUrls;
    _droppedUrls.clear();   // each drop is used once
    if (!real) {
        qCWarning(interfaceapp) << "Make: refused dropPictures (no real drop of these in the dock just now)";
        return false;
    }
    QStringList files;
    for (const auto& url : given) {
        if (url.isLocalFile()) {
            files << url.toLocalFile();
        }
    }
    cleanPictures(files);
    return true;
}

// Each picture checked before it's decoded (a file, its size, PNG/JPEG/WebP, its pixel size), then a clean copy (no
// metadata, at most 2048 px a side) written as <Make folder>/pictures/<id>.png, off the main thread.
void MakeDock::cleanPictures(const QStringList& files) {
    const QString folder = picturesFolder();
    QStringList chosen = files.mid(0, PICTURES_PER_ADD);
    const bool tooMany = files.size() > PICTURES_PER_ADD;
    QPointer<MakeDock> self(this);
    QThreadPool::globalInstance()->start([self, folder, chosen, tooMany] {
        QStringList ids;
        QStringList refusals;
        if (tooMany) {
            refusals << QString("Only %1 pictures at a time: the first %1 were taken.").arg(PICTURES_PER_ADD);
        }
        QDir().mkpath(folder);
        for (const auto& file : chosen) {
            const QFileInfo info(file);
            const QString name = MakeDock::clean(info.fileName()).left(60);
            if (!info.isFile() || info.size() <= 0) {
                refusals << name + " isn't a file Make can read.";
                continue;
            }
            if (info.size() > PICTURE_MAX_BYTES) {
                refusals << name + " is too big (20 MB at most).";
                continue;
            }
            QImageReader reader(file);
            reader.setDecideFormatFromContent(true);
            const QByteArray format = reader.format().toLower();
            if (format != "png" && format != "jpeg" && format != "jpg" && format != "webp") {
                refusals << name + " isn't a PNG, JPEG or WebP picture.";
                continue;
            }
            const QSize size = reader.size();
            if (!size.isValid() || size.width() > PICTURE_MAX_SIDE || size.height() > PICTURE_MAX_SIDE) {
                refusals << name + " is too large a picture (8192 pixels a side at most).";
                continue;
            }
            if (size.width() > PICTURE_KEEP_SIDE || size.height() > PICTURE_KEEP_SIDE) {
                reader.setScaledSize(size.scaled(PICTURE_KEEP_SIDE, PICTURE_KEEP_SIDE, Qt::KeepAspectRatio));
            }
            QImage image = reader.read();
            if (image.isNull()) {
                refusals << name + " couldn't be read as a picture.";
                continue;
            }
            const QString id = QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
            QImageWriter writer(folder + "/" + id + ".png", "png");
            if (!writer.write(image.convertToFormat(QImage::Format_ARGB32))) {
                refusals << name + " couldn't be kept: " + writer.errorString();
                continue;
            }
            ids << id;
        }
        QMetaObject::invokeMethod(qApp, [self, ids, refusals] {
            if (!self) {
                return;
            }
            for (const auto& refusal : refusals) {
                emit self->pictureRefused(refusal);
            }
            if (!ids.isEmpty()) {
                QVariantList list;
                for (const auto& id : ids) {
                    list << id;
                }
                emit self->picturesAdded(list);
            }
        }, Qt::QueuedConnection);
    }, -1);   // below the default: a waiting texture or mesh job goes first (performance)
}

// ---- the timeline ----

void MakeDock::addRow(MakeDockRows::Row row) {
    row.time = QDateTime::currentMSecsSinceEpoch();
    _timeline.append(row);
}

void MakeDock::addMade(const MakeDockRows::Row& row) {
    _made.prepend(row);
}

void MakeDock::requestDone(int request, double cost) {
    if (request <= 0 || request <= _doneRequest) {
        return;
    }
    bool any = false;
    for (const auto& row : _timeline.rows()) {
        if (row.request == request) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    _doneRequest = request;
    MakeDockRows::Row row;
    row.kind = "done";
    row.request = request;
    row.cost = cost;
    addRow(row);
}

void MakeDock::setWorking(bool working) {
    if (working != _working) {
        _working = working;
        emit workingChanged();
    }
}

// ---- the dock's window ----

void MakeDock::watchWindow(QQuickWindow* window, QQmlEngine* engine) {
    // the dock is open for the first time: what Connect shows
    const bool hasKey = MakeKey::isStored();
    if (hasKey != _hasKey) {
        _hasKey = hasKey;
        emit keyChanged();
    }
    readFound();
    if (_window) {
        _window->removeEventFilter(this);
    }
    _window = window;
    if (_window) {
        _window->installEventFilter(this);
    }
    if (engine && !engine->imageProvider(PROVIDER)) {
        engine->addImageProvider(PROVIDER, new MakeImageProvider());   // the engine owns it
    }
}

bool MakeDock::eventFilter(QObject* watched, QEvent* event) {
    if (watched == _window && (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate)) {
        // the dock shown again, or its window back in front: what's on this computer may have changed (Blender just
        // installed), so the Connect view's rows are read again; at most every 2 s, a few file checks
        if (!_sinceFoundRead.isValid() || _sinceFoundRead.elapsed() > FOUND_REREAD_MS) {
            _sinceFoundRead.restart();
            readFound();
        }
    }
    if (watched == _window && event->spontaneous()) {   // from the window system: never a script's or a test's
        switch (event->type()) {
            case QEvent::MouseButtonRelease:
            case QEvent::KeyPress:
                _sinceInput.restart();
                _inputUnused = true;
                break;
            case QEvent::Drop: {
                auto drop = static_cast<QDropEvent*>(event);
                if (drop->mimeData() && drop->mimeData()->hasUrls()) {
                    _droppedUrls = drop->mimeData()->urls();
                    _sinceDrop.restart();
                }
                break;
            }
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

QString MakeDock::clean(const QString& text) {
    // by code point, not by QChar: a tag character is a surrogate pair (an unpaired surrogate comes as U+FFFD)
    const auto points = text.toUcs4();
    std::u32string out;
    out.reserve(points.size());
    for (const auto point : points) {
        const char32_t u = static_cast<char32_t>(point);
        if (u == '\n') {
            out += u;
        } else if (u == '\t') {
            out += U' ';
        } else if (u < 0x20 || (u >= 0x7f && u < 0xa0)) {
            continue;   // control characters
        } else if ((u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069) || u == 0x200e || u == 0x200f ||
                   u == 0x061c) {
            continue;   // bidi overrides, isolates and marks
        } else if (u == 0x2028 || u == 0x2029) {
            continue;   // line and paragraph separators
        } else if ((u >= 0x200b && u <= 0x200d) || u == 0x2060 || u == 0xfeff) {
            continue;   // zero-width characters
        } else if (u >= 0xe0000 && u <= 0xe007f) {
            continue;   // tag characters: invisible, and can carry hidden text (security)
        } else {
            out += u;
        }
    }
    return QString::fromUcs4(out.data(), static_cast<int>(out.size()));
}
