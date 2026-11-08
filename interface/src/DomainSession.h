//
//  DomainSession.h
//  interface/src
//
//  The client's session with a domain. The connection's events (a node added, activated or killed; the domain URL
//  changed, the domain resetting, a connection refused, a packet version mismatch, the session id); the queries the
//  client sends the entity and avatar servers (the octree query with its connection id, sent when the views changed
//  enough or the period passed, and the explicit refreshes), the NACKs for missing octree packets, the octree stats
//  and the packet accounting per server, the incoming octree packets' thread; the serverless domains (a world from a
//  file) and the interstitial mode (a loading screen: audio and avatar packets paused); the physics gate, which keeps
//  physics off from the arrival in a world until the scene near the avatar is in (safe landing, a full scene
//  received, the GPU textures settled), with the bookkeeping that decides it; the connection monitor; and where the
//  client goes first (goToStartingLocation), the domain's content reloaded (reloadResourceCaches), a later launch's
//  URL handed off (goToHandedOffUrl). Once a frame, in this order: enablePhysicsWhenReady() before the simulation,
//  queryServers() once the view is updated, sendNacksWhenDue() after it. A QObject: the node list's and the domain
//  handler's signals connect to it, and it tells the interstitial mode's changes. Application owns one, reached by
//  getDomainSession(); its isServerlessMode(), isInterstitialMode(), isPhysicsEnabled(), the octree accessors and
//  the refreshes the menu and the Test API reach by name forward here. These were Application's _octreeQuery,
//  _queryExpiry, _lastQueriedViews, _octreeProcessor, _octreeServerSceneStats, _connectionMonitor, the entity-server
//  connection timer, the physics-ready counters, the members of ApplicationDomain.cpp, the frame loop's physics
//  gate, query decision and NACK step, and the start-up's connection handlers (ARCHITECTURE.md §5.4 step 4, the
//  eighth owner class).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_DomainSession_h
#define hifi_DomainSession_h

#include <atomic>
#include <chrono>
#include <map>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVariantMap>

#include <Node.h>
#include <OctreeConstants.h>
#include <OctreeQuery.h>
#include <OctreeSceneStats.h>
#include <ReceivedMessage.h>
#include <SettingHandle.h>
#include <shared/ConicalViewFrustum.h>
#include <udt/PacketHeaders.h>

#include "ConnectionMonitor.h"
#include "octree/OctreePacketProcessor.h"

class DomainSession : public QObject {
    Q_OBJECT
public:
    DomainSession();   // the connection timer and the packet processor's mismatch signal wired; the packet rate read from its setting

    bool isServerlessMode() const;                                   // a world from a file, with no domain server
    bool isInterstitialMode() const { return _interstitialMode; }    // the loading screen is up: audio and avatar packets paused
    bool isPhysicsEnabled() const { return _physicsEnabled; }        // the arrival is complete: physics runs
    // whole-world loading: the domain's settings ask for a loading screen until the whole world is in (the models and
    // textures within a radius of the avatar loaded), with a time limit and "Show me now"
    bool isWorldLoading() const { return _worldLoading.active; }
    QVariantMap getWorldLoadingState();   // for the loading screen: received, ready, the server done, the seconds, the limit, the radius
    void showWorldNow();                  // "Show me now": physics at the next frame, the screen down
    bool isMissingSequenceNumbers() const { return _isMissingSequenceNumbers; }
    OctreePacketProcessor& octreeProcessor() { return _octreeProcessor; }               // the incoming octree packets' thread
    const OctreePacketProcessor& octreeProcessor() const { return _octreeProcessor; }
    NodeToOctreeSceneStats* octreeSceneStats() { return &_octreeServerSceneStats; }   // the stats per entity server
    int getMaxOctreePacketsPerSecond() const;
    void setMaxOctreePacketsPerSecond(int maxOctreePPS);
    // the Test API's: the texture gate's settle as a count of checks at the loading screen's 60 presents a second
    // (30 = the default 500 ms); the check floor follows it, up to the default's
    void setMinimumGPUTextureMemStabilityCount(int stabilityCount);
    bool gpuTextureMemSizeStable();
    void setArrivalWaitsForNearby(bool wait);   // a loading screen waits for what is near the avatar, even where the zone allows moving at once
    // The first arrival after start-up waits for what is near the avatar, as a hop does: set by the sign-in gate just
    // before it queues the first goToStartingLocation, because at a launch the default scripts (worldLoading.js, which
    // sets the wait for a hop) start in the same moment and lost the race (physics on before the script ran: the
    // first ride's car corrected by up to 1.2 m, physics 11-07). worldLoading.js takes the wait over when it starts
    // (its setArrivalWaitsForNearby); if nothing takes it over, it ends 8 s after the connect (LAUNCH_HOLD_LIMIT_SECONDS).
    void holdFirstArrival();
    void setOverrideEntry() { _overrideEntry = true; }   // the first run goes to the address given, not to the entry
    // A link the app is opened with: macOS hands a world link clicked while Darkride isn't running to the app as it
    // starts, before it can go anywhere. Kept until the starting location is chosen, which then goes there instead of
    // the last place, home or the entry (goToStartingLocation).
    bool startingLocationChosen() const { return _startingLocationChosen; }
    void keepStartupLink(const QString& url) { _startupLink = url; }
    // start-up that chooses no starting location (a test run without --url): a link the app was opened with is
    // followed all the same
    void noStartingLocation();

    // once a frame, from Application::update(), in this order
    void enablePhysicsWhenReady();        // the physics gate
    void queryServers();                  // the entity and avatar queries, under the view's lock
    void sendNacksWhenDue(quint64 now);   // once a second

    // the connection's events; the start-up connects them
    void connectedToDomain();
    void disconnectedFromDomain();
    void domainURLChanged(QUrl domainURL);
    void goToErrorDomainURL(QUrl errorDomainURL);
    void resettingDomain();
    void domainConnectionRefused(const QString& reasonMessage, int reason, const QString& extraInfo);
    void nodeAdded(SharedNodePointer node);
    void nodeActivated(SharedNodePointer node);
    void nodeKilled(SharedNodePointer node);
    void setSessionUUID(const QUuid& sessionUUID) const;
    void notifyPacketVersionMismatch();
    void arrivedWithoutPhysics();   // the avatar went somewhere while physics is off: safe landing restarted, the query at once

    // the packet processor's thread calls these
    int processOctreeStats(ReceivedMessage& message, SharedNodePointer sendingNode);
    void trackIncomingOctreePacket(ReceivedMessage& message, SharedNodePointer sendingNode, bool wasStatsPacket);

    // the explicit refreshes and the modes
    void requeryOctree();          // Redraw: a new connection id, the whole view resent
    void reloadResourceCaches();   // Reload Content
    void clearDomainOctreeDetails(bool clearAll = true);
    void resetPhysicsReadyInformation();
    void setIsInterstitialMode(bool interstitialMode);
    void setIsServerlessMode(bool serverlessDomain);
    void loadServerlessDomain(QUrl domainURL);
    void loadErrorDomain(QUrl domainURL);

    void goToStartingLocation();
    void goToHandedOffUrl(const QString& url) const;
    void shutdown();   // the packet processor's thread stopped

signals:
    void interstitialModeChanged(bool isInInterstitialMode);
    void worldLoadingChanged(bool isWorldLoading);
    // the arrival gate: true when the world is presented (physics on), false when a new arrival closes it again; the
    // loading-time work keys on it (the shader warm-up, the texture engine's loading boost), since
    // interstitialModeChanged fires only with the domain's interstitial setting or whole-world loading
    void physicsEnabledChanged(bool enabled);

private:
    // the arrival trace's gate line (TIVOLI_ARRIVAL_TRACE), once a second while physics is off; read-only
    void traceArrivalGate();
    void setFailedToConnectToEntityServer();
    std::map<QString, QString> prepareServerlessDomainContents(QUrl domainURL, QByteArray data);
    void tryToEnablePhysics(bool skipTextureCheck = false);
    void updateWorldLoading();   // the domain's settings read once they arrive; the whole-world load started when they ask for it
    void startWorldLoading(int timeLimitSeconds, float radius);
    void endWorldLoading();
    void queryOctree(NodeType_t serverType, PacketType packetType, QJsonObject jsonParameters);
    void queryAvatars();
    int sendNackPackets();

    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    // the entity-server session
    OctreeQuery _octreeQuery{ true };   // NodeData derived class for querying octee cells from octree servers
    TimePoint _queryExpiry;
    ConicalViewFrustums _lastQueriedViews;   // last views used to query servers
    Setting::Handle<int> _maxOctreePPSSetting{ "maxOctreePPS", DEFAULT_MAX_OCTREE_PPS };
    int _maxOctreePPS;
    QTimer _entityServerConnectionTimer;
    bool _failedToConnectToEntityServer{ false };
    bool _notifiedPacketVersionMismatchThisDomain{ false };
    OctreePacketProcessor _octreeProcessor;
    NodeToOctreeSceneStats _octreeServerSceneStats;
    bool _isMissingSequenceNumbers{ false };
    quint64 _lastNackTime;
    ConnectionMonitor _connectionMonitor;
    bool _interstitialMode{ false };
    bool _overrideEntry{ false };
    bool _startingLocationChosen{ false };
    QString _startupLink;

    // the physics gate
    bool _physicsEnabled{ false };
    std::atomic<bool> _arrivalWaitsForNearby{ false };
    void setArrivalWait(bool wait);    // the wait itself (the flag and SafeLanding's), whoever asks
    void updateFirstArrivalHold();     // once a frame, before the gate: the launch hold's time limit
    struct FirstArrivalHold {
        // the start-up's own wait, not yet taken over by a script; atomic: a script's setArrivalWaitsForNearby runs on
        // its engine's thread, the frame on the main thread (performance's item)
        std::atomic<bool> held { false };
        quint64 connectedAt { 0 };      // when the domain (or a world file) was first there while held
    } _firstArrivalHold;
    std::atomic<uint32_t> _fullSceneReceivedCounter{ 0 };  // how many times have we received a full-scene octree stats packet
    uint32_t _fullSceneCounterAtLastPhysicsCheck{ 0 };     // _fullSceneReceivedCounter last time we checked physics ready
    // the texture gate (gpuTextureMemSizeStable): the GPU texture memory size unchanged for a settle time, with a small
    // floor of checks; a check runs once per present. Until 2026-11-07 the gate was a count of 30 checks alone, which a
    // slowly presenting loading screen stretched from 0.5 s to several seconds (the shader warm-up's compiles, item 53)
    qint64 _gpuTextureMemSizeAtLastCheck{ 0 };
    qint64 _gpuTextureMemSizeStabilityCount{ 0 };     // consecutive checks that read the same size
    QElapsedTimer _gpuTextureMemSizeStableFor;        // since the size last changed (a monotonic clock); invalid = no check yet
    qint64 _gpuTextureMemSettleMs{ 500 };             // 500 ms = 30 checks at 60 presents a second, the old gate in front
    int _minimumGPUTextureMemSizeStabilityCount{ 5 };  // at least this many checks (a few slow presents cannot pass on two readings)
    // the arrival trace (TIVOLI_ARRIVAL_TRACE): one gate line a second while physics is off
    QElapsedTimer _arrivalTraceTimer;
    // the arrival trace's loading tail: started when physics turns on, the gate line goes on for 30 s more
    QElapsedTimer _arrivalTraceTail;
    bool _domainLoadingInProgress{ false };   // the "Scene Loading" profile range is open

    // Whole-world loading, per arrival (reset with the physics-ready information). The choice is the domain's
    // ("loading.whole_world" and "loading.time_limit" in its settings, sent to the client when it connects).
    struct WorldLoading {
        bool settingsKnown { false };   // the domain's settings have arrived, or we stopped waiting for them
        bool active { false };          // the loading screen is up and the entity query asks for the whole world
        bool skipRequested { false };   // "Show me now"
        quint64 waitStart { 0 };        // since when we are connected and waiting for the domain's settings
        quint64 started { 0 };
        quint64 deadline { 0 };
        int timeLimit { 0 };            // seconds
        float radius { 0.0f };          // metres round the avatar whose models are waited for; 0 for all
        quint64 lastQuery { 0 };
    } _worldLoading;
};

#endif // hifi_DomainSession_h
