//
//  MakeKey.cpp
//  libraries/make-link/src
//
//  The key's shape check, the same on every system; keeping it is each system's own (MakeKeyMac.cpp, and a stand-in
//  below where there is none yet: the Terminal route only).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeKey.h"

#include <QtCore/QRegularExpression>

bool MakeKey::looksRight(const QString& key, QString& why) {
    if (key.startsWith("sk-ant-oat")) {
        why = "That's a sign-in token from a Claude plan, which an app may not use for you. Use Make from Terminal "
              "with your plan instead, or make an API key at console.anthropic.com.";
        return false;
    }
    static const QRegularExpression SHAPE("^sk-ant-api[0-9A-Za-z_-]{30,300}$");
    if (!SHAPE.match(key).hasMatch()) {
        why = "That doesn't look like an Anthropic API key (it starts sk-ant-api, with no spaces).";
        return false;
    }
    return true;
}

#if !defined(Q_OS_MAC)

// no key store here yet (Windows' and Linux's come next): the Terminal route only, never a plaintext file
bool MakeKey::store(const QString& key, QString& why) {
    if (!looksRight(key, why)) {
        return false;
    }
    why = "Make can't keep a key on this system yet: use Make from Terminal with your own Claude Code.";
    return false;
}

bool MakeKey::isStored() {
    return false;
}

bool MakeKey::forget() {
    return true;
}

QByteArray MakeKey::readForChild() {
    return QByteArray();
}

#endif
