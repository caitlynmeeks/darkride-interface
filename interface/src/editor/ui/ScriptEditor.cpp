//
//  ScriptEditor.cpp
//  interface/src/editor/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ScriptEditor.h"

#include <algorithm>
#include <functional>

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <DependencyManager.h>
#include <EntityItemProperties.h>
#include <EntityScriptingInterface.h>
#include <ScriptCallback.h>

#include "../../InterfaceLogging.h"
#include "../../ui/CodeEditor.h"
#include "../PropertySpec.h"

namespace {

const int POLL_MS = 2000;
const qint64 ANSWER_WAIT_MS = 10000;   // questions unanswered this long are asked again
const int MAX_ASKED = 50;              // a bigger selection's status is about its first 50
const int MAX_ERROR_SHOWN = 300;       // characters of an error on the line; all of it in the tooltip

// queryPropertyMetadata answers through a ScriptCallback, as it answers a script's function; this stands in for the
// script engine and hands each answer, by its callback id, to the editor on the main thread
class AnswerTarget : public CallbackTarget {
public:
    explicit AnswerTarget(std::function<void(int32_t, const QVariantList&)> onAnswer) : _onAnswer(std::move(onAnswer)) {}
    bool invoke(int32_t callback, const QVariantList& arguments, QVariant&, QString&) override {
        post(callback, arguments);
        return true;
    }
    void post(int32_t callback, const QVariantList& arguments) override {
        auto onAnswer = _onAnswer;
        QMetaObject::invokeMethod(qApp, [onAnswer, callback, arguments] { onAnswer(callback, arguments); }, Qt::QueuedConnection);
    }
    void request(int32_t callback, const QVariantList& arguments, std::function<void(const QVariant&)>) override {
        post(callback, arguments);
    }

private:
    std::function<void(int32_t, const QVariantList&)> _onAnswer;
};

bool isError(const QString& status) {
    return status == "error_loading_script" || status == "error_running_script";
}

// an engine's error for the status line: its first line, the error's name once ("SyntaxError: SyntaxError: …" from the
// engine), and the line of the script it points at; the whole of it goes in the tooltip
QString shortError(const QString& error, const QString& link) {
    QString first = error.section('\n', 0, 0).trimmed();
    const int colon = first.indexOf(": ");
    if (colon > 0 && first.mid(colon + 2).startsWith(first.left(colon + 2))) {
        first = first.mid(colon + 2);
    }
    if (!link.isEmpty()) {
        const QRegularExpression at(QRegularExpression::escape(link) + ":(\\d+)(:\\d+)?");
        const auto match = at.match(error);
        if (match.hasMatch()) {
            first += QString(" (line %1)").arg(match.captured(1));
        }
    }
    return first;
}

}

ScriptEditor::ScriptEditor(const PropertySpec& spec, QWidget* parent) : PropertyEditor(spec, parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    _url = new TextEditor(spec, this);
    // the link, and Edit beside it: the script in the Code panel (Make v1; Qt 6 only, ui/CodeEditor.h); New in Edit's
    // place while the field is empty (New Script, mary 11-08): a starter script, made in the world's files at its
    // first Save
    auto linkRow = new QHBoxLayout();
    linkRow->setContentsMargins(0, 0, 0, 0);
    linkRow->setSpacing(6);
    linkRow->addWidget(_url, 1);
    _edit = new QPushButton("Edit", this);
    _edit->setVisible(false);
    linkRow->addWidget(_edit, 0, Qt::AlignTop);
    layout->addLayout(linkRow);
    connect(_edit, &QPushButton::clicked, this, [this] {
        if (_ids.size() != 1) {
            return;
        }
        auto code = DependencyManager::get<CodeEditor>();
        if (_link.isEmpty()) {
            if (!code->openNew(_ids.first(), _spec.key)) {
                showEdit();   // it can't now (the world or the rights changed): the button says why
            }
        } else {
            code->open(_ids.first(), _spec.key);
        }
    });
    connect(_url, &PropertyEditor::editBegan, this, &PropertyEditor::editBegan);
    connect(_url, &PropertyEditor::liveValue, this, &PropertyEditor::liveValue);
    connect(_url, &PropertyEditor::committed, this, &PropertyEditor::committed);
    connect(_url, &PropertyEditor::editCancelled, this, &PropertyEditor::editCancelled);
    connect(_url, &PropertyEditor::liveValue, this, [this](const QVariant& value) { showInlineNote(value.toString()); });
    connect(_url, &PropertyEditor::committed, this, [this](const QVariant& value) { showInlineNote(value.toString()); });
    _inlineNote = new QLabel("An inline script this long can't reach visitors. Host it as a file and give its address.", this);
    _inlineNote->setObjectName("EditorScriptStatus");
    _inlineNote->setWordWrap(true);
    _inlineNote->hide();
    layout->addWidget(_inlineNote);

    _statusRow = new QWidget(this);
    auto row = new QHBoxLayout(_statusRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    _status = new QLabel(_statusRow);
    _status->setObjectName("EditorScriptStatus");
    _status->setWordWrap(true);
    _status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(_status, 1);
    _reload = new QPushButton("Reload", _statusRow);
    _reload->setToolTip(server() ? "Asks the script server to load this script again, from its link"
                                 : "Loads this script again, from its link, for everyone in the world (a server script on the "
                                   "entity starts again too)");
    row->addWidget(_reload, 0, Qt::AlignTop);
    layout->addWidget(_statusRow);
    _statusRow->hide();
    connect(_reload, &QPushButton::clicked, this, &ScriptEditor::reload);

    QPointer<ScriptEditor> self(this);
    _target = std::make_shared<AnswerTarget>([self](int32_t callback, const QVariantList& arguments) {
        if (self) {
            self->answered(callback, arguments);
        }
    });
    _poll.setInterval(POLL_MS);
    connect(&_poll, &QTimer::timeout, this, &ScriptEditor::ask);
}

ScriptEditor::~ScriptEditor() = default;

void ScriptEditor::showInlineNote(const QString& text) {
    // a script written into the field, not an address, reaches visitors only if it fits one entity packet with the
    // rest of the entity: past that the world keeps it but every visitor gets no script and no error (servers, 11-01;
    // the true edge is a little over 1 KB and varies, so the note comes at 1 KB)
    static const int INLINE_SCRIPT_LIMIT_BYTES = 1024;
    const QString script = text.trimmed();
    const bool address = !script.contains('\n') && !QUrl(script).scheme().isEmpty();
    _inlineNote->setVisible(_spec.key == "script" && !address && script.toUtf8().size() > INLINE_SCRIPT_LIMIT_BYTES);
}

bool ScriptEditor::isFor(const PropertySpec& spec) {
    return spec.kind == PropertyKind::Url && (spec.key == "script" || spec.key == "serverScripts");
}

bool ScriptEditor::server() const {
    return _spec.key == "serverScripts";
}

void ScriptEditor::setValue(const QVariant& value, bool mixed) {
    _url->setValue(value, mixed);
    showInlineNote(mixed ? QString() : value.toString());
    const QString link = mixed ? QString("\n") : value.toString().trimmed();   // mixed: never equal to a real link
    const bool changed = link != _link || mixed != _mixed;
    if (link != _link) {
        _link = link;
        _hasLink = !link.isEmpty();
        ++_generation;
        _answers.clear();
        showStatus();
    }
    _statusRow->setVisible(_hasLink);
    _mixed = mixed;
    showEdit(changed);   // the Inspector sets values every 100 ms: New's check, at most once a second unless it changed
    updatePolling();
}

// one entity's script, on a build with the Code panel: Edit, or New while the field is empty, off with why it can't
// (a world opened from a file can't keep scripts, mary 11-08)
void ScriptEditor::showEdit(bool now) {
    const bool one = CodeEditor::isAvailable() && _ids.size() == 1 && !_mixed;
    _edit->setVisible(one);
    if (!one) {
        return;
    }
    if (!_link.isEmpty()) {
        _edit->setText("Edit");
        _edit->setEnabled(true);
        _edit->setToolTip(server() ? "Opens this server script in the Code panel" : "Opens this script in the Code panel");
        return;
    }
    static const qint64 NEW_CHECK_MS = 1000;
    const qint64 at = QDateTime::currentMSecsSinceEpoch();
    if (!now && _edit->text() == "New" && at - _newCheckedAt < NEW_CHECK_MS) {
        return;
    }
    _newCheckedAt = at;
    const QString why = CodeEditor::whyNoNewScript(_ids.first(), _spec.key);
    _edit->setText("New");
    _edit->setEnabled(why.isEmpty());
    // a thing's name is world text: always plain in the tooltip, never read as rich text
    _edit->setToolTip(!why.isEmpty() ? "<qt>" + why.toHtmlEscaped() + "</qt>"
        : server() ? "Starts a new server script for this thing in the Code panel"
                   : "Starts a new script for this thing in the Code panel");
}

void ScriptEditor::setEntities(const QList<QUuid>& ids) {
    if (ids == _ids) {
        return;
    }
    _ids = ids;
    ++_generation;
    _answers.clear();
    showStatus();
    showEdit();
    updatePolling();
}

void ScriptEditor::showEvent(QShowEvent* event) {
    PropertyEditor::showEvent(event);
    updatePolling();
}

void ScriptEditor::hideEvent(QHideEvent* event) {
    PropertyEditor::hideEvent(event);
    updatePolling();
}

// asked only while it shows: a hidden Inspector, a folded section or a row without a link costs nothing
void ScriptEditor::updatePolling() {
    const bool wanted = _hasLink && !_ids.isEmpty() && isVisible();
    if (wanted && !_poll.isActive()) {
        _poll.start();
        ask();
    } else if (!wanted && _poll.isActive()) {
        _poll.stop();
    }
}

void ScriptEditor::ask() {
    if (!_hasLink || _ids.isEmpty()) {
        return;
    }
    // the last questions still out (the server can be slow): wait for them, up to a point
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!_questions.isEmpty() && now - _askedAt < ANSWER_WAIT_MS) {
        return;
    }
    _questions.clear();
    _askedAt = now;
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    const QString property = server() ? "serverScripts" : "script";
    for (int i = 0; i < _ids.size() && i < MAX_ASKED; ++i) {
        ScriptCallback callback;
        callback.id = _nextCallback++;
        callback.target = _target;
        _questions.insert(callback.id, qMakePair(_generation, _ids[i]));
        entities->queryPropertyMetadata(_ids[i], property, callback);
    }
}

// (error, result): the result's isRunning, status and errorInfo; an error when this client's engine or the script
// server has nothing on the entity, its message in message
void ScriptEditor::answered(int32_t callback, const QVariantList& arguments) {
    auto question = _questions.find(callback);
    if (question == _questions.end()) {
        return;
    }
    const int generation = question->first;
    const QUuid id = question->second;
    _questions.erase(question);
    if (generation != _generation) {
        return;
    }
    const QVariantMap error = arguments.value(0).toMap();
    const QVariantMap result = arguments.value(1).toMap();
    Answer answer;
    answer.answered = true;
    if (!result.isEmpty()) {
        answer.running = result.value("isRunning").toBool();
        answer.status = result.value("status").toString();
        answer.error = result.value("errorInfo").toString().trimmed();
    } else {
        answer.error = error.value("message").toString().trimmed();
    }
    _answers[id] = answer;
    showStatus();
}

void ScriptEditor::showStatus() {
    auto word = [this](const Answer& answer) -> QString {
        if (answer.status == "running") {
            return "Running";
        }
        if (answer.status == "pending" || answer.status == "loading") {
            return "Starting…";
        }
        if (answer.status == "error_loading_script") {
            return "Couldn't load";
        }
        if (answer.status == "error_running_script") {
            return "Stopped with an error";
        }
        if (answer.status == "unloaded") {
            return "Not running";
        }
        return server() ? "No answer from the script server" : "Not running here";
    };

    const int asked = std::min((int)_ids.size(), MAX_ASKED);
    QString text;
    QString tip;
    QString state;
    if (_answers.isEmpty()) {
        text = "Asking…";
    } else if (asked == 1) {
        const Answer& answer = _answers.first();
        text = word(answer);
        if (!answer.running && !answer.error.isEmpty()) {
            text += ": " + shortError(answer.error, _link);
            tip = answer.error;
        }
        state = answer.status == "running" ? "running" : (isError(answer.status) || answer.status.isEmpty()) ? "error" : "";
    } else {
        int running = 0;
        const Answer* trouble = nullptr;
        for (const auto& answer : _answers) {
            if (answer.status == "running") {
                ++running;
            } else if (!trouble || (isError(answer.status) && !isError(trouble->status))) {
                trouble = &answer;
            }
        }
        text = QString("Running in %1 of %2").arg(running).arg(asked);
        if (trouble) {
            text += "; " + word(*trouble) + (trouble->error.isEmpty() ? QString() : ": " + shortError(trouble->error, _link));
            tip = trouble->error;
        }
        state = running == asked ? "running" : trouble && (isError(trouble->status) || trouble->status.isEmpty()) ? "error" : "";
        if (_ids.size() > MAX_ASKED) {
            text += QString(" (the first %1 selected)").arg(MAX_ASKED);
        }
    }
    if (text.size() > MAX_ERROR_SHOWN) {
        text = text.left(MAX_ERROR_SHOWN) + "…";
    }
    if (text != _status->text()) {
        qCDebug(interfaceapp) << "Inspector:" << _spec.label << "status:" << text;
    }
    _status->setText(text);
    _status->setToolTip(tip);
    if (_status->property("state").toString() != state) {
        _status->setProperty("state", state);
        _status->style()->unpolish(_status);
        _status->style()->polish(_status);
    }
}

QString ScriptEditor::statusText() const {
    return _statusRow->isVisible() ? _status->text() : QString();
}

QVariantMap ScriptEditor::editButton() const {
    return { { "shown", _edit->isVisibleTo(const_cast<ScriptEditor*>(this)) }, { "text", _edit->text() },
             { "enabled", _edit->isEnabled() }, { "tip", _edit->toolTip() } };
}

bool ScriptEditor::pressEdit() {
    if (!_edit->isVisible() || !_edit->isEnabled()) {
        return false;
    }
    _edit->click();
    return true;
}

bool ScriptEditor::pressReload() {
    if (!_reload->isVisible() || !_reload->isEnabled()) {
        return false;
    }
    _reload->click();
    return true;
}

// Script: everyone's client loads it again when the entity's scriptTimestamp changes (as the old Create app's Reload
// did), and the entity tree has the script server reload the entity's server script on that change too; Server
// Script: the script server alone is asked to. Neither is an undo step: nothing about the world changes.
void ScriptEditor::reload() {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    for (const auto& id : _ids) {
        if (server()) {
            entities->reloadServerScripts(id);
        } else {
            EntityItemProperties properties;
            properties.setScriptTimestamp((quint64)QDateTime::currentMSecsSinceEpoch());
            entities->editEntity(id, properties);
        }
    }
    ++_generation;
    _answers.clear();
    _questions.clear();
    _status->setText("Reloading…");
    QTimer::singleShot(1000, this, &ScriptEditor::ask);
}
