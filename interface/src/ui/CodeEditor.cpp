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

#include <functional>

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMenu>
#include <QUndoStack>   // QtWidgets in Qt 5, QtGui in Qt 6

#include <AssetClient.h>
#include <AssetUpload.h>
#include <MainWindow.h>   // the menu's parent
#include <material-networking/ShaderCache.h>
#include <EntityItemProperties.h>
#include <EntityScriptRights.h>
#include <EntityScriptingInterface.h>
#include <MappingRequest.h>
#include <NodeList.h>
#include <ScriptCallback.h>

#include "Application.h"
#include "CodeNearby.h"
#include "InterfaceLogging.h"
#include "Panels.h"
#include "editor/Editor.h"
#include "editor/EditorUndo.h"

namespace {

const QString PANEL = QStringLiteral("Code");
const QString SCRIPT = QStringLiteral("script");
const QString SERVER_SCRIPTS = QStringLiteral("serverScripts");
const QString FRAGMENT_SHADER = QStringLiteral("fragmentShader");
const QString VERTEX_SHADER = QStringLiteral("vertexShader");
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
    _nearby = new CodeNearby(this);
    _poll.setInterval(POLL_MS);
    connect(&_poll, &QTimer::timeout, this, &CodeEditor::askStatus);
    _target = std::make_shared<AnswerTarget>([this](int32_t callback, const QVariantList& arguments) {
        answered(callback, arguments);
    });
    // the header's "used by … (3.1 m)" follows the list's look at the world
    connect(_nearby, &QAbstractItemModel::dataChanged, this, &CodeEditor::updateUsedBy);
    connect(_nearby, &QAbstractItemModel::modelReset, this, &CodeEditor::updateUsedBy);
    // Build's Undo of a save (or of any edit of the field) shows in the panel, unless the person has typed since
    if (auto editor = DependencyManager::get<Editor>()) {
        connect(editor->undoStack().stack(), &QUndoStack::indexChanged, this, &CodeEditor::refreshAfterUndo);
    }
    // the world's rights change with the world, and with an instance's edit mode
    auto nodeList = DependencyManager::get<NodeList>();
    connect(nodeList.data(), &LimitedNodeList::canRezChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezTmpChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canWriteAssetsChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezCertifiedChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::isAllowedEditorChanged, this, &CodeEditor::updateRights);
    // unsaved changes are kept per world: the same atp: path, or an entity id copied with a world, in another world is
    // another file, and arriving there clears the open document, whose entity isn't that world's (security 11-07)
    auto& domainHandler = nodeList->getDomainHandler();
    connect(&domainHandler, &DomainHandler::domainURLChanged, this, [this](QUrl url) {
        const QString address = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        if (address != _worldAddress) {
            _worldAddress = address;
            _worldID = QUuid();
            enterWorld();
        }
    });
    connect(&domainHandler, &DomainHandler::connectedToDomain, this, [this] {
        const QUuid id = DependencyManager::get<NodeList>()->getDomainHandler().getUUID();
        if (id == _worldID) {
            return;   // the same domain again (a reconnect)
        }
        const bool another = !_worldID.isNull();   // another domain at the same address
        _worldID = id;
        if (another) {
            enterWorld();
        } else {
            emit draftsChanged();
        }
    });
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
    return _which == SERVER_SCRIPTS ? "Server script" : _which == FRAGMENT_SHADER ? "Fragment shader"
        : _which == VERTEX_SHADER ? "Vertex shader" : "Entity script";
}

bool CodeEditor::isShader() const {
    return _which == FRAGMENT_SHADER || _which == VERTEX_SHADER;
}

QString CodeEditor::openKey() const {
    return hasDocument() ? CodeNearby::keyFor(_which, _value, _entity) : QString();
}

QString CodeEditor::path() const {
    if (!hasDocument() || _url.isEmpty()) {
        return QString();
    }
    return CodeNearby::pathOf(_url.toString());
}

QString CodeEditor::worldKey() const {
    if (!_testWorld.isEmpty()) {
        return "test:" + _testWorld;
    }
    return _worldAddress + QLatin1Char('|') + _worldID.toString(QUuid::WithoutBraces);
}

QStringList CodeEditor::draftKeys() const {
    const QString prefix = worldKey() + QLatin1Char('\n');
    QStringList keys;
    for (auto it = _drafts.cbegin(); it != _drafts.cend(); ++it) {
        if (it.key().startsWith(prefix)) {
            keys.append(it.key().mid(prefix.size()));
        }
    }
    return keys;
}

QString CodeEditor::openStamp() const {
    return hasDocument() ? QString::number(_generation) + QLatin1Char('\n') + docDraftKey() : QString();
}

bool CodeEditor::hasDraft() const {
    return hasDocument() && _drafts.contains(docDraftKey());
}

QString CodeEditor::draft() const {
    return hasDocument() ? _drafts.value(docDraftKey()) : QString();
}

void CodeEditor::enterWorld() {
    emit draftsChanged();   // the list's Edited marks are this world's
    if (!hasDocument() || _leaving) {
        return;
    }
    if (_dirty && _pageReady) {
        // the person's typing since the last kept draft goes into the old world's drafts before the document goes
        // (security 11-07); nothing is saved meanwhile, and the document goes anyway if the page doesn't answer
        static const int LEAVE_WAIT_MS = 1500;
        _leaving = true;
        updateRights();
        const int generation = _generation;
        emit keepBeforeLeaving();
        QTimer::singleShot(LEAVE_WAIT_MS, this, [this, generation] {
            if (_leaving && generation == _generation) {
                qCWarning(interfaceapp) << "Code: the page didn't answer before leaving the world; its last draft is kept";
                finishLeaving();
            }
        });
    } else {
        finishLeaving();
    }
}

void CodeEditor::leaveDocument(const QString& stamp) {
    if (_leaving && stamp == openStamp()) {
        finishLeaving();
    }
}

void CodeEditor::finishLeaving() {
    clear();
    emit documentChanged();
    emit rightsChanged();
    emit draftsChanged();
}

void CodeEditor::testSetWorld(const QString& name) {
    if (name != _testWorld) {
        _testWorld = name;
        enterWorld();
    }
}

void CodeEditor::setDraft(const QString& text, bool has) {
    if (!hasDocument()) {
        return;
    }
    const QString key = docDraftKey();
    const bool had = _drafts.contains(key);
    if (has) {
        _drafts.insert(key, text);
    } else {
        _drafts.remove(key);
    }
    if (had != has) {
        emit draftsChanged();
    }
}

void CodeEditor::updateUsedBy() {
    QString usedBy;
    int users = 0;
    if (hasDocument()) {
        QString nearest;
        QString distance;
        if (_nearby->find(openKey(), nearest, distance, users)) {
            // a written-in script is its entity's own: the header's first line already names it
            usedBy = _place == Place::WrittenIn ? distance + " away" : "used by " + nearest + " (" + distance + ")";
        } else if (_place != Place::WrittenIn && !_entityName.isEmpty()) {
            usedBy = "used by " + _entityName;
            users = 1;
        }
    }
    if (usedBy != _usedBy || users != _sharedBy) {
        _usedBy = usedBy;
        _sharedBy = users;
        emit usedByChanged();
    }
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
    if (isShader()) {
        flags += PROP_USER_DATA;
        flags += PROP_MATERIAL_URL;
        flags += PROP_MATERIAL_DATA;
    } else {
        flags += _which == SERVER_SCRIPTS ? PROP_SERVER_SCRIPTS : PROP_SCRIPT;
    }
    EntityItemProperties properties = entities->getEntityProperties(_entity, flags);
    if (properties.getType() == EntityTypes::Unknown) {
        return false;
    }
    if (isShader()) {
        // the entity's own shader of that kind, as the renderer reads it: its address, never one given from outside
        value.clear();
        const auto shaders = CodeNearby::shadersOf(properties.getType() == EntityTypes::Material,
            properties.getUserData(), properties.getMaterialURL(), properties.getMaterialData());
        for (const auto& shader : shaders) {
            if (shader.first == _which) {
                value = shader.second.toString();
                break;
            }
        }
        name = properties.getName();
        locked = properties.getLocked();
        return true;
    }
    value = _which == SERVER_SCRIPTS ? properties.getServerScripts() : properties.getScript();
    name = properties.getName();
    locked = properties.getLocked();
    return true;
}

bool CodeEditor::open(const QUuid& entityId, const QString& which) {
    if (!isAvailable() || entityId.isNull() ||
        (which != SCRIPT && which != SERVER_SCRIPTS && which != FRAGMENT_SHADER && which != VERTEX_SHADER)) {
        return false;
    }
    clear();
    _docWorld = worldKey();
    _entity = entityId;
    _which = which;
    QString value;
    bool locked = false;
    if (!readEntity(value, _entityName, locked) || (isShader() && value.isEmpty())) {
        clear();
        emit documentChanged();
        return false;
    }
    ++_generation;
    _value = value;
    updateRights();

    const QString trimmed = value.trimmed();
    if (isShader()) {
        // read only here (Caity 11-07): from the shader cache the renderer draws it from (shadersOf: network only)
        _url = QUrl(trimmed);
        _fileName = _url.fileName().isEmpty() ? _url.toDisplayString() : _url.fileName();
        _place = _url.scheme().toLower() == "atp" ? Place::WorldFiles : Place::Web;
        fetchShader();
    } else if (which == SERVER_SCRIPTS && !_canSave) {
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
    updateUsedBy();
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

// a shader's text from the shader cache, the same copy the renderer draws (already there for one drawn nearby);
// otherwise the cache fetches it as the renderer would, under the same rules
void CodeEditor::fetchShader() {
    _shader = ShaderCache::instance().getShader(_url);
    if (!_shader) {
        _problem = "This shader couldn't be loaded.";
        return;
    }
    auto take = [this](bool success) {
        if (!success || !_shader || !_shader->isLoaded()) {
            _problem = "This shader couldn't be loaded.";
            _text.clear();
        } else if (_shader->_source.toUtf8().size() > MAX_SAVE_BYTES) {
            _problem = "This shader is too big to show here (1 MB at most).";
            _text.clear();
        } else {
            _text = _shader->_source;
        }
        _loading = false;
        _shader.reset();
    };
    if (_shader->isLoaded() || _shader->isFailed()) {
        take(_shader->isLoaded());
        return;
    }
    _loading = true;
    const int generation = _generation;
    _shaderConnection = connect(_shader.data(), &Resource::finished, this, [this, take, generation](bool success) {
        QObject::disconnect(_shaderConnection);
        if (generation != _generation || !_shader) {
            return;   // another file opened meanwhile
        }
        take(success);
        qCDebug(interfaceapp) << "Code: shader" << (success ? "loaded" : "failed") << _entity << _which;
        emit documentChanged();
    });
}

void CodeEditor::clear() {
    ++_generation;
    _leaving = false;
    _docWorld.clear();
    _entity = QUuid();
    _which.clear();
    _value.clear();
    _url = QUrl();
    _place = Place::None;
    _fileName.clear();
    _entityName.clear();
    _text.clear();
    if (_shader) {
        QObject::disconnect(_shaderConnection);
        _shader.reset();
    }
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
        auto nodeList = DependencyManager::get<NodeList>();
        QString value;
        QString name;
        bool locked = false;
        const bool there = readEntity(value, name, locked);
        // the entity server's own rule (EntityScriptRights.h): someone who may only make temporary entities has
        // their script changes kept from everyone else
        const bool mayEdit = there && entity_script_rights::mayChangeScriptsOf(locked);
        if (!there) {
            reason = "This thing isn't in the world any more.";
        } else if (isShader()) {
            reason = "Shaders are read only here.";
        } else if (_place == Place::Web) {
            reason = "On the web: read only here.";
        } else if (!entity_script_rights::mayChangeScripts()) {
            reason = "Only this world's builders can change its scripts.";
        } else if (locked) {
            reason = "Locked: unlock it in the Inspector to change its script.";
        } else if (_place == Place::WorldFiles && !nodeList->getThisNodeCanWriteAssets()) {
            reason = "You can't change this world's files.";
        }
        // a server script's rights decide whether it opens at all (open()), so they're read before the place is known
        canSave = mayEdit && !isShader() && (_place != Place::Web) &&
            (_place != Place::WorldFiles || nodeList->getThisNodeCanWriteAssets());
        if (_leaving) {
            canSave = false;
            reason = "You've left this world: your change is kept for when you're back.";
        }
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
    if (!hasDocument() || !_problem.isEmpty() || _saving || _claudeWriting || _leaving) {
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
    if (_drafts.remove(docDraftKey()) > 0) {
        emit draftsChanged();
    }
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
    if (!hasDocument() || !_problem.isEmpty() || isShader()) {
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

QMenu* CodeEditor::makeMoreMenu() {
    auto menu = new QMenu(qApp->getWindow());
    auto inspector = menu->addAction("Show in Inspector");
    inspector->setEnabled(hasDocument());
    connect(inspector, &QAction::triggered, this, &CodeEditor::showInInspector);
    auto link = menu->addAction("Copy Link");
    link->setEnabled(hasLink());
    connect(link, &QAction::triggered, this, &CodeEditor::copyLink);
    return menu;
}

void CodeEditor::showMoreMenu(qreal x, qreal y) {
    auto menu = makeMoreMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QPoint(qRound(x), qRound(y)));
}

bool CodeEditor::saveMenuPicture(const QString& which, const QString& path) {
    if (which != "more") {
        return false;
    }
    QMenu* menu = makeMoreMenu();
    menu->ensurePolished();
    menu->adjustSize();
    const bool saved = menu->grab().save(path);   // drawn as it would show, without showing it
    delete menu;
    return saved;
}

void CodeEditor::refreshNearby() {
    _nearby->refresh();
}

QObject* CodeEditor::nearby() const {
    return _nearby;
}

void CodeEditor::setNearbyShown(bool shown) {
    _nearbyShown = shown;
    _nearby->setActive(_panelOpen && _nearbyShown);
}

void CodeEditor::setPanelOpen(bool open) {
    _panelOpen = open;
    _nearby->setActive(_panelOpen && _nearbyShown);
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
    if (_dirty || hasDraft()) {
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
    if (wasWriting || (!_dirty && !hasDraft())) {
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

void CodeEditor::testShowNearby() {
    clear();
    emit documentChanged();
    emit rightsChanged();
    emit savingChanged();
    emit savedChanged();
    emit noteChanged();
    emit statusChanged();
    emit claudeChanged();
    updateUsedBy();
    DependencyManager::get<Panels>()->open(PANEL);
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
    if (!_panelOpen || !hasDocument() || !_problem.isEmpty() || isShader()) {
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
