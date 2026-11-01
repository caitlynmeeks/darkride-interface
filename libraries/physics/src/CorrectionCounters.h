//
//  CorrectionCounters.h
//  libraries/physics/src
//
//  The server-state corrections of track cars and vehicles, counted (team/performance/INSTRUMENTS-PLAN.md, item 6):
//  snaps and blends since the start and the largest residual of the last five seconds, kept always for the Stats
//  line (a few numbers); and, when the frame trace is on, each correction's residual, closing rate and the counts
//  as the frame's net.* columns. One of these in the track simulation and one in the vehicle simulation.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the whole file.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#ifndef hifi_CorrectionCounters_h
#define hifi_CorrectionCounters_h

#include <algorithm>
#include <cstdint>

#include <FrameTrace.h>
#include <NumericalConstants.h>
#include <SharedUtil.h>

class CorrectionCounters {
public:
    // a correction adopted: the residual between the shown pose and where the state puts it (m), the rate it will
    // close at (m/s; 0 for a snap), and whether it is taken at once (a snap) or closed over time (a blend)
    void note(float residualM, float closingRate, bool snapped) {
        uint64_t now = usecTimestampNow();
        if (now - _windowStart >= WINDOW_USECS) {
            _lastWindowMax = _windowMax;
            _windowMax = 0.0f;
            _windowStart = now;
        }
        _windowMax = std::max(_windowMax, residualM);
        if (snapped) {
            _snaps++;
        } else {
            _blends++;
        }
        if (FrameTrace::enabled()) {
            FrameTrace::set("net.residual_cm", residualM * 100.0f);
            FrameTrace::set("net.closing", closingRate);
            FrameTrace::set("net.snaps", (double)_snaps);
            FrameTrace::set("net.blends", (double)_blends);
        }
    }
    uint32_t getSnaps() const { return _snaps; }
    uint32_t getBlends() const { return _blends; }
    // the largest residual of the last five seconds or so (this window's and the last whole one's), m
    float getResidualMaxRecent() const { return std::max(_lastWindowMax, _windowMax); }

private:
    static constexpr uint64_t WINDOW_USECS = 5 * USECS_PER_SECOND;
    uint32_t _snaps { 0 };
    uint32_t _blends { 0 };
    float _windowMax { 0.0f };
    float _lastWindowMax { 0.0f };
    uint64_t _windowStart { 0 };
};

#endif // hifi_CorrectionCounters_h
