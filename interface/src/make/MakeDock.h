//
//  MakeDock.h
//  interface/src/make
//
//  The Make dock's one object (Make v1, team/editor-ui/MAKE-DOCK-CONTRACT.md): what the dock's QML (MakePanel.qml)
//  sees, set on the Make dock's own QML context only (never the root context, the tablet or a web page). It forwards
//  the person's calls to scripting's MakeLink and MakeClaude, keeps the timeline and the Made list as list models,
//  cleans every string before the QML sees it (security: no control or bidi-override characters), serves the dock's
//  pictures from the Make folder alone (image://make/...), and refuses each call marked "real input" unless a real
//  mouse release or key press reached the dock's own window in the last 500 ms (each one used once; events made by a
//  script or a test don't count).
//
//  It lives on the main thread and emits only there: MakeLink's and MakeClaude's signals reach it with it as the
//  receiver (contract, "Threads").
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once
#ifndef hifi_MakeDock_h
#define hifi_MakeDock_h

#include <functional>

#include <QtCore/QAbstractListModel>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariantList>
#include <QtCore/QVector>

#include <DependencyManager.h>

class QQmlEngine;
class QQuickWindow;
class QFileDialog;

/// One list of the dock's rows: the timeline (oldest first) or the Made tab (newest first). Every row has every role.
class MakeBadge;

class MakeDockRows : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        RequestRole,
        TextRole,
        DetailRole,
        ItemIdRole,
        AnsweredRole,
        ViewsRole,
        ThumbRole,
        CostRole,
        FoldedRole,
        UndoneRole,
        TimeRole
    };
    struct Row {
        QString kind;           // ask, activity, reply, question, thing, script, model, done
        int request { 0 };
        QString text;
        QString detail;
        QString itemId;         // askId for a question, the made thing's id for a card
        QString answered;       // a question: "", allowed, refused, timed out
        QStringList views;      // image://make/ addresses only
        QString thumb;          // a model's front view (image://make/thumb/<id>)
        double cost { -1.0 };
        bool folded { false };
        bool undone { false };
        qint64 time { 0 };      // ms since the epoch, MakeDock's clock
        // MakeDock's own, never a role: what a script card opens
        QString entityId;
        QString which;
    };

    explicit MakeDockRows(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void append(const Row& row);
    void prepend(const Row& row);
    void clear();
    const QVector<Row>& rows() const { return _rows; }
    /// Changes the rows `match` picks with `change`, and says which changed.
    void update(const std::function<bool(const Row&)>& match, const std::function<void(Row&)>& change);

signals:
    void countChanged();

private:
    QVector<Row> _rows;
};

class MakeDock : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString reason READ reason NOTIFY stateChanged)
    Q_PROPERTY(QString route READ route NOTIFY routeChanged)
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY keyChanged)
    Q_PROPERTY(bool claudeFound READ claudeFound NOTIFY foundChanged)
    Q_PROPERTY(bool blenderFound READ blenderFound NOTIFY foundChanged)
    Q_PROPERTY(bool blenderSealed READ blenderSealed NOTIFY foundChanged)
    Q_PROPERTY(QString blenderWhy READ blenderWhy NOTIFY foundChanged)
    Q_PROPERTY(QString keyStoreName READ keyStoreName CONSTANT)
    Q_PROPERTY(QString setupText READ setupText NOTIFY setupTextChanged)
    Q_PROPERTY(bool setupChanged READ isSetupChanged NOTIFY setupTextChanged)
    Q_PROPERTY(QString startLine READ startLine NOTIFY setupTextChanged)
    // the Claude-plan route's chat in the panel over a Claude Code channel (MAKE-CHANNEL-PLAN.md): a choice of its own
    // beside typing in Terminal (the default) and the API key, when offered; what the connected Claude Code said of the
    // channel ("on", "off", "unknown"); its version; a message sent and not yet taken up; the first message not taken
    // up in 30 s
    Q_PROPERTY(bool chat READ chat NOTIFY routeChanged)
    Q_PROPERTY(bool chatOffered READ chatOffered CONSTANT)
    Q_PROPERTY(QString channelFlag READ channelFlag NOTIFY stateChanged)
    Q_PROPERTY(QString claudeVersion READ claudeVersion NOTIFY stateChanged)
    Q_PROPERTY(bool sentWaiting READ sentWaiting NOTIFY channelChanged)
    Q_PROPERTY(bool notGotIt READ notGotIt NOTIFY channelChanged)
    Q_PROPERTY(bool working READ isWorking NOTIFY workingChanged)
    Q_PROPERTY(double costSoFar READ costSoFar NOTIFY workingChanged)
    Q_PROPERTY(QObject* timeline READ timeline CONSTANT)
    Q_PROPERTY(QObject* made READ made CONSTANT)
    // the more menu (•••) is showing: its button is drawn lit, as the Build toolbar's tools while their menu is open
    Q_PROPERTY(bool moreMenuOpen READ moreMenuOpen NOTIFY moreMenuChanged)

public:
    MakeDock();
    ~MakeDock() override;

    /// The Make dock exists in this build (Qt 6, with the Code panel its script cards open in).
    static bool isAvailable();

    QString state() const;
    /// MakeLink's line when it failed or reached its limit; else MakeDock's own (Claude Code's end, a refused key)
    QString reason() const;
    QString route() const { return _route; }
    bool hasKey() const { return _hasKey; }
    bool claudeFound() const { return _claudeFound; }
    bool blenderFound() const { return _blenderFound; }
    bool blenderSealed() const { return _blenderSealed; }
    QString blenderWhy() const { return _blenderWhy; }
    QString keyStoreName() const;
    QString setupText() const;
    bool isSetupChanged() const;
    QString startLine() const;
    bool isWorking() const { return _working; }
    /// A question waits for the person's answer (the Make badge's "answer").
    bool hasQuestion() const { return !_askTimes.isEmpty(); }
    /// The Make badge: the desktop's only Make object (MakeBadge.h).
    MakeBadge* badge() const { return _badge; }
    double costSoFar() const { return _costSoFar; }
    QObject* timeline() { return &_timeline; }
    QObject* made() { return &_made; }
    /// The dock's window is watched for real input (watchWindow was given it).
    bool isWatching() const { return !_window.isNull(); }
    bool chat() const;
    bool chatOffered() const;
    QString channelFlag() const;
    QString claudeVersion() const;
    bool sentWaiting() const { return _sentWaiting; }
    bool notGotIt() const { return _notGotIt; }
    bool moreMenuOpen() const { return _moreMenuOpen; }

    // ---- the dock's calls (contract). "Real input" ones answer false when refused. ----

    /// The Allow in Connect: writes the Make folder (after the person has seen setupText) and listens (Terminal) or
    /// starts Claude Code (key). Real input.
    Q_INVOKABLE bool connect();
    /// Stops everything: Claude Code, the link and Blender, by their own pids.
    Q_INVOKABLE void disconnect();
    /// "key", "terminal" (the Claude plan, typing in Terminal) or "chat" (the Claude plan, typing here; only when
    /// chatOffered), kept in private/Make/route and private/Make/chat (not secrets). Real input.
    Q_INVOKABLE bool setRoute(const QString& route);
    /// Keeps the key (MakeKey); the key is never readable back. Real input.
    Q_INVOKABLE bool storeKey(const QString& text);
    /// Forgets the kept key. Real input.
    Q_INVOKABLE bool forgetKey();
    /// The key route's Send from the dock's own text box: the request's number, 0 when refused. Real input.
    Q_INVOKABLE int send(const QString& text, const QStringList& pictureIds);
    /// The person's answer to a question card. Real input.
    Q_INVOKABLE bool answer(const QString& askId, bool allow);
    Q_INVOKABLE void stop();
    /// Undo for request `request`: only the newest request not undone, once it's done. Real input.
    Q_INVOKABLE bool undoRequest(int request);
    /// A script card's script in the Code panel, by its entity id and which script (never an address).
    Q_INVOKABLE void openInCode(const QString& cardId);
    /// Build mode on, the made thing selected and framed. Changes nothing in the world.
    Q_INVOKABLE void showThing(const QString& itemId);
    /// The file chooser (PNG, JPEG or WebP); the answer comes as picturesAdded or pictureRefused. Real input.
    Q_INVOKABLE void addPictures();
    /// Files dropped on the dock: true only when a real drop of exactly these reached the dock's window just now.
    Q_INVOKABLE bool dropPictures(const QVariantList& urls);
    /// A waiting picture's address from MakeDock's own provider.
    Q_INVOKABLE QString pictureView(const QString& id) const;
    /// The keyboard back to the world (Esc in the dock).
    Q_INVOKABLE void giveBackKeyboard();
    /// Copy onto the clipboard, from C++ (the Assets panel's path, not a QML text item's): the whole of the Terminal
    /// route's start line ("start") or of setupText ("setup"), read here, never a part. Real input.
    Q_INVOKABLE bool copyLine(const QString& which);
    /// "Stop allowing" in the Allow's Details: forgets the Allow, so the next Connect shows the whole card (security R3).
    Q_INVOKABLE void stopAllowing();
    /// The more menu (•••): the app's own menu at (x, y) on the screen, as the Code panel's. Disconnect.
    Q_INVOKABLE void showMoreMenu(qreal x, qreal y);

    /// Tests only (Test.makeDockConnect, with --testScript): Connect on the Terminal route without the person's click;
    /// from then on every question Claude asks is answered no at once. Never the key route. Not on the QML's context.
    bool testConnectTerminal(bool chat);
    /// Tests only (Test.makeDockItem): an item of the dock's view by objectName: found, visible, its place, text and
    /// selectedText.
    QVariantMap testItem(const QString& objectName) const;
    /// Tests only (Test.makeDockCopy): copyLine without the real-input check; the text it put on the clipboard, or "".
    QString testCopyLine(const QString& which);
    /// Tests only (Test.makeDockMouse): a left press, a drag in steps and a release over that item, at fractions of its
    /// size, sent as Qt events (not spontaneous, so takeRealInput refuses whatever they lead to).
    bool testMouse(const QString& objectName, const QPointF& from, const QPointF& to);
    /// Tests only (Test.makeDockSend; security 21:48 11-07): send() without the real-input check, text only, and only
    /// into the chat a test's Connect turned on (testConnectTerminal(true)), never the person's own chat or the key
    /// route. The request's number, or 0 whichever check refused it (a kit can't learn which); the log counts
    /// characters, never the text.
    int testSend(const QString& text);

    /// The Make dock's view, once made (Panels): MakeDock watches its input and gives its engine the image provider.
    void watchWindow(QQuickWindow* window, QQmlEngine* engine);
    /// Plain text for the dock: control characters (but new lines) and bidi-override characters taken out.
    static QString clean(const QString& text);
    /// A file inside the Make folder that MakeDock serves (`what`: sheet, thumb or picture), or "" (image provider).
    QString servedFile(const QString& what, const QString& id) const;

signals:
    void stateChanged();
    void routeChanged();
    void keyChanged();
    void foundChanged();
    void setupTextChanged();
    void workingChanged();
    void channelChanged();
    void moreMenuChanged();
    /// a question was asked, answered or timed out
    void questionsChanged();
    /// a request came to its end by itself: finished (true) or couldn't finish (false); never after the person's Stop
    /// or Disconnect
    void requestEnded(bool finished);
    /// the Make badge was clicked: MakePanel.qml shows Now
    void showNow();
    void picturesAdded(const QVariantList& ids);
    void pictureRefused(const QString& reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// A real input in the dock's window in the last 500 ms, used up by this call. Logged when refused.
    bool takeRealInput(const char* call);
    /// Connect's work, after the gate (or a test's Terminal connect)
    bool connectNow();
    /// send()'s checks before the real-input gate: the route, the chat's state, the text, not already working
    bool canSend(const QString& text) const;
    /// send()'s work after its checks and the gate: the pictures, the link's send, the ask's row
    int sendNow(const QString& text, const QStringList& pictureIds);
    /// A test's chat ends (Disconnect, a refused connect, the person's route): the chat line is the person's own again
    void endTestChat();
    void linkStateChanged();
    void readFound();
    void addRow(MakeDockRows::Row row);
    void addMade(const MakeDockRows::Row& row);
    void requestDone(int request, double cost);
    void setWorking(bool working);
    void cleanPictures(const QStringList& files);
    QString picturesFolder() const;
    /// A model's sheet of views kept in the Make folder's sheets/ from MakeLink's bytes; a path as it is; "" when not kept
    QString keepSheet(const QString& model, const QVariant& views) const;
    QString lastAllowedSetupHash() const;
    /// The hash one Allow covers: the Allow's words, the route, setupText and the start line (per-Connect parts masked)
    /// and the signed-in account (security R2, R4).
    QString allowHash() const;

    MakeDockRows _timeline;
    MakeDockRows _made;

    bool _starting { false };       // the key route: Claude Code started, not ready yet
    bool _testRefusesAsks { false }; // connected by a test: every question answered no
    bool _testChat { false };        // connected by a test with the chat line, for that run only
    QString _reason;
    QString _route;
    bool _hasKey { false };
    bool _claudeFound { false };
    bool _blenderFound { false };
    bool _blenderSealed { false };
    QString _blenderWhy;
    bool _working { false };
    double _costSoFar { -1.0 };
    int _doneRequest { 0 };         // the newest request with a "done" row

    QPointer<QQuickWindow> _window;
    QElapsedTimer _sinceInput;      // since the last real input in the dock's window
    bool _inputUnused { false };
    QElapsedTimer _sinceDrop;
    QElapsedTimer _sinceFoundRead;  // the last re-read of what's on this computer on show or activation
    QList<QUrl> _droppedUrls;
    QPointer<QFileDialog> _chooser;
    // the Terminal route: a request is over after 45 s with no activity, or 5 s after Claude's reply with none (Make
    // can't see Claude's turn end there; an activity after the reply waits 45 s again)
    QTimer _terminalQuiet;
    bool _moreMenuOpen { false };
    // the channel: a message sent and not yet taken up by Claude (from the link's "sent to Claude" to the request's
    // next activity or reply); the first message of a Connect not taken up in 30 s (not got: org policy, no flag)
    bool _sentWaiting { false };
    bool _notGotIt { false };
    bool _channelProven { false };  // Claude has taken up a message this Connect
    int _sentRequest { 0 };
    QTimer _notGotItTimer;
    void channelTakenUp(int request);
    void resetChannel();
    QHash<QString, QString> _sheets;  // model id -> its sheet file in the Make folder
    MakeBadge* _badge { nullptr };     // the Make badge, a child of this
    bool _stopRequested { false };     // the person's Stop or Disconnect since the request began: no "couldn't finish"
    QHash<int, qint64> _askTimes;     // question id -> when it was asked (MakeLink refuses after 100 s)
    QTimer _askTimeouts;
};

#endif // hifi_MakeDock_h
