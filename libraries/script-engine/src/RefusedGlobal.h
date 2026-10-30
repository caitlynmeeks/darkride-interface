//
//  RefusedGlobal.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The stand-in a world's entity engine gets in place of a global the script API policy removes (ScriptApiPolicy):
//  an object with no members but its name, so an old script fails at the one call ("Snapshot.takeSnapshot is not a
//  function") instead of at load with a ReferenceError, and the WebAssembly guest can name the refusal.
//

#ifndef hifi_RefusedGlobal_h
#define hifi_RefusedGlobal_h

#include <QtCore/QObject>
#include <QtCore/QString>

class RefusedGlobal : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString refusedName READ refusedName CONSTANT)
public:
    RefusedGlobal(const QString& name, QObject* parent);
    QString refusedName() const { return _name; }

private:
    QString _name;
};

#endif // hifi_RefusedGlobal_h
