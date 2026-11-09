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
//  New Script (Caity's walkthrough 11-08, 1.1.4; mary's words): "New" beside an empty Script or Server Script field in
//  the Inspector, "New Script" in the panel's header for the one selected thing (an entity script). The panel opens a
//  starter script, named for the thing ("Cube.js", "not saved yet"), and nothing is made until the first Save: then the
//  text is kept where Make keeps its work (newScriptPlace: the person's My Files by default, the world's files where
//  they chose them and the world allows them; Caity 11-08) at a free scripts/ name, and the field points at it, as ONE
//  Build mode edit that Build's Undo takes back (the file stays). Nothing is ever written into the field instead (a
//  written-in script over about 1 KB never reaches visitors). Signed out, My Files can't keep it; a world opened from a
//  file has no server for a server script (mary's words). A script in the person's own My Files opens savable: a Save
//  sends it to the same path, and its link stays.
//
//  From My Files (Caity 11-08, 1.1.4: "if you open a code file there it should open in our code editor"): a script
//  double-clicked in the person's own My Files opens with no entity (openMyFile). Its path and link come from My
//  Files' own listing, in C++ only, never from a page or a script; the link must be one at our files host. It's fetched
//  as the app fetches any of our files (a private one asked once more with the person's login, at our files host only),
//  under the same cap, time limit and network rule as an entity's script, and a Save sends it back to the same path.
//  Nothing in the world is its, so it has no status, no Reload and no Show in Inspector, and arriving in another world
//  leaves it open (its unsaved typing is kept apart from every world's).
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

#include <functional>
#include <memory>

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVariant>

#include <DependencyManager.h>

class CallbackTarget;
class CodeNearby;
class NetworkShader;
class QMenu;

class CodeEditor : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY documentChanged)
    // the header's: they change with every document, and on a new script's first Save without the text changing
    Q_PROPERTY(QString fileName READ fileName NOTIFY headerChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY documentChanged)
    Q_PROPERTY(QString where READ where NOTIFY headerChanged)
    // the header's second line: where the script is kept, as a place (mary 11-08): "Inside Greeter", "In My Files",
    // "On the web"; nothing for the world's files
    Q_PROPERTY(QString place READ place NOTIFY headerChanged)
    Q_PROPERTY(QString entityName READ entityName NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language NOTIFY documentChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY documentChanged)
    Q_PROPERTY(bool hasLink READ hasLink NOTIFY headerChanged)
    // a new script not saved yet: nothing of it is in the world (the header says "not saved yet"; Save is on as it is)
    Q_PROPERTY(bool isNew READ isNew NOTIFY headerChanged)
    // opened from My Files, with no entity: no Reload, no status, no Show in Inspector
    Q_PROPERTY(bool fromMyFiles READ isFromMyFiles NOTIFY documentChanged)
    // the header's New Script: why it's off for Build mode's selection now ("" when it's on), and its tip when on
    Q_PROPERTY(QString newScriptOff READ newScriptOff NOTIFY newScriptChanged)
    Q_PROPERTY(QString newScriptTip READ newScriptTip NOTIFY newScriptChanged)
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
    // the list on the left (Caity 11-07, mockups/code-panel-list): the open file's key (its row), its path for the
    // header's first line, what uses it and how far for its second, and how many things use it (a save changes them all)
    Q_PROPERTY(QString openKey READ openKey NOTIFY headerChanged)
    Q_PROPERTY(QString path READ path NOTIFY headerChanged)
    Q_PROPERTY(bool isShader READ isShader NOTIFY documentChanged)
    Q_PROPERTY(QString usedBy READ usedBy NOTIFY usedByChanged)
    Q_PROPERTY(int sharedBy READ sharedBy NOTIFY usedByChanged)
    // the files with an unsaved change kept (the person moved to another file), in this world: their rows say Edited
    Q_PROPERTY(QStringList draftKeys READ draftKeys NOTIFY draftsChanged)
    // changes with every document opened or cleared: an answer from the page about the text is kept as a draft only
    // while it's still the same document (the file key alone is the same for one atp: path in two worlds)
    Q_PROPERTY(QString openStamp READ openStamp NOTIFY headerChanged)

public:
    CodeEditor();
    ~CodeEditor() override;

    // whether this build has the Code panel: its page needs Qt 6's web engine
    static bool isAvailable();

    // from the Inspector, the Hierarchy, Code near you and Make's cards: opens the panel on that entity's script
    // ("script" or "serverScripts") or shader ("fragmentShader" or "vertexShader", read only: its address read from the
    // entity itself, never given); answers false for an entity that isn't there or a script it doesn't have
    // (by value: callers may pass this document's own entity and field, which opening clears first)
    Q_INVOKABLE bool open(QUuid entityId, QString which);
    // the Inspector's New beside an empty Script ("script") or Server Script ("serverScripts") field: the panel opens
    // a starter script for that entity, made in the world's files at its first Save; false when it can't
    // (whyNoNewScript says why). open() on an empty field comes here too (the Hierarchy's New Script)
    Q_INVOKABLE bool openNew(QUuid entityId, QString which);
    // the header's New Script: a new entity script for the one thing selected in Build mode
    Q_INVOKABLE void newScript();
    // My Files' double-click on a script: the person's own file, by its path in their listing and its link there (from
    // My Files itself, C++ only), opened with no entity; false when it can't open here (not a link at our files host,
    // or not a script Code shows). size is the listing's, so a file over the cap isn't fetched at all
    bool openMyFile(const QString& path, const QUrl& link, const QString& name, qint64 size);
    // the My Files names Code opens: scripts as text (.js, .mjs, .qml); a .wasm stays as it is
    static bool opensInCode(const QString& name);
    // why a new script can't be made for that entity's field now ("" when it can): the words the Inspector's New and
    // the header's New Script show while they're off (mary 11-08)
    static QString whyNoNewScript(const QUuid& entityId, const QString& which);
    // a world opened from a file (serverless): no files of its own, no server for a server script
    static bool isFileWorld();
    // where a new script's file goes now: Make's place (MakeLink::filesGoTo), "my_files" or "world"; the one place
    // New Script reads it, so it follows Make's choice
    static QString newScriptPlace();
    QString newScriptOff() const { return _newScriptOff; }
    QString newScriptTip() const { return _newScriptTip; }

    bool hasDocument() const { return !_entity.isNull() || _fromMyFiles; }
    bool isFromMyFiles() const { return _fromMyFiles; }
    bool isLoading() const { return _loading; }
    QString fileName() const { return _fileName; }
    QString kind() const;
    QString where() const;
    QString place() const;
    QString entityName() const { return _entityName; }
    QString language() const { return isShader() ? QStringLiteral("glsl") : QStringLiteral("javascript"); }
    bool isShader() const;
    QString openKey() const;
    QString path() const;
    QString usedBy() const { return _usedBy; }
    int sharedBy() const { return _sharedBy; }
    QStringList draftKeys() const;
    QString openStamp() const;
    int allDraftCount() const { return _drafts.size(); }   // every world's (for Test)
    // what a quit would lose (ui/QuitCheck.cpp): the names of the files with typing that isn't saved, in every world,
    // the open one first (a new script by the name its first Save gives it)
    QStringList unsavedNames() const;
    // the open file's name there: the header's, but "Cube's script" for one written into Cube's field (mary 11-08)
    QString unsavedName() const;
    QString problem() const { return _problem; }
    bool hasLink() const {
        return !_isNew && (_place == Place::WorldFiles || _place == Place::MyFiles || _place == Place::Web);
    }
    bool isNew() const { return _isNew; }
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
    // the person's unsaved change to the open file, kept while the panel is closed or another file is open, and shown
    // again when it opens; gone on a save (one per file and world, for this run of the app: the same atp: path or
    // entity id in another world is another file, security 11-07)
    Q_INVOKABLE void setDraft(const QString& text, bool has);
    Q_INVOKABLE bool hasDraft() const;
    Q_INVOKABLE QString draft() const;
    // the panel's answer to keepBeforeLeaving (its text kept first): the document goes
    Q_INVOKABLE void leaveDocument(const QString& stamp);
    // the person's Save or Cmd+S, with the page's text
    Q_INVOKABLE void save(const QString& text);
    // Script: everyone's client loads it again; Server Script: the script server does (the Inspector's Reload)
    Q_INVOKABLE void reloadForEveryone();
    // Build mode on, the entity selected in the Inspector
    Q_INVOKABLE void showInInspector();
    // the script's address on the clipboard
    Q_INVOKABLE void copyLink();
    // Esc in the editor: the keyboard back to the 3D view
    Q_INVOKABLE void giveBackKeyboard();
    // the panel's ••• menu, the app's own (as Create's and the Entities list's), at (x, y) on the screen: Show in
    // Inspector, Copy Link
    Q_INVOKABLE void showMoreMenu(qreal x, qreal y);
    // a test's picture of that menu ("more"), drawn without showing it
    bool saveMenuPicture(const QString& which, const QString& path);
    // the list's Refresh: a look at the world now
    Q_INVOKABLE void refreshNearby();
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
    // the header's New Script pressed, through the panel's own press (which keeps unsaved typing first)
    void testNewScript() { emit testRequest("newScript", QString(), 0); }
    // the panel open on Code near you alone, unfolded, no script (performance's ride row)
    void testShowNearby();
    // a row of the list clicked, through the panel's own click (which keeps unsaved typing first)
    void testOpenRow(const QUuid& entityId, const QString& which) { emit testOpenRowRequested(entityId, which); }
    // the list unfolded or folded, as its sidebar button does
    void testShowList(bool shown) { emit testRequest("list", shown ? "open" : "fold", 0); }
    // stands in for another world ("" = the real one), as arriving there does: the open document cleared, drafts kept
    // per world. An offline kit can't make one file key in two worlds (a file:// world gives new entity ids)
    void testSetWorld(const QString& name);

signals:
    void documentChanged();   // a new document, or the same one's text replaced (Undo of a save): the page shows text()
    void headerChanged();     // with documentChanged, and alone when a new script's first Save names its file
    void newScriptChanged();
    void rightsChanged();
    void savingChanged();
    void savedChanged();
    void noteChanged();
    void statusChanged();
    void testRequest(const QString& what, const QString& source, int run);   // Test only: the panel runs it
    void testOpenRowRequested(const QUuid& entityId, const QString& which);   // Test only
    void claudeChanged();
    // to the page, through the panel
    void streamBegan();
    void streamed(const QString& chunk);
    void streamEnded(const QString& text);
    void changeShown(const QString& oldText, const QString& newText);
    void changeClosed();
    void usedByChanged();
    void draftsChanged();
    // arriving in another world with unsaved typing: the panel keeps the page's text as a draft, then leaveDocument
    void keepBeforeLeaving();

private:
    QMenu* makeMoreMenu();
    void updateUsedBy();
    QString worldKey() const;
    // the open document's draft key: the world it was opened in, then the file (a world change doesn't move it)
    QString docDraftKey() const { return _docWorld + QLatin1Char('\n') + openKey(); }
    void enterWorld();   // another world: its entities aren't this document's, so the document goes
    void finishLeaving();
    void fetchShader();
    void updateNewScript();   // the header's New Script, for the selection now
    void saveNew(const QString& text);
    void pickFreePath(const QString& hash, const QString& text, int generation);
    void saveNewToMyFiles(const QString& text, int generation);
    // the field pointed at the new file (address: atp:/scripts/… or a My Files link), with what made it
    void pointAtNewFile(const QString& address, const QString& path, const QString& madeKey, const QString& place,
                        const QString& text);
    QString savedAs(const QString& path, const QString& place) const;   // "Saved in My Files as scripts/Cube.js"
    // My Files: the person's listing, fetched now (done(false) signed out or when it can't be had)
    void withMyFiles(const std::function<void(bool listed)>& done);
    void checkMyFilesOwner();   // whether the open My Files link is one of the person's own files (then it saves)
    void saveToMyFiles(const QString& text);

    enum class Place { None, WrittenIn, WorldFiles, MyFiles, Web };

    void clear();
    bool readEntity(QString& value, QString& name, bool& locked) const;
    void updateRights();
    void setNote(const QString& text, bool problem);
    void setSaving(bool saving);
    void finishSave(const QString& text);
    void saveWrittenIn(const QString& text);
    void saveToWorldFiles(const QString& text);
    void fetch();
    void fetchMyFile();   // a My Files script's text, by its link (openMyFile)
    void askStatus();
    void answered(int32_t callback, const QVariantList& arguments);
    void refreshAfterUndo();

    QUuid _entity;
    bool _fromMyFiles { false };   // opened from My Files: no entity (_entity null), _myFilesPath and _url its own
    QString _which;            // "script" or "serverScripts"
    QString _value;            // the entity's field as it was when opened or saved: the address, or the script itself
    bool _isNew { false };     // a new script, not saved yet: _value is the empty field, _fileName the name it will get
    QString _newBase;          // its name without ".js" ("Cube", "Cube server")
    QString _madePath;         // its file once saved, and the text's hash there (world files) or its link (My Files):
    QString _madeHash;         //   an Undo then Save again reuses the file it made if nothing has changed it since
    QString _madeAddress;      //   (no "Cube-2.js" for the same script); the field's value it set, and where it went
    QString _madePlace;
    class UserFiles* _userFiles { nullptr };   // the person's My Files listing, made when first needed
    QString _myFilesPath;      // the open My Files script's path in the person's own My Files ("" not theirs, or unknown)
    bool _myFilesChecked { false };   // the listing has answered for the open My Files script
    bool _pushingEdit { false };   // the first Save's Build mode edit going onto the stack
    QString _newScriptOff;
    QString _newScriptTip;
    QUrl _url;
    Place _place { Place::None };
    QString _fileName;
    QString _entityName;
    QString _text;
    QHash<QString, QString> _drafts;   // by world, then file (docDraftKey() for the open document)
    QHash<QString, QString> _draftNames;   // the header's name of each file when its draft was kept (for the quit)
    QString _worldAddress;     // the world's address (the domain's URL), from DomainHandler
    QUuid _worldID;            // the domain's id once connected; another domain at the same address is another world
    QString _testWorld;
    QString _docWorld;         // worldKey() when the open document was opened
    bool _leaving { false };   // in another world, the page's unsaved text on its way into the old world's drafts
    QString _usedBy;
    int _sharedBy { 0 };
    QSharedPointer<NetworkShader> _shader;   // a shader on its way from the shader cache
    QMetaObject::Connection _shaderConnection;
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
