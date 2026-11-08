//
//  MakeRequests.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeRequests.h"

#include <memory>

#include <QtCore/QElapsedTimer>

const qint64 MakeRequests::PAUSE_MS = 45 * 1000;
const qint64 MakeRequests::OPEN_SEND_MS = 10 * 60 * 1000;

MakeRequests::MakeRequests(std::function<qint64()> now) : _now(std::move(now)) {
    if (!_now) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        _now = [clock] { return clock->elapsed(); };
    }
}

void MakeRequests::started(int request, bool bySend) {
    _current = request;
    if (bySend) {
        // the Send's own calls are its own however long Claude thinks first: the pause counts from the Send
        _openSend = request;
        _last = _now();
    }
}

void MakeRequests::answered(int request) {
    // a late answer naming a Send that is no longer the current request (closed by the long silence, or followed by
    // another Send) changes nothing about the current one
    if (request != 0 && request == _openSend && request == _current) {
        _openSend = 0;
        _last = _now();   // a late call shortly after the answer still counts for this Send
    }
}

void MakeRequests::touched() {
    _last = _now();
}

bool MakeRequests::newRequestDue() const {
    if (_last < 0) {
        return false;   // no call yet: the first one belongs to the request that's there
    }
    return _now() - _last > (sendOpen() ? OPEN_SEND_MS : PAUSE_MS);
}
