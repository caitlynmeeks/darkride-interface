//
//  Golden.h
//  tests/script-values/src
//
//  Recorded expectations for the plain script values and the entity-property conversions: the files under
//  tests/script-values/golden/ hold what the conversions produce, recorded on a build where they were proven
//  equal to the QtScript engine's (the comparison these tests made on every build until QtScript was removed).
//  A test compares what the code produces now with the recording; SCRIPT_VALUES_RECORD=1 rewrites the recordings
//  (do that only for a change of the conversions that is meant, and read the diff).
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_script_values_Golden_h
#define hifi_script_values_Golden_h

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QString>

inline QString goldenPath(const QString& name) {
    return QFileInfo(__FILE__).dir().filePath("../golden/" + name);
}

inline bool recordingGolden() {
    return qEnvironmentVariableIsSet("SCRIPT_VALUES_RECORD");
}

/// True when `text` is what the recording `name` holds (or when it was just recorded); `where` says what differs,
/// and the text produced is left beside the recording as <name>.actual for a look.
inline bool checkGolden(const QString& name, const QString& text, QString& where) {
    const QString path = goldenPath(name);
    if (recordingGolden()) {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            where = "cannot record " + path;
            return false;
        }
        file.write(text.toUtf8());
        where = "recorded " + path;
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        where = "no recording at " + path + " (SCRIPT_VALUES_RECORD=1 records)";
        return false;
    }
    const QString expected = QString::fromUtf8(file.readAll());
    if (expected == text) {
        return true;
    }
    int at = 0;
    while (at < text.size() && at < expected.size() && text.at(at) == expected.at(at)) {
        ++at;
    }
    QFile actual(path + ".actual");
    if (actual.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        actual.write(text.toUtf8());
    }
    where = QString("%1 differs from the recording at %2: ...%3... vs recorded ...%4... (the text produced is in %5.actual)")
        .arg(name).arg(at).arg(text.mid(qMax(0, at - 40), 80)).arg(expected.mid(qMax(0, at - 40), 80)).arg(path);
    return false;
}

#endif // hifi_script_values_Golden_h
