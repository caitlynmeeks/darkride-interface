//
//  ConnectionMonitor.h
//  interface/src
//
//  Created by Ryan Huffman on 8/4/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ConnectionMonitor_h
#define hifi_ConnectionMonitor_h

#include <QObject>
#include <QTimer>

#include <QString>

class QUrl;

class ConnectionMonitor : public QObject {
    Q_OBJECT
public:
    // the world this start-up goes to came from Settings > General > Start in ("last": the last world, "home"): if it never
    // answers, the viewer goes to the entry world instead of its connection failure window. Before init()
    void setStartUpFallback(const QString& from) { _fallbackFrom = from; }
    void init();

signals:
    void setRedirectErrorState(QUrl errorURL, QString reasonMessage = "", int reasonCode = -1, const QString& extraInfo = "");

private slots:
    void startTimer();
    void stopTimer();

private:
    void startUpWorldAsked(const QString& world);

    QTimer _timer;
    QString _fallbackFrom;      // "last" or "home" while the start-up world may still fall back; empty after
    QString _startUpWorld;      // the domain the start-up world's lookups led to (host:port, or an ICE id)
};

#endif // hifi_ConnectionMonitor_h
