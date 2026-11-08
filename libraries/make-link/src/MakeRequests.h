//
//  MakeRequests.h
//  libraries/make-link/src
//
//  When a tool call of Claude's belongs to a new request (the unit the dock's Undo takes back). On the Terminal route
//  Make learns of a new request only from a pause between calls (PAUSE_MS). A Send from the dock starts a request of
//  its own that stays open for Claude's calls until that Send is answered (the reply naming it, or the key route's
//  turn ending); while it is open only a much longer silence (OPEN_SEND_MS) starts a new one. After the answer the pause
//  counts from the answer. The clock is given, so a test can drive the minutes in microseconds.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MakeRequests_h
#define hifi_MakeRequests_h

#include <functional>

#include <QtCore/QtGlobal>

class MakeRequests {
public:
    static const qint64 PAUSE_MS;       // 45 s between calls: the person asked again in Terminal
    static const qint64 OPEN_SEND_MS;   // 10 min of silence while a Send is unanswered: Claude went quiet

    /// `now` gives milliseconds on any steady clock; empty: the app's steady clock
    explicit MakeRequests(std::function<qint64()> now = nullptr);

    /// A new request started: `request` is its number; `bySend` when a Send of the dock's started it (it stays open)
    void started(int request, bool bySend);
    /// A Send's answer came, naming `request`: only the open Send of the current request counts
    void answered(int request);
    /// A call started or ended: the pause counts from here
    void touched();
    /// At a call's start: has the pause passed, so the call belongs to a new request?
    bool newRequestDue() const;

    int current() const { return _current; }
    bool sendOpen() const { return _openSend != 0 && _openSend == _current; }

private:
    std::function<qint64()> _now;
    int _current { 0 };
    int _openSend { 0 };
    qint64 _last { -1 };   // when a call last started or ended, or the open Send started or was answered; -1 never
};

#endif  // hifi_MakeRequests_h
