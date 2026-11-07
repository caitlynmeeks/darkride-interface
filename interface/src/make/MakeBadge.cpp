//
//  MakeBadge.cpp
//  interface/src/make
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "MakeBadge.h"

#include <DependencyManager.h>

#include "MakeDock.h"
#include "ui/Panels.h"

namespace {
    const int DONE_MS = 2000;   // mary: a tick for about 2 s (no fade: a hide is one scene change, performance)
}

MakeBadge::MakeBadge(MakeDock* dock) : QObject(dock), _dock(dock) {
    _doneTimer.setSingleShot(true);
    _doneTimer.setInterval(DONE_MS);
    connect(&_doneTimer, &QTimer::timeout, this, [this] {
        _done = false;
        update();
    });
    connect(dock, &MakeDock::workingChanged, this, [this] {
        if (_dock->isWorking()) {
            _done = false;    // a new request: the last one's tick or red dot goes
            _failed = false;
            _doneTimer.stop();
        }
        update();
    });
    connect(dock, &MakeDock::stateChanged, this, &MakeBadge::update);
    connect(dock, &MakeDock::questionsChanged, this, &MakeBadge::update);
    connect(dock, &MakeDock::requestEnded, this, &MakeBadge::requestEnded);
}

void MakeBadge::open() {
    _failed = false;
    update();
    if (auto panels = DependencyManager::get<Panels>()) {
        panels->open("Make");
    }
    emit _dock->showNow();
}

void MakeBadge::setPanelInSight(bool inSight) {
    if (inSight == _panelInSight) {
        return;
    }
    _panelInSight = inSight;
    if (inSight) {
        _failed = false;   // the panel shows the reason itself
        _done = false;
        _doneTimer.stop();
    }
    update();
}

void MakeBadge::testForce(const QString& state) {
    _forced = state;
    update();
}

void MakeBadge::requestEnded(bool finished) {
    _done = finished;
    _failed = !finished;
    if (finished) {
        _doneTimer.start();
    } else {
        _doneTimer.stop();
    }
    update();
}

void MakeBadge::update() {
    QString state;
    if (!_forced.isEmpty()) {
        state = _forced == "off" ? QString() : _forced;
    } else if (_panelInSight) {
        state = QString();   // the panel shows it all
    } else if (_dock->hasQuestion()) {
        state = "answer";
    } else if (_dock->isWorking()) {
        state = "working";
    } else if (_failed) {
        state = "failed";    // even after the link dropped: a failure is never silent
    } else if (_done) {
        state = "done";
    }
    if (state != _state) {
        _state = state;
        emit stateChanged();
    }
}
