//
//  FrameTrace.cpp
//  libraries/shared/src
//
//  See FrameTrace.h.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FrameTrace.h"

#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <QtCore/QFile>
#include <QtCore/QTextStream>

#include "SharedUtil.h"

namespace FrameTrace {

namespace detail {
    std::atomic<bool> on { false };
}

namespace {

const int DEFAULT_FRAMES = 2048;
const double UNSET = std::numeric_limits<double>::quiet_NaN();
const uint64_t SUMMARY_INTERVAL_USECS = 250000;
const uint64_t SUMMARY_WINDOW_USECS = 1000000;

struct Column {
    std::string name;
    bool isText { false };
    int slot { 0 };   // into Record::values or Record::texts
};

struct Record {
    uint64_t frame { 0 };
    uint64_t time { 0 };
    std::vector<double> values;
    std::vector<QString> texts;
};

struct State {
    std::mutex mutex;
    std::atomic<uint64_t> current { 0 };   // the frame beginFrame last opened; readable without the mutex
    uint64_t frameCounter { 0 };
    std::vector<Record> ring;
    size_t capacity { 0 };
    size_t count { 0 };   // records held, the newest at (next - 1)
    size_t next { 0 };    // where the next record goes
    std::vector<Column> columns;
    std::unordered_map<std::string, int> columnIndex;
    int numberSlots { 0 };
    int textSlots { 0 };
    uint64_t summaryTime { 0 };
    QStringList summary;
};

State& state() {
    static State instance;
    return instance;
}

// the record for a frame, or null when it is not in the ring (the frames are contiguous: the newest is current)
Record* recordFor(State& s, uint64_t frame) {
    uint64_t newest = s.current.load(std::memory_order_relaxed);
    if (s.count == 0 || frame == 0 || frame > newest || newest - frame >= s.count) {
        return nullptr;
    }
    size_t back = (size_t)(newest - frame);
    size_t position = (s.next + s.capacity - 1 - back) % s.capacity;
    return &s.ring[position];
}

const Column& columnFor(State& s, const char* name, bool isText) {
    auto found = s.columnIndex.find(name);
    if (found != s.columnIndex.end()) {
        return s.columns[found->second];
    }
    Column column;
    column.name = name;
    column.isText = isText;
    column.slot = isText ? s.textSlots++ : s.numberSlots++;
    s.columnIndex[column.name] = (int)s.columns.size();
    s.columns.push_back(column);
    return s.columns.back();
}

void setNumber(uint64_t frame, const char* name, double value) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Record* record = recordFor(s, frame);
    if (!record) {
        return;
    }
    const Column& column = columnFor(s, name, false);
    if (column.isText) {
        return;   // a text column written as a number: dropped rather than mixed
    }
    if ((int)record->values.size() <= column.slot) {
        record->values.resize(column.slot + 1, UNSET);
    }
    record->values[column.slot] = value;
}

void setString(uint64_t frame, const char* name, const QString& text) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Record* record = recordFor(s, frame);
    if (!record) {
        return;
    }
    const Column& column = columnFor(s, name, true);
    if (!column.isText) {
        return;
    }
    if ((int)record->texts.size() <= column.slot) {
        record->texts.resize(column.slot + 1);
    }
    record->texts[column.slot] = text;
}

QVariant cell(const Record& record, const Column& column) {
    if (column.isText) {
        if (column.slot < (int)record.texts.size() && !record.texts[column.slot].isEmpty()) {
            return record.texts[column.slot];
        }
    } else if (column.slot < (int)record.values.size() && !std::isnan(record.values[column.slot])) {
        return record.values[column.slot];
    }
    return QVariant();
}

QString number(double value) {
    return QString::number(value, 'g', 10);
}

QString csvText(const QString& text) {
    if (!text.contains(',') && !text.contains('"') && !text.contains('\n')) {
        return text;
    }
    QString quoted = text;
    quoted.replace("\"", "\"\"");
    return "\"" + quoted + "\"";
}

// the records oldest first, under the lock
template <typename Visit>
void eachRecord(State& s, Visit visit) {
    for (size_t i = 0; i < s.count; i++) {
        visit(s.ring[(s.next + s.capacity - s.count + i) % s.capacity]);
    }
}

} // namespace

void start(int frames) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.capacity = frames > 0 ? (size_t)frames : (size_t)DEFAULT_FRAMES;
    s.ring.assign(s.capacity, Record());
    s.count = 0;
    s.next = 0;
    s.summary.clear();
    s.summaryTime = 0;
    detail::on.store(true, std::memory_order_release);
}

void stop() {
    detail::on.store(false, std::memory_order_release);
}

void clear() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.count = 0;
    s.next = 0;
    s.summary.clear();
    s.summaryTime = 0;
}

int frames() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return (int)s.count;
}

uint64_t beginFrame() {
    if (!enabled()) {
        return 0;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.capacity == 0) {
        return 0;
    }
    Record& record = s.ring[s.next];
    record.frame = ++s.frameCounter;
    record.time = usecTimestampNow();
    record.values.assign(s.numberSlots, UNSET);
    record.texts.assign(s.textSlots, QString());
    s.next = (s.next + 1) % s.capacity;
    if (s.count < s.capacity) {
        s.count++;
    }
    s.current.store(record.frame, std::memory_order_relaxed);
    return record.frame;
}

uint64_t currentFrame() {
    return enabled() ? state().current.load(std::memory_order_relaxed) : 0;
}

void set(const char* column, double value) {
    if (enabled()) {
        setNumber(state().current.load(std::memory_order_relaxed), column, value);
    }
}

void set(uint64_t frame, const char* column, double value) {
    if (enabled()) {
        setNumber(frame, column, value);
    }
}

void setText(const char* column, const QString& text) {
    if (enabled()) {
        setString(state().current.load(std::memory_order_relaxed), column, text);
    }
}

void setText(uint64_t frame, const char* column, const QString& text) {
    if (enabled()) {
        setString(frame, column, text);
    }
}

QStringList columns() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    QStringList names { "frame", "t_us" };
    for (const Column& column : s.columns) {
        names << QString::fromStdString(column.name);
    }
    return names;
}

namespace {

// the rows, under the caller's lock
QList<QVariantList> rowsLocked(State& s) {
    QList<QVariantList> out;
    out.reserve((int)s.count);
    eachRecord(s, [&](const Record& record) {
        QVariantList row;
        row.reserve(2 + (int)s.columns.size());
        row << (double)record.frame << (double)record.time;
        for (const Column& column : s.columns) {
            row << cell(record, column);
        }
        out << row;
    });
    return out;
}

} // namespace

QList<QVariantList> rows() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return rowsLocked(s);
}

QList<QVariantList> take() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    QList<QVariantList> out = rowsLocked(s);
    s.count = 0;
    s.next = 0;
    s.summary.clear();
    s.summaryTime = 0;
    return out;
}

int dump(const QString& path) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return -1;
    }
    QTextStream out(&file);
    out << "frame,t_us";
    for (const Column& column : s.columns) {
        out << ',' << QString::fromStdString(column.name);
    }
    out << '\n';
    int written = 0;
    eachRecord(s, [&](const Record& record) {
        out << record.frame << ',' << record.time;
        for (const Column& column : s.columns) {
            out << ',';
            QVariant value = cell(record, column);
            if (value.type() == QVariant::Double) {
                out << number(value.toDouble());
            } else if (value.isValid()) {
                out << csvText(value.toString());
            }
        }
        out << '\n';
        written++;
    });
    out.flush();
    return file.error() == QFile::NoError ? written : -1;
}

QStringList summaryLines() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.count == 0) {
        return QStringList();
    }
    uint64_t now = usecTimestampNow();
    if (now - s.summaryTime < SUMMARY_INTERVAL_USECS) {
        return s.summary;
    }
    s.summaryTime = now;
    // the newest finished frame: the one before the current while the trace runs (the current is still being written)
    uint64_t newest = s.current.load(std::memory_order_relaxed);
    uint64_t finished = (enabled() && s.count >= 2) ? newest - 1 : newest;
    const Record* last = recordFor(s, finished);
    std::vector<double> maxima(s.numberSlots, UNSET);
    eachRecord(s, [&](const Record& record) {
        if (record.frame == newest && enabled()) {
            return;
        }
        if (record.time + SUMMARY_WINDOW_USECS < now) {
            return;
        }
        for (size_t slot = 0; slot < record.values.size() && slot < maxima.size(); slot++) {
            double value = record.values[slot];
            if (!std::isnan(value) && (std::isnan(maxima[slot]) || value > maxima[slot])) {
                maxima[slot] = value;
            }
        }
    });
    QStringList lines;
    lines << QString("Frame trace: %1, %2 frames").arg(enabled() ? "on" : "off").arg((qulonglong)s.count);
    for (const Column& column : s.columns) {
        QString line = QString::fromStdString(column.name) + ": ";
        QVariant value = last ? cell(*last, column) : QVariant();
        if (column.isText) {
            line += value.isValid() ? value.toString() : QString("-");
        } else {
            line += value.isValid() ? QString::number(value.toDouble(), 'g', 6) : QString("-");
            double maximum = column.slot < (int)maxima.size() ? maxima[column.slot] : UNSET;
            if (!std::isnan(maximum)) {
                line += QString(" (max 1 s %1)").arg(QString::number(maximum, 'g', 6));
            }
        }
        lines << line;
    }
    s.summary = lines;
    return lines;
}

} // namespace FrameTrace
