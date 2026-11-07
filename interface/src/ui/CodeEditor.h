//
//  CodeEditor.h
//  interface/src/ui
//
//  The Code panel's document (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md): the one script the panel shows, which
//  entity and which of its scripts it is, where it lives, whether the person may save it, its running status and
//  error, and saving it. The panel (qml/darkride/CodePanel.qml, Qt 6 only) sees this object in its own context only;
//  the page inside it (html/code/) sees nothing of it.
//
//  Opening takes an entity id and which script, never an address (security, team/security/reviews/
//  make-v1-editor-page.md, item 1): the address is the entity's own, and a script file is fetched under the rules any
//  script from that world gets (scripting's ScriptOpener). A script written into the entity's field needs no fetch.
//  A world's server scripts open only for people who can edit that entity (Caity, 22:25 11-06).
//
//  Saving is always the person's own click or Cmd+S, through ordinary edits with their own rights (item 5): a
//  written-in script back into its field as a Build mode edit (so Build's Undo takes it back), a file in the world's
//  files back to its path with the asset server's own upload rights, then reloaded for everyone as the Inspector's
//  Reload does. A script on the web is read only.
//
//  It lives on the main thread and emits only there (the panel's QML connects to it, and on Qt 6 a QML connection to an
//  object of another thread is fatal): everything from other threads reaches it queued, and MakeDock calls it on the
//  main thread only.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_CodeEditor_h
#define hifi_CodeEditor_h

#include <memory>

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVariant>

#include <DependencyManager.h>

class CallbackTarget;
class CodeNearby;
class QMenu;

class CodeEditor : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY documentChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY documentChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY documentChanged)
    Q_PROPERTY(QString where READ where NOTIFY documentChanged)
    Q_PROPERTY(QString entityName READ entityName NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language NOTIFY documentChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY documentChanged)
    Q_PROPERTY(bool hasLink READ hasLink NOTIFY documentChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY rightsChanged)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY rightsChanged)
    Q_PROPERTY(bool saving READ isSaving NOTIFY savingChanged)
    Q_PROPERTY(QString savedText READ savedText NOTIFY savedChanged)
    Q_PROPERTY(QString note READ note NOTIFY noteChanged)
    Q_PROPERTY(bool noteIsProblem READ noteIsProblem NOTIFY noteChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusState READ statusState NOTIFY statusChanged)
    Q_PROPERTY(int errorLine READ errorLine NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)
    Q_PROPERTY(bool claudeWriting READ isClaudeWriting NOTIFY claudeChanged)
    Q_PROPERTY(bool showingChange READ isShowingChange NOTIFY claudeChanged)
    Q_PROPERTY(QObject* nearby READ nearby CONSTANT)

public:
    CodeEditor();
    ~CodeEditor() override;

    // whether this build has the Code panel: its page needs Qt 6's web engine
    static bool isAvailable();

    // from the Inspector, the Hierarchy, Code near you and Make's cards: opens the panel on that entity's script
    // ("script" or "serverScripts"); answers false for an entity that isn't there or a script it doesn't have
    Q_INVOKABLE bool open(const QUuid& entityId, const QString& which);

    bool hasDocument() const { return !_entity.isNull(); }
    bool isLoading() const { return _loading; }
    QString fileName() const { return _fileName; }
    QString kind() const;
    QString where() const;
    QString entityName() const { return _entityName; }
    QString language() const { return QStringLiteral("javascript"); }
    QString problem() const { return _problem; }
    bool hasLink() const { return _place == Place::WorldFiles || _place == Place::Web; }
    bool canSave() const { return _canSave; }
    QString readOnlyReason() const { return _readOnlyReason; }
    bool isSaving() const { return _saving; }
    QString savedText() const { return _savedText; }
    QString note() const { return _note; }
    bool noteIsProblem() const { return _noteIsProblem; }
    QString status() const { return _status; }
    QString statusState() const { return _statusState; }
    int errorLine() const { return _errorLine; }
    QString errorText() const { return _errorText; }

    QUuid entity() const { return _entity; }
    QString which() const { return _which; }

    // the text as opened or last saved: what the page shows when it (re)opens
    Q_INVOKABLE QString text() const { return _text; }
    // a change still shown side by side, for a page made after it was shown
    Q_INVOKABLE QStringList change() const { return _showingChange ? QStringList{ _changeOld, _changeNew } : QStringList(); }
    // the page's text differs from it (the page tells the panel; Undo of a save doesn't replace typing)
    Q_INVOKABLE void setDirty(bool dirty) { _dirty = dirty; }
    // the person's unsaved change, kept while the panel is closed and shown again when it opens; gone on a save or
    // when another script opens
    Q_INVOKABLE void setDraft(const QString& text, bool has) { _draft = has ? text : QString(); _hasDraft = has; }
    Q_INVOKABLE bool hasDraft() const { return _hasDraft; }
    Q_INVOKABLE QString draft() const { return _draft; }
    // the person's Save or Cmd+S, with the page's text
    Q_INVOKABLE void save(const QString& text);
    // Script: everyone's client loads it again; Server Script: the script server does (the Inspector's Reload)
    Q_INVOKABLE void reloadForEveryone();
    // Build mode on, the entity selected in the Inspector
    Q_INVOKABLE void showInInspector();
    // the script's address on the clipboard
    Q_INVOKABLE void copyLink();
    // whether this script is the open one (Code near you's highlighted row)
    Q_INVOKABLE bool isOpenScript(const QUuid& entity, const QString& which) const {
        return hasDocument() && entity == _entity && which == _which;
    }
    // Esc in the editor: the keyboard back to the 3D view
    Q_INVOKABLE void giveBackKeyboard();
    // the panel's two menus, the app's own (as Create's and the Entities list's), at (x, y) on the screen: ••• (Show in
    // Inspector, Copy Link), and the name's: the code near you, nearest first, the open one ticked, then Show All Code
    // Near You (the list in the panel; `listShown`: it's showing)
    Q_INVOKABLE void showMoreMenu(qreal x, qreal y);
    Q_INVOKABLE void showNearbyMenu(qreal x, qreal y, bool listShown);
    // a test's picture of one of those menus ("more" or "nearby"), drawn without showing it
    bool saveMenuPicture(const QString& which, const QString& path);
    // the panel open or closed: the status is asked for only while it's open
    Q_INVOKABLE void setPanelOpen(bool open);
    // Code near you (ui/CodeNearby.h), and whether it's unfolded: it looks at the world only while the panel is open
    // and it's unfolded
    QObject* nearby() const;
    Q_INVOKABLE void setNearbyShown(bool shown);
    // the page made and ready for the panel's calls, or gone
    Q_INVOKABLE void setPageReady(bool ready) { _pageReady = ready; }
    bool isPageReady() const { return _pageReady; }

    // Make's side (MakeDock): Claude writing an entity's script through scripting's building tools. When the panel is
    // open on that script, the text shows as it arrives, read only and marked "Claude is writing…"; when Claude
    // finishes, old and new side by side (Keep and Undo stay on Make's card). The change itself lands through the
    // building tools, never through the panel's save. A script with the person's unsaved typing isn't streamed over:
    // the panel says Claude changed it.
    bool isShowing(const QUuid& entityId, const QString& which) const { return _entity == entityId && _which == which; }
    void claudeBegin(const QUuid& entityId, const QString& which);
    void claudeText(const QUuid& entityId, const QString& which, const QString& chunk);
    void claudeEnd(const QUuid& entityId, const QString& which, const QString& oldText, const QString& newText);
    // Make's card "Open in Code": the script, with what Claude changed side by side
    bool openChange(const QUuid& entityId, const QString& which, const QString& oldText, const QString& newText);
    bool isClaudeWriting() const { return _claudeWriting; }
    bool isShowingChange() const { return _showingChange; }
    // the panel's "Back to the script" after a change was shown
    Q_INVOKABLE void closeChange();

    // for Test (scripted runs only, --testScript): JavaScript run in the page through the panel, its answer by number,
    // and the panel's Save pressed
    int testRun(const QString& source);
    QVariant testAnswerFor(int run) const { return _testAnswers.contains(run) ? QVariant(_testAnswers.value(run)) : QVariant(); }
    Q_INVOKABLE void testAnswer(int run, const QString& json) { _testAnswers.insert(run, json); }
    void testSave() { emit testRequest("save", QString(), 0); }
    // the panel open on Code near you alone, unfolded, no script (performance's ride row)
    void testShowNearby();

signals:
    void documentChanged();   // a new document, or the same one's text replaced (Undo of a save): the page shows text()
    void rightsChanged();
    void savingChanged();
    void savedChanged();
    void noteChanged();
    void statusChanged();
    void testRequest(const QString& what, const QString& source, int run);   // Test only: the panel runs it
    void claudeChanged();
    // to the page, through the panel
    void streamBegan();
    void streamed(const QString& chunk);
    void streamEnded(const QString& text);
    void changeShown(const QString& oldText, const QString& newText);
    void changeClosed();
    void nearbyUnfoldRequested();
    void nearbyFoldRequested();

private:
    QMenu* makeMoreMenu();
    QMenu* makeNearbyMenu(bool listShown);

    enum class Place { None, WrittenIn, WorldFiles, Web };

    void clear();
    bool readEntity(QString& value, QString& name, bool& locked) const;
    void updateRights();
    void setNote(const QString& text, bool problem);
    void setSaving(bool saving);
    void finishSave(const QString& text);
    void saveWrittenIn(const QString& text);
    void saveToWorldFiles(const QString& text);
    void fetch();
    void askStatus();
    void answered(int32_t callback, const QVariantList& arguments);
    void refreshAfterUndo();

    QUuid _entity;
    QString _which;            // "script" or "serverScripts"
    QString _value;            // the entity's field as it was when opened or saved: the address, or the script itself
    QUrl _url;
    Place _place { Place::None };
    QString _fileName;
    QString _entityName;
    QString _text;
    QString _draft;
    bool _hasDraft { false };
    QString _problem;
    bool _loading { false };
    bool _dirty { false };
    bool _canSave { false };
    QString _readOnlyReason;
    bool _saving { false };
    QString _savedText;
    QString _note;
    bool _noteIsProblem { false };
    int _generation { 0 };     // a new document: answers about the one before are dropped

    bool _panelOpen { false };
    bool _nearbyShown { false };
    CodeNearby* _nearby { nullptr };
    bool _pageReady { false };
    bool _claudeWriting { false };
    bool _showingChange { false };
    QString _changeOld;        // a change shown, for a page made after it (the panel opened later)
    QString _changeNew;
    int _nextTestRun { 1 };
    QHash<int, QString> _testAnswers;
    QTimer _poll;
    std::shared_ptr<CallbackTarget> _target;
    int32_t _nextCallback { 1 };
    int32_t _question { 0 };
    QString _status;
    QString _statusState;      // "running", "error", "" (starting, not running here)
    int _errorLine { 0 };
    QString _errorText;
};

#endif // hifi_CodeEditor_h
