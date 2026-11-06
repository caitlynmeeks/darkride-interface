//
//  ScriptEditor.h
//  interface/src/editor/ui
//
//  The Inspector's Script and Server Script rows (the old properties panel had both, 10-25): the script's link as any
//  link field, and under it what the script is doing now, from this client's entity script engine for Script and
//  from the entity script server for Server Script ("Running", "Starting…", "Couldn't load: …", "Stopped with an
//  error: …"), with Reload: Script's reloads it for everyone in the world (the entity's scriptTimestamp, as the old
//  Create app did; the entity's server script starts again too), Server Script's asks the server to load it again. The status is asked for every 2 s, only while
//  the row is on screen with a link in it. Edit beside the link opens one entity's script in the Code panel (Make v1, Qt 6).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ScriptEditor_h
#define hifi_ScriptEditor_h

#include <memory>

#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include "PropertyEditors.h"

class CallbackTarget;
class QLabel;
class QPushButton;

class ScriptEditor : public PropertyEditor {
    Q_OBJECT
public:
    ScriptEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    ~ScriptEditor() override;

    // whether a row of this property is one (Script, Server Script)
    static bool isFor(const PropertySpec& spec);

    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override { return _url->isEditing(); }
    void revertTyping() override { _url->revertTyping(); }
    QVariant currentValue() const override { return _url->currentValue(); }
    bool isMixed() const override { return _url->isMixed(); }

    // the selected entities, whose scripts the status line is about
    void setEntities(const QList<QUuid>& ids);

    // the status line as shown, and Reload pressed as a click would (false when it isn't there to press): for a
    // scripted test
    QString statusText() const;
    bool pressReload();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Answer {
        bool answered { false };
        bool running { false };
        QString status;     // pending, loading, error_loading_script, error_running_script, running, unloaded; "" for none
        QString error;
    };

    bool server() const;
    void ask();
    void answered(int32_t callback, const QVariantList& arguments);
    void showStatus();
    void showInlineNote(const QString& text);   // a written-in script too long to reach visitors
    void reload();
    void updatePolling();
    void showEdit();

    TextEditor* _url { nullptr };
    QWidget* _statusRow { nullptr };
    QLabel* _status { nullptr };
    QLabel* _inlineNote { nullptr };
    QPushButton* _reload { nullptr };
    QPushButton* _edit { nullptr };   // the script in the Code panel
    bool _mixed { false };
    QTimer _poll;
    QList<QUuid> _ids;
    QMap<QUuid, Answer> _answers;
    int _generation { 0 };         // a new selection or link: answers to earlier questions are dropped
    QMap<int32_t, QPair<int, QUuid>> _questions;   // by callback id: the generation and the entity asked about
    int32_t _nextCallback { 1 };
    qint64 _askedAt { 0 };
    QString _link;
    bool _hasLink { false };
    std::shared_ptr<CallbackTarget> _target;
};

#endif  // hifi_ScriptEditor_h
