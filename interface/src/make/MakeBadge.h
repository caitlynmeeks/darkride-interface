//
//  MakeBadge.h
//  interface/src/make
//
//  The Make badge (Caity's idea, 11-07; mary's drawing team/mary/brand/make-panel/MAKE-BADGE.png): a small chip in the
//  bottom-right corner of the 3D view saying what Make is doing while its panel is out of sight. It is the only Make
//  object on the desktop's QML context, and it holds nothing but its state and one call: open() opens the Make panel,
//  which any script can already do (Panels.open). MakeDock itself stays on the Make dock's own context.
//
//  States: "" (hidden), "working", "answer" (a question waits for the person), "done" (about 2 s after a request
//  finished), "failed" (a request couldn't finish; until the badge is clicked or the panel is seen).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once
#ifndef hifi_MakeBadge_h
#define hifi_MakeBadge_h

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

class MakeDock;

class MakeBadge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit MakeBadge(MakeDock* dock);

    QString state() const { return _state; }

    /// The Make panel, on Now; a "failed" badge is cleared (seen).
    Q_INVOKABLE void open();

    /// Whether the Make panel is on screen (its dock's visibilityChanged: shown, and not a tab behind another).
    void setPanelInSight(bool inSight);
    /// A test's state (Test.makeBadge): "working", "answer", "done", "failed" or "off" (hidden), shown without a
    /// connected Make and whatever the panel; "" gives the badge back to Make.
    void testForce(const QString& state);

signals:
    void stateChanged();

private:
    void requestEnded(bool finished);
    void update();

    MakeDock* _dock;
    bool _panelInSight { false };
    bool _done { false };     // the last request finished: shown about 2 s
    bool _failed { false };   // the last request couldn't finish: shown until clicked or the panel is seen
    QTimer _doneTimer;
    QString _forced;
    QString _state;
};

#endif // hifi_MakeBadge_h
