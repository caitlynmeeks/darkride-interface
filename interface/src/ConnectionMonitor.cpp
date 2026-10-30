//
//  ConnectionMonitor.cpp
//  interface/src
//
//  Created by Ryan Huffman on 8/4/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ConnectionMonitor.h"

#include "Application.h"
#include "InterfaceLogging.h"
#include "ui/DialogsManager.h"
#include "scripting/WindowScriptingInterface.h"

#include <DependencyManager.h>
#include <DomainHandler.h>
#include <AddressManager.h>
#include <NodeList.h>

// Because the connection monitor is created at startup, the time we wait on initial load
// should be longer to allow the application to initialize.
static const int ON_INITIAL_LOAD_REDIRECT_AFTER_DISCONNECTED_FOR_X_MS = 10000;
static const int REDIRECT_AFTER_DISCONNECTED_FOR_X_MS = 5000;

void ConnectionMonitor::init() {
    // Connect to domain disconnected message
    auto nodeList = DependencyManager::get<NodeList>();
    const DomainHandler& domainHandler = nodeList->getDomainHandler();
    connect(&domainHandler, &DomainHandler::resetting, this, &ConnectionMonitor::startTimer);
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, this, &ConnectionMonitor::startTimer);
    connect(&domainHandler, &DomainHandler::connectedToDomain, this, &ConnectionMonitor::stopTimer);
    connect(&domainHandler, &DomainHandler::domainConnectionRefused, this, &ConnectionMonitor::stopTimer);
    connect(&domainHandler, &DomainHandler::redirectToErrorDomainURL, this, &ConnectionMonitor::stopTimer);
    // left on purpose (the sign-in screen after a logout, #34): no time-out, no connection-failure window over it. Queued
    // after leave()'s resetting, so the timer that one started ends stopped
    connect(&domainHandler, &DomainHandler::left, this, &ConnectionMonitor::stopTimer);
    connect(this, &ConnectionMonitor::setRedirectErrorState, &domainHandler, &DomainHandler::setRedirectErrorState);

    _timer.setSingleShot(true);
    if (!domainHandler.isConnected()) {
        _timer.start(ON_INITIAL_LOAD_REDIRECT_AFTER_DISCONNECTED_FOR_X_MS);
    } else {
        _fallbackFrom.clear();
    }

    // the start-up world is the one its lookups lead to, again on every retry; a lookup leading elsewhere is a world the
    // person asked for meanwhile (a link, a hop), which keeps the failure window. A plain address has led there already
    if (!_fallbackFrom.isEmpty()) {
        if (!domainHandler.getHostname().isEmpty()) {
            _startUpWorld = domainHandler.getHostname() + ":" + QString::number(domainHandler.getPort());
        }
        auto addressManager = DependencyManager::get<AddressManager>();
        connect(addressManager.data(), &AddressManager::possibleDomainChangeRequired, this,
                [this](QUrl domainURL, QUuid) {
            startUpWorldAsked(domainURL.host() + ":" +
                              QString::number(domainURL.port() != -1 ? domainURL.port() : DEFAULT_DOMAIN_SERVER_PORT));
        });
        connect(addressManager.data(), &AddressManager::possibleDomainChangeRequiredViaICEForID, this,
                [this](const QString&, const QUuid& domainID) { startUpWorldAsked(domainID.toString()); });
    }

    connect(&_timer, &QTimer::timeout, this, [this]() {
        // the start-up world never answered (Caity's 1.1.1: her last world was an instance stopped since, tried again
        // and again behind the failure window): the entry world instead, the loading screen saying why. Once only:
        // if the entry doesn't answer either, the failure window as for any world
        if (!_fallbackFrom.isEmpty()) {
            const QString from = _fallbackFrom;
            _fallbackFrom.clear();
            qCInfo(interfaceapp) << "ConnectionMonitor: the start-up world (" << from << _startUpWorld
                                 << ") doesn't answer, going to the entry world";
            emit DependencyManager::get<WindowScriptingInterface>()->startUpWorldUnreachable(from);
            DependencyManager::get<AddressManager>()->goToEntry();
            _timer.start(ON_INITIAL_LOAD_REDIRECT_AFTER_DISCONNECTED_FOR_X_MS);   // a lookup that never answers
            return;
        }
        // scripts are told first: the loading screen (worldLoading.js) lifts, so the failure window isn't hidden behind it
        emit DependencyManager::get<WindowScriptingInterface>()->domainConnectionTimedOut();
        // set in a timeout error
        bool enableInterstitial = DependencyManager::get<NodeList>()->getDomainHandler().getInterstitialModeEnabled();
        if (enableInterstitial) {
            qDebug() << "ConnectionMonitor: Redirecting to 404 error domain";
            emit setRedirectErrorState(REDIRECT_HIFI_ADDRESS, "", 5);
        } else {
            qDebug() << "ConnectionMonitor: Showing connection failure window";
            DependencyManager::get<DialogsManager>()->setDomainConnectionFailureVisibility(true);
        }
    });
}

void ConnectionMonitor::startTimer() {
    _timer.start(REDIRECT_AFTER_DISCONNECTED_FOR_X_MS);
}

void ConnectionMonitor::startUpWorldAsked(const QString& world) {
    if (_fallbackFrom.isEmpty()) {
        return;
    }
    if (_startUpWorld.isEmpty()) {
        _startUpWorld = world;
    } else if (world != _startUpWorld) {
        qCDebug(interfaceapp) << "ConnectionMonitor: another world asked for during start-up, no fallback";
        _fallbackFrom.clear();
    }
}

void ConnectionMonitor::stopTimer() {
    // the start-up world answered (connected, refused, or left on purpose): nothing to fall back from any more
    _fallbackFrom.clear();
    _timer.stop();
    bool enableInterstitial = DependencyManager::get<NodeList>()->getDomainHandler().getInterstitialModeEnabled();
    if (!enableInterstitial) {
        DependencyManager::get<DialogsManager>()->setDomainConnectionFailureVisibility(false);
    }
}
