//
//  CodeEditor.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "CodeEditor.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QUndoStack>

#include <AssetClient.h>
#include <AssetUpload.h>
#include <EntityItemProperties.h>
#include <EntityScriptingInterface.h>
#include <MappingRequest.h>
#include <NodeList.h>
#include <ScriptCallback.h>

#include "Application.h"
#include "InterfaceLogging.h"
#include "Panels.h"
#include "editor/Editor.h"
#include "editor/EditorUndo.h"

namespace {

const QString PANEL = QStringLiteral("Code");
const QString SCRIPT = QStringLiteral("script");
const QString SERVER_SCRIPTS = QStringLiteral("serverScripts");
const int POLL_MS = 2000;                  // the script's status, as often as the Inspector asks
const qint64 MAX_SAVE_BYTES = 1024 * 1024; // as ScriptOpener's cap on what it opens
const int INLINE_SCRIPT_LIMIT_BYTES = 1024;   // a written-in Script past this doesn't reach visitors (ScriptEditor.cpp)
const int MAX_ERROR_SHOWN = 400;

// queryPropertyMetadata answers through a ScriptCallback, as it answers a script's function: this stands in for the
// script engine and hands the answer to the editor on the main thread (as ScriptEditor.cpp's)
class AnswerTarget : public CallbackTarget {
public:
    explicit AnswerTarget(std::function<void(int32_t, const QVariantList&)> onAnswer) : _onAnswer(std::move(onAnswer)) {}
    bool invoke(int32_t callback, const QVariantList& arguments, QVariant&, QString&) override {
        post(callback, arguments);
        return true;
    }
    void post(int32_t callback, const QVariantList& arguments) override {
        auto onAnswer = _onAnswer;
        QMetaObject::invokeMethod(qApp, [onAnswer, callback, arguments] { onAnswer(callback, arguments); },
            Qt::QueuedConnection);
    }
    void request(int32_t callback, const QVariantList& arguments, std::function<void(const QVariant&)>) override {
        post(callback, arguments);
    }

private:
    std::function<void(int32_t, const QVariantList&)> _onAnswer;
};

bool isAddress(const QString& value) {
    const QString trimmed = value.trimmed();
    return !trimmed.isEmpty() && !trimmed.contains('\n') && !QUrl(trimmed).scheme().isEmpty();
}

// the engine's error: its first line, the error's name once ("SyntaxError: SyntaxError: …"), and the line of the
// script it points at, found after the script's address or as "line N"
void readError(const QString& error, const QString& link, QString& shown, int& line) {
    QString first = error.section('\n', 0, 0).trimmed();
    const int colon = first.indexOf(": ");
    if (colon > 0 && first.mid(colon + 2).startsWith(first.left(colon + 2))) {
        first = first.mid(colon + 2);
    }
    shown = first.left(MAX_ERROR_SHOWN);
    line = 0;
    // a file's script: its own address and line (a line in a file it includes names that file, and isn't this one's);
    // a written-in one: the "(<name>:<line>:<col>)" the engine ends its error with, when that name isn't a file's
    // address (scripting, 11-06: the wasm engine adds it)
    QRegularExpressionMatch match;
    if (!link.isEmpty()) {
        match = QRegularExpression(QRegularExpression::escape(link) + ":(\\d+)(:\\d+)?").match(error);
        if (match.hasMatch()) {
            line = match.captured(1).toInt();
        }
        return;
    }
    match = QRegularExpression("\\(([^()\\s]*):(\\d+):(\\d+)\\)\\s*$").match(error.section('\n', 0, 0));
    if (match.hasMatch()) {
        const QString scheme = QUrl(match.captured(1)).scheme().toLower();
        if (scheme != "http" && scheme != "https" && scheme != "atp" && scheme != "file") {
            line = match.captured(2).toInt();
        }
    }
}

QString clockText() {
    return "Saved " + QDateTime::currentDateTime().toString("HH:mm");
}

}

CodeEditor::CodeEditor() {
    _poll.setInterval(POLL_MS);
    connect(&_poll, &QTimer::timeout, this, &CodeEditor::askStatus);
    _target = std::make_shared<AnswerTarget>([this](int32_t callback, const QVariantList& arguments) {
        answered(callback, arguments);
    });
    // Build's Undo of a save (or of any edit of the field) shows in the panel, unless the person has typed since
    if (auto editor = DependencyManager::get<Editor>()) {
        connect(editor->undoStack().stack(), &QUndoStack::indexChanged, this, &CodeEditor::refreshAfterUndo);
    }
    // the world's rights change with the world, and with an instance's edit mode
    auto nodeList = DependencyManager::get<NodeList>();
    connect(nodeList.data(), &LimitedNodeList::canRezChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezTmpChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canWriteAssetsChanged, this, &CodeEditor::updateRights);
}

CodeEditor::~CodeEditor() {
}

bool CodeEditor::isAvailable() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return true;
#else
    return false;
#endif
}

QString CodeEditor::kind() const {
    if (!hasDocument()) {
        return QString();
    }
    return _which == SERVER_SCRIPTS ? "Server script" : "Entity script";
}

QString CodeEditor::where() const {
    switch (_place) {
        case Place::WrittenIn:
            return "Written in";
        case Place::WorldFiles:
            return "World files";
        case Place::Web:
            return "On the web, read only";
        default:
            return QString();
    }
}

bool CodeEditor::readEntity(QString& value, QString& name, bool& locked) const {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    EntityPropertyFlags flags;
    flags += PROP_NAME;
    flags += PROP_LOCKED;
    flags += _which == SERVER_SCRIPTS ? PROP_SERVER_SCRIPTS : PROP_SCRIPT;
    EntityItemProperties properties = entities->getEntityProperties(_entity, flags);
    if (properties.getType() == EntityTypes::Unknown) {
        return false;
    }
    value = _which == SERVER_SCRIPTS ? properties.getServerScripts() : properties.getScript();
    name = properties.getName();
    locked = properties.getLocked();
    return true;
}

bool CodeEditor::open(const QUuid& entityId, const QString& which) {
    if (!isAvailable() || entityId.isNull() || (which != SCRIPT && which != SERVER_SCRIPTS)) {
        return false;
    }
    clear();
    _entity = entityId;
    _which = which;
    QString value;
    bool locked = false;
    if (!readEntity(value, _entityName, locked)) {
        clear();
        emit documentChanged();
        return false;
    }
    ++_generation;
    _value = value;
    updateRights();

    const QString trimmed = value.trimmed();
    if (which == SERVER_SCRIPTS && !_canSave) {
        // the world's editors only (Caity, 22:25 11-06): nothing of it is read, not even its address
        _value.clear();
        _fileName = _entityName.isEmpty() ? "Server script" : _entityName;
        _problem = "Only people who can change this thing can read its server script.";
    } else if (!isAddress(trimmed)) {
        // written into the field (or empty: a new script typed here is saved into the field)
        _place = Place::WrittenIn;
        _fileName = _entityName.isEmpty() ? "Script" : _entityName;
        _text = value;
    } else {
        _url = QUrl(trimmed);
        _fileName = _url.fileName().isEmpty() ? _url.toDisplayString() : _url.fileName();
        const QString scheme = _url.scheme().toLower();
        if (scheme == "atp") {
            _place = Place::WorldFiles;
        } else if (scheme == "http" || scheme == "https") {
            _place = Place::Web;
        }
        if (_place == Place::None) {
            _problem = "This script's address can't be opened here.";
        } else {
            fetch();
        }
    }
    updateRights();
    qCDebug(interfaceapp) << "Code: opened" << _entity << _which << where() << (_problem.isEmpty() ? "" : "problem");
    emit documentChanged();
    emit savedChanged();
    emit noteChanged();
    emit statusChanged();
    emit claudeChanged();

    DependencyManager::get<Panels>()->open(PANEL);
    askStatus();
    return true;
}

// a script file's text, fetched by scripting's ScriptOpener under the rules any script of that world gets (public
// addresses only for a world from the network, a size cap, a time limit; file: refused)
void CodeEditor::fetch() {
    // ScriptOpener (scripting, Make v1 piece 7) is not in this branch yet: until it is, only written-in scripts open
    _problem = "Opening a script from its file comes with the next step of the Code panel.";
}

void CodeEditor::clear() {
    ++_generation;
    _entity = QUuid();
    _which.clear();
    _value.clear();
    _url = QUrl();
    _place = Place::None;
    _fileName.clear();
    _entityName.clear();
    _text.clear();
    _draft.clear();
    _hasDraft = false;
    _problem.clear();
    _loading = false;
    _dirty = false;
    _canSave = false;
    _readOnlyReason.clear();
    _saving = false;
    _savedText.clear();
    _note.clear();
    _noteIsProblem = false;
    _question = 0;
    _status.clear();
    _statusState.clear();
    _errorLine = 0;
    _errorText.clear();
    _claudeWriting = false;
    _showingChange = false;
    _changeOld.clear();
    _changeNew.clear();
}

void CodeEditor::updateRights() {
    bool canSave = false;
    QString reason;
    if (hasDocument()) {
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        auto nodeList = DependencyManager::get<NodeList>();
        QString value;
        QString name;
        bool locked = false;
        const bool there = readEntity(value, name, locked);
        const bool mayEdit = there && !locked && (entities->canRez() || entities->canRezTmp());
        if (!there) {
            reason = "This thing isn't in the world any more.";
        } else if (_place == Place::Web) {
            reason = "On the web: read only here.";
        } else if (!(entities->canRez() || entities->canRezTmp())) {
            reason = "You can't change things in this world.";
        } else if (locked) {
            reason = "Locked: unlock it in the Inspector to change its script.";
        } else if (_place == Place::WorldFiles && !nodeList->getThisNodeCanWriteAssets()) {
            reason = "You can't change this world's files.";
        }
        // a server script's rights decide whether it opens at all (open()), so they're read before the place is known
        canSave = mayEdit && (_place != Place::Web) &&
            (_place != Place::WorldFiles || nodeList->getThisNodeCanWriteAssets());
    }
    if (canSave != _canSave || reason != _readOnlyReason) {
        _canSave = canSave;
        _readOnlyReason = reason;
        emit rightsChanged();
    }
}

void CodeEditor::setNote(const QString& text, bool problem) {
    _note = text;
    _noteIsProblem = problem;
    emit noteChanged();
}

void CodeEditor::setSaving(bool saving) {
    if (saving != _saving) {
        _saving = saving;
        emit savingChanged();
    }
}

void CodeEditor::save(const QString& text) {
    updateRights();
    if (!hasDocument() || !_problem.isEmpty() || _saving || _claudeWriting) {
        return;
    }
    if (!_canSave) {
        setNote(_readOnlyReason.isEmpty() ? "You can't change this script here." : _readOnlyReason, true);
        return;
    }
    if (text.toUtf8().size() > MAX_SAVE_BYTES) {
        setNote("This script is too big to save (1 MB at most).", true);
        return;
    }
    if (_place == Place::WrittenIn) {
        saveWrittenIn(text);
    } else if (_place == Place::WorldFiles) {
        saveToWorldFiles(text);
    }
}

void CodeEditor::finishSave(const QString& text) {
    _text = text;
    _dirty = false;
    _draft.clear();
    _hasDraft = false;
    _savedText = clockText();
    emit savedChanged();
    if (_place == Place::WrittenIn && _which == SCRIPT && text.trimmed().toUtf8().size() > INLINE_SCRIPT_LIMIT_BYTES) {
        setNote("Saved. A written-in script this long can't reach visitors: host it as a file and give its address.",
            false);
    } else {
        setNote(QString(), false);
    }
    qCDebug(interfaceapp) << "Code: saved" << _entity << _which << where();
}

// into the entity's field, as a Build mode edit with the person's own rights (the entity server applies its own
// rules): Build's Undo puts the old text back, and the changed field reloads the script for everyone
void CodeEditor::saveWrittenIn(const QString& text) {
    EntityItemProperties before;
    EntityItemProperties after;
    if (_which == SERVER_SCRIPTS) {
        before.setServerScripts(_value);
        after.setServerScripts(text);
    } else {
        before.setScript(_value);
        after.setScript(text);
    }
    _value = text;   // before the undo stack's indexChanged, so the panel doesn't take its own save for an undo
    if (auto editor = DependencyManager::get<Editor>()) {
        editor->undoStack().pushPropertyEdit(_which == SERVER_SCRIPTS ? "Edit Server Script" : "Edit Script",
            { _entity }, { before }, { after }, false);
    } else {
        DependencyManager::get<EntityScriptingInterface>()->editEntity(_entity, after);
    }
    finishSave(text);
}

// back to its path in the world's files with the asset server's own upload rights: the new text uploaded, the path
// pointed at it, then the script reloaded for everyone
void CodeEditor::saveToWorldFiles(const QString& text) {
    const QString path = _url.path();
    if (path.isEmpty() || !path.startsWith('/')) {
        setNote("This script's address in the world's files can't be saved to.", true);
        return;
    }
    setSaving(true);
    setNote("Saving…", false);
    const int generation = _generation;
    auto assetClient = DependencyManager::get<AssetClient>();
    auto upload = assetClient->createUpload(text.toUtf8());
    connect(upload, &AssetUpload::finished, this, [this, generation, path, text](AssetUpload* upload, const QString& hash) {
        upload->deleteLater();
        if (generation != _generation) {
            return;   // another script was opened meanwhile: the file is uploaded but its path isn't changed
        }
        if (upload->getError() != AssetUpload::NoError || hash.isEmpty()) {
            setSaving(false);
            setNote("Couldn't save: the world's files didn't take it.", true);
            qCWarning(interfaceapp) << "Code: the upload failed" << upload->getError();
            return;
        }
        auto request = DependencyManager::get<AssetClient>()->createSetMappingRequest(path, hash);
        connect(request, &SetMappingRequest::finished, this, [this, generation, text](SetMappingRequest* request) {
            request->deleteLater();
            if (generation != _generation) {
                return;
            }
            setSaving(false);
            if (request->getError() != MappingRequest::NoError) {
                setNote("Couldn't save: the world's files didn't take it.", true);
                qCWarning(interfaceapp) << "Code: setting the path failed" << request->getError();
                return;
            }
            finishSave(text);
            reloadForEveryone();
        });
        request->start();
    });
    upload->start();
}

void CodeEditor::reloadForEveryone() {
    if (!hasDocument() || !_problem.isEmpty()) {
        return;
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    if (_which == SERVER_SCRIPTS) {
        entities->reloadServerScripts(_entity);
    } else {
        // everyone's client loads it again when scriptTimestamp changes (and the script server its server script);
        // not an undo step: nothing about the world changes
        EntityItemProperties properties;
        properties.setScriptTimestamp((quint64)QDateTime::currentMSecsSinceEpoch());
        entities->editEntity(_entity, properties);
    }
    _status = "Reloading…";
    _statusState.clear();
    _errorLine = 0;
    _errorText.clear();
    _question = 0;
    emit statusChanged();
    QTimer::singleShot(1000, this, &CodeEditor::askStatus);
}

void CodeEditor::showInInspector() {
    if (!hasDocument()) {
        return;
    }
    if (auto editor = DependencyManager::get<Editor>()) {
        if (!editor->isActive()) {
            editor->setActive(true);
        }
        editor->select({ _entity });
    }
}

void CodeEditor::copyLink() {
    if (hasLink()) {
        QGuiApplication::clipboard()->setText(_url.toString());
    }
}

void CodeEditor::giveBackKeyboard() {
    if (auto viewport = qApp->getPrimaryWidget()) {
        viewport->activateWindow();
        viewport->setFocus(Qt::OtherFocusReason);
    }
}

void CodeEditor::setPanelOpen(bool open) {
    _panelOpen = open;
    if (open && hasDocument()) {
        _poll.start();
        askStatus();
    } else {
        _poll.stop();
    }
}

void CodeEditor::claudeBegin(const QUuid& entityId, const QString& which) {
    if (!isShowing(entityId, which) || !_problem.isEmpty()) {
        return;
    }
    if (_dirty || _hasDraft) {
        setNote("Claude is changing this script. Your changes here aren't saved: copy them before Claude finishes.",
            true);
        return;
    }
    closeChange();
    _claudeWriting = true;
    emit claudeChanged();
    emit streamBegan();
}

void CodeEditor::claudeText(const QUuid& entityId, const QString& which, const QString& chunk) {
    if (_claudeWriting && isShowing(entityId, which)) {
        emit streamed(chunk);
    }
}

void CodeEditor::claudeEnd(const QUuid& entityId, const QString& which, const QString& oldText, const QString& newText) {
    if (!isShowing(entityId, which)) {
        return;
    }
    const bool wasWriting = _claudeWriting;
    _claudeWriting = false;
    if (_place == Place::WrittenIn) {
        // the building tools wrote it into the field: that's the saved text now
        _value = newText;
    }
    if (wasWriting || (!_dirty && !_hasDraft)) {
        _text = newText;
        emit streamEnded(newText);
        _showingChange = oldText != newText;
        _changeOld = oldText;
        _changeNew = newText;
        if (_showingChange) {
            emit changeShown(oldText, newText);
        }
        setNote("Claude changed this script. Keep or Undo it on Make's card.", false);
    }
    emit claudeChanged();
}

bool CodeEditor::openChange(const QUuid& entityId, const QString& which, const QString& oldText, const QString& newText) {
    if (!open(entityId, which)) {
        return false;
    }
    _showingChange = oldText != newText;
    _changeOld = oldText;
    _changeNew = newText;
    emit claudeChanged();
    if (_showingChange) {
        emit changeShown(oldText, newText);
    }
    return true;
}

void CodeEditor::closeChange() {
    if (_showingChange) {
        _showingChange = false;
        _changeOld.clear();
        _changeNew.clear();
        emit claudeChanged();
        emit changeClosed();
    }
}

int CodeEditor::testRun(const QString& source) {
    if (!_pageReady) {
        return 0;
    }
    const int run = _nextTestRun++;
    emit testRequest("run", source, run);
    return run;
}

void CodeEditor::askStatus() {
    if (!_panelOpen || !hasDocument() || !_problem.isEmpty()) {
        _poll.stop();
        return;
    }
    if (!_poll.isActive()) {
        _poll.start();
    }
    updateRights();
    ScriptCallback callback;
    callback.id = _nextCallback++;
    callback.target = _target;
    _question = callback.id;
    DependencyManager::get<EntityScriptingInterface>()->queryPropertyMetadata(_entity, _which, callback);
}

// (error, result): the result's isRunning, status and errorInfo; an error when this client's engine or the script
// server has nothing on the entity
void CodeEditor::answered(int32_t callback, const QVariantList& arguments) {
    if (callback != _question) {
        return;   // an earlier question, or one about another script
    }
    const QVariantMap error = arguments.value(0).toMap();
    const QVariantMap result = arguments.value(1).toMap();
    const QString status = result.value("status").toString();
    const QString errorInfo = (result.isEmpty() ? error.value("message") : result.value("errorInfo")).toString().trimmed();
    QString text;
    QString state;
    if (status == "running") {
        text = "Running";
        state = "running";
    } else if (status == "pending" || status == "loading") {
        text = "Starting…";
    } else if (status == "error_loading_script") {
        text = "Couldn't load";
        state = "error";
    } else if (status == "error_running_script") {
        text = "Stopped with an error";
        state = "error";
    } else if (status == "unloaded") {
        text = "Not running";
    } else {
        text = _which == SERVER_SCRIPTS ? "No answer from the script server" : "Not running here";
    }
    QString shown;
    int line = 0;
    if (state == "error" && !errorInfo.isEmpty()) {
        readError(errorInfo, _url.isEmpty() ? QString() : _url.toString(), shown, line);
    }
    if (text != _status || state != _statusState || shown != _errorText || line != _errorLine) {
        _status = text;
        _statusState = state;
        _errorText = shown;
        _errorLine = line;
        emit statusChanged();
    }
}

void CodeEditor::refreshAfterUndo() {
    if (!hasDocument() || _place != Place::WrittenIn || _dirty) {
        return;
    }
    QString value;
    QString name;
    bool locked = false;
    if (!readEntity(value, name, locked) || value == _value) {
        return;
    }
    // the field changed under the panel (Build's Undo or Redo of a save, or the Inspector): show what it holds now
    _value = value;
    _text = value;
    if (!isAddress(value)) {
        emit documentChanged();
    } else {
        open(_entity, _which);   // it's an address now
    }
}
