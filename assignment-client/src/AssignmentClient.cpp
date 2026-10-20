//
//  AssignmentClient.cpp
//  assignment-client/src
//
//  Created by Stephen Birarda on 11/25/2013.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssignmentClient.h"

#include <assert.h>

#include <QProcess>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>

#include <shared/QtHelpers.h>
#include <AccountManager.h>
#include <AddressManager.h>
#include <Assignment.h>
#include <CrashAnnotations.h>
#include <LogHandler.h>
#include <LogUtils.h>
#include <LimitedNodeList.h>
#include <NodeList.h>
#include <udt/PacketHeaders.h>
#include <SharedUtil.h>
#include <ShutdownEventListener.h>

#include <Trace.h>
#include <StatTracker.h>

#include "AssignmentClientLogging.h"
#include "AssignmentFactory.h"
#include "ResourceRequestObserver.h"

const QString ASSIGNMENT_CLIENT_TARGET_NAME = "assignment-client";
const long long ASSIGNMENT_REQUEST_INTERVAL_MSECS = 1 * 1000;

AssignmentClient::AssignmentClient(Assignment::Type requestAssignmentType, QString assignmentPool,
                                   quint16 listenPort, QString assignmentServerHostname,
                                   quint16 assignmentServerPort, bool assignmentServerPortGiven, quint16 assignmentMonitorPort) :
    _assignmentServerHostname(DEFAULT_ASSIGNMENT_SERVER_HOSTNAME),
    _assignmentServerPortGiven(assignmentServerPortGiven)
{
    LogUtils::init();

    DependencyManager::set<tracing::Tracer>();
    DependencyManager::set<StatTracker>();
    DependencyManager::set<AccountManager>();
    DependencyManager::set<ResourceRequestObserver>();

    auto addressManager = DependencyManager::set<AddressManager>();

    // create a NodeList as an unassigned client, must be after addressManager
    auto nodeList = DependencyManager::set<NodeList>(NodeType::Unassigned, listenPort);

    nodeList->startThread();
    // set the logging target to the the CHILD_TARGET_NAME
    LogHandler::getInstance().setTargetName(ASSIGNMENT_CLIENT_TARGET_NAME);

    // make sure we output process IDs for a child AC otherwise it's insane to parse
    LogHandler::getInstance().setShouldOutputProcessID(true);

    // setup our _requestAssignment member variable from the passed arguments
    _requestAssignment = Assignment(Assignment::RequestCommand, requestAssignmentType, assignmentPool);

    // check for an overriden assignment server hostname
    if (assignmentServerHostname != "") {
        // change the hostname for our assignment server
        _assignmentServerHostname = assignmentServerHostname;
    }

    _assignmentServerSocket = HifiSockAddr(_assignmentServerHostname, assignmentServerPort, true);
    if (_assignmentServerSocket.isNull()) {
        qCCritical(assignment_client) << "PAGE: Couldn't resolve domain server address" << _assignmentServerHostname;
    }
    _assignmentServerSocket.setObjectName("AssignmentServer");
    nodeList->setAssignmentServerSocket(_assignmentServerSocket);

    qCDebug(assignment_client) << "Assignment server socket is" << _assignmentServerSocket;

    // call a timer function every ASSIGNMENT_REQUEST_INTERVAL_MSECS to ask for assignment, if required
    qCDebug(assignment_client) << "Waiting for assignment -" << _requestAssignment;

    if (_assignmentServerHostname != "localhost" || _assignmentServerPortGiven) {
        qCDebug(assignment_client) << "- will attempt to connect to domain-server on" << _assignmentServerSocket.getPort();
    }

    connect(&_requestTimer, SIGNAL(timeout()), SLOT(sendAssignmentRequest()));
    _requestTimer.start(ASSIGNMENT_REQUEST_INTERVAL_MSECS);

    // connections to AccountManager for authentication
    connect(DependencyManager::get<AccountManager>().data(), &AccountManager::authRequired,
            this, &AssignmentClient::handleAuthenticationRequest);

    // Create Singleton objects on main thread
    NetworkAccessManager::getInstance();

    // an AI resident's body (AI-FIRST-CLIENT.md §10.3): sign in now, before any assignment arrives,
    // so the agent's name and presence follow its account from the first identity packet
    signInWithConnectionKey();

    // did we get an assignment-client monitor port?
    if (assignmentMonitorPort > 0) {
        _assignmentClientMonitorSocket = HifiSockAddr(DEFAULT_ASSIGNMENT_CLIENT_MONITOR_HOSTNAME, assignmentMonitorPort);
        _assignmentClientMonitorSocket.setObjectName("AssignmentClientMonitor");

        qCDebug(assignment_client) << "Assignment-client monitor socket is" << _assignmentClientMonitorSocket;

        // Hook up a timer to send this child's status to the Monitor once per second
        setUpStatusToMonitor();
    }
    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::CreateAssignment, this, "handleCreateAssignmentPacket");
    packetReceiver.registerListener(PacketType::StopNode, this, "handleStopNodePacket");
}

void AssignmentClient::stopAssignmentClient() {
    qCDebug(assignment_client) << "Forced stop of assignment-client.";

    _requestTimer.stop();
    _statsTimerACM.stop();

    if (_currentAssignment) {
        // grab the thread for the current assignment
        QThread* currentAssignmentThread = _currentAssignment->thread();

        // ask the current assignment to stop
        QMetaObject::invokeMethod(_currentAssignment, "stop");

        auto PROCESS_EVENTS_INTERVAL_MS = 100;
        while (!currentAssignmentThread->wait(PROCESS_EVENTS_INTERVAL_MS)) {
            QCoreApplication::processEvents();
        }
    }
}

AssignmentClient::~AssignmentClient() {
    // remove the NodeList from the DependencyManager
    DependencyManager::destroy<NodeList>();
}

void AssignmentClient::aboutToQuit() {
    crash::annotations::setShutdownState(true);
    stopAssignmentClient();
}

void AssignmentClient::setUpStatusToMonitor() {
    // send a stats packet every 1 seconds
    connect(&_statsTimerACM, &QTimer::timeout, this, &AssignmentClient::sendStatusPacketToACM);
    _statsTimerACM.start(1000);
}

void AssignmentClient::sendStatusPacketToACM() {
    // tell the assignment client monitor what this assignment client is doing (if anything)
    auto nodeList = DependencyManager::get<NodeList>();
    
    quint8 assignmentType = Assignment::Type::AllTypes;

    if (_currentAssignment) {
        assignmentType = _currentAssignment->getType();
    }

    auto statusPacket = NLPacket::create(PacketType::AssignmentClientStatus, sizeof(assignmentType) + NUM_BYTES_RFC4122_UUID);

    statusPacket->write(_childAssignmentUUID.toRfc4122());
    statusPacket->writePrimitive(assignmentType);
    
    nodeList->sendPacket(std::move(statusPacket), _assignmentClientMonitorSocket);
}

void AssignmentClient::sendAssignmentRequest() {
    if (!_currentAssignment && !_isAssigned) {
        crash::annotations::setShutdownState(false);

        auto nodeList = DependencyManager::get<NodeList>();

        // A domain-server on this machine leaves its port in shared memory, for a client that was told only
        // "localhost": with several local domains the slot holds whichever started last, so a port given on the
        // command line is kept and the slot is read only when none was.
        if (_assignmentServerHostname == "localhost" && !_assignmentServerPortGiven) {
            // we want to check again for the local domain-server port in case the DS has restarted
            quint16 localAssignmentServerPort;
            if (nodeList->getLocalServerPortFromSharedMemory(DOMAIN_SERVER_LOCAL_PORT_SMEM_KEY, localAssignmentServerPort)) {
                if (localAssignmentServerPort == 0) {
                    qCWarning(assignment_client) << "ALERT: Server port from shared memory is 0";
                } else {
                    if (localAssignmentServerPort != _assignmentServerSocket.getPort()) {
                        qCDebug(assignment_client) << "Port for local assignment server read from shared memory is"
                            << localAssignmentServerPort;

                        _assignmentServerSocket.setPort(localAssignmentServerPort);
                        nodeList->setAssignmentServerSocket(_assignmentServerSocket);
                    }
                }
            } else {
                qCWarning(assignment_client) << "ALERT: Failed to read local assignment server port from shared memory ("
                    << DOMAIN_SERVER_LOCAL_PORT_SMEM_KEY
                    << ")- will send assignment request to previous assignment server socket.";
            }
        }

        nodeList->sendAssignment(_requestAssignment);
    }
}

void AssignmentClient::handleCreateAssignmentPacket(QSharedPointer<ReceivedMessage> message) {
    qCDebug(assignment_client) << "Received a PacketType::CreateAssignment - attempting to unpack.";

    if (_currentAssignment) {
        qCWarning(assignment_client) << "Received a PacketType::CreateAssignment while still running an active assignment. Ignoring.";
        return;
    }

    // construct the deployed assignment from the packet data
    _currentAssignment = AssignmentFactory::unpackAssignment(*message);

    if (_currentAssignment && !_isAssigned) {
        qDebug(assignment_client) << "Received an assignment -" << *_currentAssignment;
        _isAssigned = true;

        auto nodeList = DependencyManager::get<NodeList>();

        // switch our DomainHandler hostname and port to whoever sent us the assignment

        nodeList->getDomainHandler().setSockAddr(message->getSenderSockAddr(), _assignmentServerHostname);
        // a port given on the command line stays the domain's when the agent connects, as it did when it asked
        // for work: setSockAddr knows nothing of the command line, and NodeList would otherwise read the shared-memory
        // slot for a "localhost" domain and follow whichever local domain started last
        nodeList->getDomainHandler().setExplicitPort(_assignmentServerPortGiven);
        nodeList->getDomainHandler().setAssignmentUUID(_currentAssignment->getUUID());

        qCDebug(assignment_client) << "Destination IP for assignment is" << nodeList->getDomainHandler().getIP().toString();

        // start the deployed assignment
        QThread* workerThread = new QThread;
        workerThread->setObjectName("ThreadedAssignment Worker");

        connect(workerThread, &QThread::started, _currentAssignment.data(), &ThreadedAssignment::run);

        // Once the ThreadedAssignment says it is finished - we ask it to deleteLater
        // This is a queued connection so that it is put into the event loop to be processed by the worker
        // thread when it is ready.
        connect(_currentAssignment.data(), &ThreadedAssignment::finished, _currentAssignment.data(),
                &ThreadedAssignment::deleteLater, Qt::QueuedConnection);

        // once it is deleted, we quit the worker thread
        connect(_currentAssignment.data(), &ThreadedAssignment::destroyed, workerThread, &QThread::quit);

        // have the worker thread remove itself once it is done
        connect(workerThread, &QThread::finished, workerThread, &QThread::deleteLater);

        // once the worker thread says it is done, we consider the assignment completed
        connect(workerThread, &QThread::destroyed, this, &AssignmentClient::assignmentCompleted);

        _currentAssignment->moveToThread(workerThread);

        // Starts an event loop, and emits workerThread->started()
        workerThread->start();
    } else {
        qCWarning(assignment_client) << "ALERT: Received an assignment that could not be unpacked. Re-requesting.";
    }
}

void AssignmentClient::handleStopNodePacket(QSharedPointer<ReceivedMessage> message) {
    const HifiSockAddr& senderSockAddr = message->getSenderSockAddr();
    
    if (senderSockAddr.getAddress() == QHostAddress::LocalHost ||
        senderSockAddr.getAddress() == QHostAddress::LocalHostIPv6) {
        
        qCDebug(assignment_client) << "AssignmentClientMonitor at" << senderSockAddr << "requested stop via PacketType::StopNode.";
        QCoreApplication::quit();
    } else {
        qCWarning(assignment_client) << "Got a stop packet from other than localhost.";
    }
}

void AssignmentClient::handleAuthenticationRequest() {
    const QString DATA_SERVER_USERNAME_ENV = "HIFI_AC_USERNAME";
    const QString DATA_SERVER_PASSWORD_ENV = "HIFI_AC_PASSWORD";

    // this node will be using an authentication server, let's make sure we have a username/password
    QProcessEnvironment sysEnvironment = QProcessEnvironment::systemEnvironment();

    QString username = sysEnvironment.value(DATA_SERVER_USERNAME_ENV);
    QString password = sysEnvironment.value(DATA_SERVER_PASSWORD_ENV);

    auto accountManager = DependencyManager::get<AccountManager>();

    if (!username.isEmpty() && !password.isEmpty()) {
        // ask the account manager to log us in from the env variables
        accountManager->requestAccessToken(username, password);
    } else {
        qCWarning(assignment_client) << "Authentication was requested against" << qPrintable(accountManager->getAuthURL().toString())
            << "but both or one of" << qPrintable(DATA_SERVER_USERNAME_ENV)
            << "/" << qPrintable(DATA_SERVER_PASSWORD_ENV) << "are not set. Unable to authenticate.";

        return;
    }
}

// HIFI_AC_USERNAME + HIFI_AC_KEY (the resident's connection key, made by its host on the metaverse
// website) sign this process in as that account with the client-credentials grant. The metaverse is
// HIFI_METAVERSE_URL. Nothing is written to disk. A refused key is logged with the metaverse's reason
// ("paused by its host", ...). Without the two variables nothing changes: a plain assignment client.
void AssignmentClient::signInWithConnectionKey() {
    const QString USERNAME_ENV = "HIFI_AC_USERNAME";
    const QString KEY_ENV = "HIFI_AC_KEY";

    QProcessEnvironment sysEnvironment = QProcessEnvironment::systemEnvironment();
    QString username = sysEnvironment.value(USERNAME_ENV);
    QString key = sysEnvironment.value(KEY_ENV);

    if (username.isEmpty() || key.isEmpty()) {
        return;
    }

    // the key is kept in memory from here; out of the environment, nothing this process runs or starts can read it
    qunsetenv(KEY_ENV.toLocal8Bit().constData());

    auto accountManager = DependencyManager::get<AccountManager>();
    accountManager->setAuthURL(NetworkingConstants::METAVERSE_SERVER_URL());

    qCDebug(assignment_client) << "Signing in to" << qPrintable(accountManager->getAuthURL().toString())
        << "as the AI resident" << username << "with its connection key";

    connect(accountManager.data(), &AccountManager::loginComplete, this, [accountManager] {
        // the key pair lets a domain verify the username (round 2); the public key goes to the metaverse
        if (!accountManager->hasKeyPair()) {
            accountManager->generateNewUserKeypair();
        }
    });
    connect(accountManager.data(), &AccountManager::usernameChanged, this, [](const QString& username) {
        qCDebug(assignment_client) << "Signed in as" << username;
    });
    connect(accountManager.data(), &AccountManager::loginFailed, this, [] {
        qCWarning(assignment_client) << "The connection key was refused (the reason is in the line above)";
    });

    accountManager->requestAccessTokenWithKey(username, key);
}

void AssignmentClient::assignmentCompleted() {
    crash::annotations::setShutdownState(true);
    
    // we expect that to be here the previous assignment has completely cleaned up
    assert(_currentAssignment.isNull());

    // reset our current assignment pointer to null now that it has been deleted
    _currentAssignment = nullptr;

    // reset the logging target to the the CHILD_TARGET_NAME
    LogHandler::getInstance().setTargetName(ASSIGNMENT_CLIENT_TARGET_NAME);

    qCDebug(assignment_client) << "Assignment finished or never started - waiting for new assignment.";

    auto nodeList = DependencyManager::get<NodeList>();

    // tell the packet receiver to stop dropping packets
    nodeList->getPacketReceiver().setShouldDropPackets(false);

    // reset our NodeList by switching back to unassigned and clearing the list
    nodeList->setOwnerType(NodeType::Unassigned);
    nodeList->reset("Assignment completed");
    nodeList->resetNodeInterestSet();
    
    _isAssigned = false;
}
