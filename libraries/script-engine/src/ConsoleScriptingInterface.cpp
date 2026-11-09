//
//  ConsoleScriptingInterface.cpp
//  libraries/script-engine/src
//
//  Created by NeetBhagat on 6/1/17.
//  Copyright 2014 High Fidelity, Inc.
//
//  ConsoleScriptingInterface is responsible for following functionality
//  Printing logs with various tags and grouping on debug Window and Logs/log file.
//  Debugging functionalities like Timer start-end, assertion, trace.
//  To use these functionalities, use "console" object in Javascript files.
//  For examples please refer "scripts/developer/tests/consoleObjectTest.js"
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ConsoleScriptingInterface.h"

#include <QtCore/QDateTime>

#include "ScriptEngine.h"

const QString STACK_TRACE_FORMAT = "\n[Stacktrace]%1%2";

void ConsoleScriptingInterface::time(QString labelName) {
    _timerDetails.insert(labelName, QDateTime::currentDateTime().toUTC());
    QString message = QString("%1: Timer started").arg(labelName);
    if (ScriptEngine* scriptEngine = qobject_cast<ScriptEngine*>(parent())) {
        scriptEngine->scriptPrintedMessage(message);
    }
}

void ConsoleScriptingInterface::timeEnd(QString labelName) {
    if (ScriptEngine* scriptEngine = qobject_cast<ScriptEngine*>(parent())) {
        if (!_timerDetails.contains(labelName)) {
            scriptEngine->scriptErrorMessage("No such label found " + labelName);
            return;
        }

        if (_timerDetails.value(labelName).isNull()) {
            _timerDetails.remove(labelName);
            scriptEngine->scriptErrorMessage("Invalid start time for " + labelName);
            return;
        }
        QDateTime _startTime = _timerDetails.value(labelName);
        QDateTime _endTime = QDateTime::currentDateTime().toUTC();
        qint64 diffInMS = _startTime.msecsTo(_endTime);

        QString message = QString("%1: %2ms").arg(labelName).arg(QString::number(diffInMS));
        _timerDetails.remove(labelName);

        scriptEngine->scriptPrintedMessage(message);
    }
}

void ConsoleScriptingInterface::trace() {
    // a module has no call stack the engine can show; a JavaScript script's console.trace is the guest's own
    if (ScriptEngine* scriptEngine = qobject_cast<ScriptEngine*>(parent())) {
        scriptEngine->scriptPrintedMessage(QString(STACK_TRACE_FORMAT).arg(" ", "(not available)"));
    }
}

void ConsoleScriptingInterface::clear() {
    if (ScriptEngine* scriptEngine = qobject_cast<ScriptEngine*>(parent())) {
        scriptEngine->clearDebugLogWindow();
    }
}
