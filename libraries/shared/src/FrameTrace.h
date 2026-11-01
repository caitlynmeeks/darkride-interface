//
//  FrameTrace.h
//  libraries/shared/src
//
//  One frame trace for the whole engine (team/performance/INSTRUMENTS-PLAN.md item 1, 2026-10-31): a ring buffer of
//  the last frames, each a record of named numbers (and the odd name or id) that any subsystem writes while the trace
//  is on: the frame's own times here, the physics step, the camera boom, the avatar's carry in a vehicle, the entity
//  render-update budget, the presents. One record per game-loop frame, opened at the top of Application::idle; a
//  writer on another thread (render, present) names the frame it writes to, with the index the game loop handed it.
//  Off, the whole cost is one relaxed atomic load per writer and nothing is allocated; on, a set() is one mutex and
//  one hash lookup. Test.frameTrace / dumpFrameTrace / takeFrameTrace and Developer > Recording Mode switch it; the
//  dump is a CSV with a column per name, blank where a frame has no value. Columns are named <area>.<name>_<unit>,
//  lowercase (frame.dt_ms, physics.substeps, boom.allowed_m); a column set twice in a frame keeps the last value.
//  Every call is thread-safe by construction (one mutex while on), so no caller needs a hop to another thread.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the whole file: the frame trace's API (start, stop, beginFrame, set, setText, columns, take, dump,
// summaryLines) and the column names every department writes (<area>.<name>_<unit>). Every writer calls it on the
// vehicle's frame, and the vehicle check reads the ride from it, so a change here changes the ride and its proof.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//
#pragma once

#include <atomic>
#include <cstdint>

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

namespace FrameTrace {
    namespace detail {
        extern std::atomic<bool> on;
    }

    // whether records are being written: the one check a writer makes before computing anything only the trace needs
    inline bool enabled() { return detail::on.load(std::memory_order_relaxed); }

    // a ring of the last `frames` frame records (0: 2048, about 14 s at 144 Hz); drops the records held so far, keeps
    // the column names, and turns the trace on
    void start(int frames = 0);
    // stops writing; the records stay for rows() and dump()
    void stop();
    // drops the records (the trace stays on or off as it was); the column names stay, so a CSV's columns hold still
    void clear();
    // how many frame records are held
    int frames();

    // the game loop only, at the top of its frame: opens this frame's record and returns its index (1, 2, ...);
    // 0 when the trace is off, which every other call treats as "nothing to write"
    uint64_t beginFrame();
    // the index beginFrame last returned, for the main thread to hand to the render or present thread with the frame
    uint64_t currentFrame();

    // a named number in the current frame's record (any thread) or in the frame named (dropped if it left the ring)
    void set(const char* column, double value);
    void set(uint64_t frame, const char* column, double value);
    // a name or an id (an entity id goes in as its string)
    void setText(const char* column, const QString& text);
    void setText(uint64_t frame, const char* column, const QString& text);

    // "frame", "t_us", then every column in the order first written
    QStringList columns();
    // the records, oldest first: each a list in columns() order; a cell is a double, a string, or invalid when unset
    QList<QVariantList> rows();
    // rows() and clear() in one step, so no frame written between the two is lost
    QList<QVariantList> take();
    // the records as a CSV (the columns() header, then one line per frame, blank cells where unset, text quoted);
    // the rows written, or -1 when the file could not be written
    int dump(const QString& path);

    // for the Stats panel: one line per column, "name: last value" (the newest finished frame's) with each number's
    // maximum over the last second; refreshed at most four times a second, empty when the trace is off and holds
    // nothing
    QStringList summaryLines();
}
