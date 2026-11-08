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
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
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
#include <ScriptOpener.h>

#include "Application.h"
#include "CodeNearby.h"
#include "InterfaceLogging.h"
#include "NewScriptTemplates.h"
#include "Panels.h"
#include "editor/Editor.h"
#include "editor/EditorSelection.h"
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
    return ScriptOpener::isAddress(value);   // one rule with scripting's opener: one line with a scheme
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

// New Script's words (mary 11-08): the same wherever New is off
const QString NO_SCRIPTS_IN_A_FILE_WORLD = QStringLiteral("This world is a file on your computer, so it can't keep scripts.");
const QString NEW_SCRIPTS_FOLDER = QStringLiteral("/scripts/");
const int NEW_NAME_MAX = 40;
const int NEW_NAME_NUMBERED_MAX = 99;   // past "-99", 4 random hex digits instead

// a thing's name in a sentence ("Cube already has a script."); world text, shown as plain text only
QString thingName(const QString& name) {
    QString shown = name.simplified();
    if (shown.size() > 40) {
        shown = shown.left(39) + "…";
    }
    return shown.isEmpty() ? QStringLiteral("This thing") : shown;
}

// scripting's naming rule (11-08) for /scripts/<Thing>.js: from the thing's name, accents dropped (á→a, ñ→n), ASCII
// letters and digits kept, every run of anything else one "-" (a space, "#", "?" or "%" in the field's address means
// encoding questions in the script cache, Script.require and logs), at most 40 characters; no name left: the thing's
// type as the Inspector names it, or "Script". A server script's is "<Thing>-server".
QString newScriptBase(const QString& name, const QString& typeName, bool server) {
    auto clean = [](const QString& text) {
        const QString decomposed = text.normalized(QString::NormalizationForm_KD);
        QString out;
        bool dash = false;
        for (const QChar c : decomposed) {
            if (c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining ||
                c.category() == QChar::Mark_Enclosing) {
                continue;
            }
            if (c.unicode() < 128 && c.isLetterOrNumber()) {
                out.append(c);
                dash = false;
            } else if (!dash) {
                out.append('-');
                dash = true;
            }
        }
        auto trimDashes = [](QString& t) {
            while (t.startsWith('-')) {
                t.remove(0, 1);
            }
            while (t.endsWith('-')) {
                t.chop(1);
            }
        };
        trimDashes(out);
        if (out.size() > NEW_NAME_MAX) {
            out.truncate(NEW_NAME_MAX);
            trimDashes(out);
        }
        return out;
    };
    QString base = clean(name);
    if (base.isEmpty()) {
        base = clean(typeName);
    }
    if (base.isEmpty()) {
        base = QStringLiteral("Script");
    }
    return server ? base + QStringLiteral("-server") : base;
}

// an entity's script field, name, type and lock, for New Script; false when it isn't there
bool readForNew(const QUuid& id, const QString& which, QString& value, QString& name, QString& typeName, bool& locked) {
    EntityPropertyFlags flags;
    flags += PROP_NAME;
    flags += PROP_LOCKED;
    flags += which == SERVER_SCRIPTS ? PROP_SERVER_SCRIPTS : PROP_SCRIPT;
    const EntityItemProperties properties =
        DependencyManager::get<EntityScriptingInterface>()->getEntityProperties(id, flags);
    if (properties.getType() == EntityTypes::Unknown) {
        return false;
    }
    value = which == SERVER_SCRIPTS ? properties.getServerScripts() : properties.getScript();
    name = properties.getName();
    typeName = EntityTypes::getEntityTypeName(properties.getType());
    locked = properties.getLocked();
    return true;
}

}

CodeEditor::CodeEditor() {
    // the header's properties change with every document too
    connect(this, &CodeEditor::documentChanged, this, &CodeEditor::headerChanged);
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
        // the header's New Script follows Build mode and its selection
        connect(&editor->selection(), &EditorSelection::changed, this, &CodeEditor::updateNewScript);
        connect(editor.data(), &Editor::activeChanged, this, &CodeEditor::updateNewScript);
    }
    // the world's rights change with the world, and with an instance's edit mode
    auto nodeList = DependencyManager::get<NodeList>();
    connect(nodeList.data(), &LimitedNodeList::canRezChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezTmpChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canWriteAssetsChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezCertifiedChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::isAllowedEditorChanged, this, &CodeEditor::updateRights);
    connect(nodeList.data(), &LimitedNodeList::canRezChanged, this, &CodeEditor::updateNewScript);
    connect(nodeList.data(), &LimitedNodeList::canRezTmpChanged, this, &CodeEditor::updateNewScript);
    connect(nodeList.data(), &LimitedNodeList::canWriteAssetsChanged, this, &CodeEditor::updateNewScript);
    connect(nodeList.data(), &LimitedNodeList::isAllowedEditorChanged, this, &CodeEditor::updateNewScript);
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
        updateNewScript();   // a world from a file can't keep scripts
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
    if (_isNew) {
        // a new script's own key (its unsaved typing is kept apart from a script written into the field)
        return QStringLiteral("new|") + _which + QLatin1Char('|') + _entity.toString();   // no row of the list has it
    }
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
        if (_isNew) {
            usedBy = _entityName.isEmpty() ? QString() : "for " + _entityName;   // nothing uses it yet
        } else if (_nearby->find(openKey(), nearest, distance, users)) {
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
    } else if (trimmed.isEmpty()) {
        // no script yet: a new one, made in the world's files at its first Save (New Script, 11-08); never written
        // into the field, where one over about 1 KB never reaches visitors. Where it can't be made, the panel says why
        const QString why = whyNoNewScript(entityId, which);
        if (why.isEmpty()) {
            return openNew(entityId, which);
        }
        _fileName = _entityName.isEmpty() ? "Script" : _entityName;
        _problem = why;
    } else if (!isAddress(trimmed)) {
        // written into the field
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

bool CodeEditor::isFileWorld() {
    return DependencyManager::get<NodeList>()->getDomainHandler().isServerless();
}

QString CodeEditor::whyNoNewScript(const QUuid& entityId, const QString& which) {
    if (isFileWorld()) {
        return NO_SCRIPTS_IN_A_FILE_WORLD;   // a world opened from a file has no files to keep a script in
    }
    QString value;
    QString name;
    QString typeName;
    bool locked = false;
    if (entityId.isNull() || !readForNew(entityId, which, value, name, typeName, locked)) {
        return "This thing isn't in the world any more.";
    }
    if (!entity_script_rights::mayChangeScripts()) {
        return "Only this world's builders can change its scripts.";
    }
    if (!value.trimmed().isEmpty()) {
        return thingName(name) + (which == SERVER_SCRIPTS ? " already has a server script." : " already has a script.");
    }
    if (!entity_script_rights::mayChangeScriptsOf(locked)) {
        return "Locked: unlock it in the Inspector to change its script.";
    }
    if (!DependencyManager::get<NodeList>()->getThisNodeCanWriteAssets()) {
        return "You can't add files to this world. Its owner can allow it.";
    }
    return QString();
}

bool CodeEditor::openNew(const QUuid& entityId, const QString& which) {
    if (!isAvailable() || entityId.isNull() || (which != SCRIPT && which != SERVER_SCRIPTS) ||
        !whyNoNewScript(entityId, which).isEmpty()) {
        return false;
    }
    if (_isNew && _entity == entityId && _which == which) {
        DependencyManager::get<Panels>()->open(PANEL);   // already open, with whatever was typed
        return true;
    }
    clear();
    _docWorld = worldKey();
    _entity = entityId;
    _which = which;
    QString value;
    QString typeName;
    bool locked = false;
    if (!readForNew(entityId, which, value, _entityName, typeName, locked)) {
        clear();
        emit documentChanged();
        return false;
    }
    ++_generation;
    _isNew = true;
    _value = value;   // the empty field
    _place = Place::WorldFiles;
    _newBase = newScriptBase(_entityName, typeName, which == SERVER_SCRIPTS);
    _fileName = _newBase + ".js";
    _text = QString::fromUtf8(which == SERVER_SCRIPTS ? new_script_templates::SERVER_SCRIPT
                                                      : new_script_templates::ENTITY_SCRIPT);
    updateRights();
    updateUsedBy();
    qCDebug(interfaceapp) << "Code: new script" << _entity << _which << _fileName;
    emit documentChanged();
    emit savedChanged();
    emit noteChanged();
    emit statusChanged();
    emit claudeChanged();
    updateNewScript();
    DependencyManager::get<Panels>()->open(PANEL);
    return true;
}

void CodeEditor::newScript() {
    updateNewScript();
    auto editor = DependencyManager::get<Editor>();
    if (!_newScriptOff.isEmpty() || !editor || editor->selection().ids().size() != 1) {
        return;
    }
    openNew(editor->selection().ids().first(), SCRIPT);
}

// the header's New Script, for Build mode's selection now (mary 11-08): an entity script for one thing; worked out
// only while the panel is open, so a closed panel costs nothing
void CodeEditor::updateNewScript() {
    if (!_panelOpen) {
        return;
    }
    QString off;
    QString tip;
    auto editor = DependencyManager::get<Editor>();
    const QList<QUuid> ids = editor ? editor->selection().ids() : QList<QUuid>();
    if (isFileWorld()) {
        off = NO_SCRIPTS_IN_A_FILE_WORLD;
    } else if (!editor || !editor->isActive()) {
        off = "Scripts are added in Build mode.";
    } else if (ids.isEmpty()) {
        off = "Select a thing first.";
    } else if (ids.size() > 1) {
        off = "Select just one thing.";
    } else if (_isNew && _entity == ids.first() && _which == SCRIPT) {
        const QString name = thingName(_entityName);
        off = name + (name.endsWith('s') ? "' new script is open." : "'s new script is open.");
    } else {
        off = whyNoNewScript(ids.first(), SCRIPT);
        if (off.isEmpty()) {
            QString value;
            QString name;
            QString typeName;
            bool locked = false;
            readForNew(ids.first(), SCRIPT, value, name, typeName, locked);
            tip = name.simplified().isEmpty() ? QStringLiteral("Start a new script for this thing")
                                              : "Start a new script for " + thingName(name);
        }
    }
    if (off != _newScriptOff || tip != _newScriptTip) {
        _newScriptOff = off;
        _newScriptTip = tip;
        emit newScriptChanged();
    }
}

// a script file's text, fetched by scripting's ScriptOpener under the rules any script of that world gets (public
// addresses only for a world from the network, a size cap, a time limit; file: refused)
void CodeEditor::fetch() {
    // scripting's ScriptOpener (Make v1 piece 7): the entity's script by its id, under the world's own rules; a
    // refusal answers at once, a download on this object's thread and never after it's gone
    _loading = true;
    const int generation = _generation;
    ScriptOpener::open(_entity, _which, this, [this, generation](const ScriptOpener::Opened& opened) {
        if (generation != _generation) {
            return;   // another file opened meanwhile
        }
        _loading = false;
        if (opened.ok()) {
            _text = opened.text;
            _problem.clear();
        } else {
            _text.clear();
            _problem = opened.problem;
        }
        qCDebug(interfaceapp) << "Code: script" << (opened.ok() ? "loaded" : "not loaded") << _entity << _which << opened.where;
        emit documentChanged();
    });
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
    _isNew = false;
    _newBase.clear();
    _madePath.clear();
    _madeHash.clear();
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
        if (_isNew) {
            // nothing of it is in the world yet: its first Save needs what New needs (the field still empty, too)
            reason = whyNoNewScript(_entity, _which);
        } else if (!there) {
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
        canSave = _isNew ? reason.isEmpty() : mayEdit && !isShader() && (_place != Place::Web) &&
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
    if (_isNew) {
        saveNew(text);
    } else if (_place == Place::WrittenIn) {
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
        connect(request, &SetMappingRequest::finished, this, [this, generation, text, path, hash](SetMappingRequest* request) {
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
            if (path == _madePath) {
                _madeHash = hash;   // still this document's own file, for an Undo then Save again
            }
            finishSave(text);
            reloadForEveryone();
        });
        request->start();
    });
    upload->start();
}

// a new script's first Save (New Script, 11-08): the text into the world's files, then a free name under /scripts/
// for it, then the field pointed at it as ONE Build mode edit (Build's Undo takes the field back; the file stays)
void CodeEditor::saveNew(const QString& text) {
    setSaving(true);
    setNote("Saving…", false);
    const int generation = _generation;
    auto upload = DependencyManager::get<AssetClient>()->createUpload(text.toUtf8());
    connect(upload, &AssetUpload::finished, this, [this, generation, text](AssetUpload* upload, const QString& hash) {
        upload->deleteLater();
        if (generation != _generation) {
            return;   // another document meanwhile: nothing is named or pointed at
        }
        if (upload->getError() != AssetUpload::NoError || hash.isEmpty()) {
            setSaving(false);
            setNote("Couldn't save: the world's files didn't take it.", true);
            qCWarning(interfaceapp) << "Code: a new script's upload failed" << upload->getError();
            return;
        }
        pickFreePath(hash, text, generation);
    });
    upload->start();
}

// scripting's rule: the first free name of "<base>.js", "<base>-2.js" … "<base>-99.js", then "<base>-<4 hex>.js",
// against ONE look at every name in the world's files, compared without case ("cube.js" beside "Cube.js" collide in a
// download on a Mac). SetMapping replaces a name without asking, so this look is what keeps someone's file safe; the
// asset server has no create-only call, so a tiny window stays between the look and the SetMapping. The file this
// document made itself, unchanged since (an Undo, then Save again), counts as free: no "Cube-2.js" for the same script.
void CodeEditor::pickFreePath(const QString& hash, const QString& text, int generation) {
    auto request = DependencyManager::get<AssetClient>()->createGetAllMappingsRequest();
    connect(request, &GetAllMappingsRequest::finished, this, [this, hash, text, generation](GetAllMappingsRequest* request) {
        request->deleteLater();
        if (generation != _generation) {
            return;
        }
        if (request->getError() != MappingRequest::NoError) {
            setSaving(false);
            setNote("Couldn't save: the world's files didn't take it.", true);
            qCWarning(interfaceapp) << "Code: reading the world's file names failed" << request->getError();
            return;
        }
        QSet<QString> taken;
        const auto mappings = request->getMappings();
        for (const auto& mapping : mappings) {
            if (mapping.first == _madePath && mapping.second.hash == _madeHash) {
                continue;   // this document's own file
            }
            taken.insert(mapping.first.toLower());
        }
        QString path;
        for (int number = 1; number <= NEW_NAME_NUMBERED_MAX + 20 && path.isEmpty(); ++number) {
            QString name = _newBase;
            if (number > NEW_NAME_NUMBERED_MAX) {
                name += '-' + QString::number(QRandomGenerator::global()->bounded(0x10000), 16).rightJustified(4, '0');
            } else if (number > 1) {
                name += '-' + QString::number(number);
            }
            const QString candidate = NEW_SCRIPTS_FOLDER + name + ".js";
            if (!taken.contains(candidate.toLower())) {
                path = candidate;
            }
        }
        if (path.isEmpty()) {
            setSaving(false);
            setNote("Couldn't save: no free name for it in the world's files.", true);
            return;
        }
        auto set = DependencyManager::get<AssetClient>()->createSetMappingRequest(path, hash);
        connect(set, &SetMappingRequest::finished, this, [this, path, hash, text, generation](SetMappingRequest* set) {
            set->deleteLater();
            if (generation != _generation) {
                return;
            }
            if (set->getError() != MappingRequest::NoError) {
                setSaving(false);
                setNote("Couldn't save: the world's files didn't take it.", true);
                qCWarning(interfaceapp) << "Code: naming a new script failed" << set->getError();
                return;
            }
            pointAtNewFile(path, hash, text);
        });
        set->start();
    });
    request->start();
}

// the field pointed at the new file, as one Build mode edit with the person's own rights; everyone's client (or the
// script server) loads it because the field changed
void CodeEditor::pointAtNewFile(const QString& path, const QString& hash, const QString& text) {
    setSaving(false);
    QString value;
    QString name;
    QString typeName;
    bool locked = false;
    if (!readForNew(_entity, _which, value, name, typeName, locked)) {
        setNote("Saved in the world's files as " + path + ", but this thing isn't in the world any more.", true);
        return;
    }
    if (!value.trimmed().isEmpty()) {
        // someone gave it a script meanwhile: never written over
        setNote("Saved in the world's files as " + path + ", but " + thingName(name) +
            " got a script meanwhile, so this one wasn't added.", true);
        return;
    }
    const QString address = "atp:" + path;
    if (_drafts.remove(docDraftKey()) > 0) {   // the typing kept under "new" is in the file now
        emit draftsChanged();
    }
    _madePath = path;
    _madeHash = hash;
    _isNew = false;
    _url = QUrl(address);
    _fileName = _url.fileName();
    _value = address;   // before the undo stack's indexChanged, so the panel doesn't take its own save for an undo
    EntityItemProperties before;
    EntityItemProperties after;
    if (_which == SERVER_SCRIPTS) {
        before.setServerScripts(value);
        after.setServerScripts(address);
    } else {
        before.setScript(value);
        after.setScript(address);
    }
    if (auto editor = DependencyManager::get<Editor>()) {
        _pushingEdit = true;   // the push's own indexChanged isn't an Undo
        editor->undoStack().pushPropertyEdit(_which == SERVER_SCRIPTS ? "New Server Script" : "New Script", { _entity },
            { before }, { after }, false);
        _pushingEdit = false;
    } else {
        DependencyManager::get<EntityScriptingInterface>()->editEntity(_entity, after);
    }
    finishSave(text);
    updateRights();
    updateUsedBy();
    emit headerChanged();
    updateNewScript();
    qCDebug(interfaceapp) << "Code: new script made" << _entity << _which << path;
    askStatus();
}

void CodeEditor::reloadForEveryone() {
    if (!hasDocument() || !_problem.isEmpty() || isShader() || _isNew) {
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
    updateNewScript();
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
    if (!_panelOpen || !hasDocument() || !_problem.isEmpty() || isShader() || _isNew) {
        _poll.stop();
        return;
    }
    updateNewScript();   // the selected thing's field can change by anyone's edit
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
    updateNewScript();   // an Undo can give the selected thing a script, or take it away
    if (_pushingEdit) {
        return;
    }
    if (hasDocument() && (_isNew || !_madePath.isEmpty())) {
        QString value;
        QString name;
        QString typeName;
        bool locked = false;
        if (!readForNew(_entity, _which, value, name, typeName, locked) || value == _value) {
            return;
        }
        if (!_isNew && value.trimmed().isEmpty()) {
            // Build's Undo of a new script's first Save: the thing has no script again. The text stays here, not saved
            // yet (the panel's own text is left as it is); its file stays in the world's files, and Save uses it again
            _isNew = true;
            _value = value;
            _url = QUrl();
            _fileName = _newBase + ".js";
            _savedText.clear();
            _status.clear();
            _statusState.clear();
            _errorLine = 0;
            _errorText.clear();
            _question = 0;
            _poll.stop();
            setNote(QString(), false);
            updateRights();
            updateUsedBy();
            emit headerChanged();
            emit savedChanged();
            emit statusChanged();
            updateNewScript();
            return;
        }
        if (_isNew && !value.trimmed().isEmpty() && !_dirty) {
            open(_entity, _which);   // Redo (or anyone's edit): the field holds a script now, shown as any other
        }
        return;
    }
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
