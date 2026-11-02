//
//  AudioRefusals.h
//  libraries/audio/src
//
//  Created by social on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_AudioRefusals_h
#define hifi_AudioRefusals_h

#include <atomic>

#include <QtCore/QDebug>
#include <QtCore/QString>

#include <SharedUtil.h>

#include "AudioLogging.h"

// Counts what was refused from senders, of one kind, and logs it at most once every 10 s with how many there were
// since the last line (the first one at once), so a server's log can be counted without flooding. Safe to count from
// the mixer's workers at once.
class AudioRefusals {
public:
    AudioRefusals(const char* what) : _what(what) {}

    void count(const QString& lastFrom = QString()) {
        ++_count;
        const quint64 LOG_INTERVAL_USECS = 10 * USECS_PER_SECOND;
        quint64 now = usecTimestampNow();
        quint64 last = _lastLog.load();
        if ((last == 0 || now - last >= LOG_INTERVAL_USECS) && _lastLog.compare_exchange_strong(last, now)) {
            int refused = _count.exchange(0);
            if (lastFrom.isEmpty()) {
                qCDebug(audio).noquote() << "Refused" << refused << _what;
            } else {
                qCDebug(audio).noquote() << "Refused" << refused << _what << "(the last from" << lastFrom + ")";
            }
        }
    }

private:
    const char* _what;
    std::atomic<int> _count { 0 };
    std::atomic<quint64> _lastLog { 0 };
};

#endif // hifi_AudioRefusals_h
