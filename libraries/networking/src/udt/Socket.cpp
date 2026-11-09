//
//  Socket.cpp
//  libraries/networking/src/udt
//
//  Created by Stephen Birarda on 2015-07-20.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Socket.h"

#include <QtCore/QTimer>

#include <glm/glm.hpp>
#include <SharedUtil.h>

#ifdef Q_OS_ANDROID
#include <sys/socket.h>
#endif

#include <QtCore/QThread>

#include <shared/QtHelpers.h>
#include <LogHandler.h>

#include "../NetworkLogging.h"
#include "Connection.h"
#include "ControlPacket.h"
#include "Packet.h"
#include "../NLPacket.h"
#include "../NLPacketList.h"
#include "PacketList.h"
#include <Trace.h>

using namespace udt;

#ifdef WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#include <netinet/in.h>
#endif


Socket::Socket(QObject* parent, bool shouldChangeSocketOptions) :
    QObject(parent),
    _udpSocket(parent),
    _readyReadBackupTimer(new QTimer(this)),
    _shouldChangeSocketOptions(shouldChangeSocketOptions)
{
    connect(&_udpSocket, &QUdpSocket::readyRead, this, &Socket::readPendingDatagrams);

    // make sure we hear about errors and state changes from the underlying socket
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)  // Qt 6 renamed the error signal; a string connect to the old name never fires
    connect(&_udpSocket, SIGNAL(errorOccurred(QAbstractSocket::SocketError)),
#else
    connect(&_udpSocket, SIGNAL(error(QAbstractSocket::SocketError)),
#endif
            this, SLOT(handleSocketError(QAbstractSocket::SocketError)));
    connect(&_udpSocket, &QAbstractSocket::stateChanged, this, &Socket::handleStateChanged);

    // in order to help track down the zombie server bug, add a timer to check if we missed a readyRead
    const int READY_READ_BACKUP_CHECK_MSECS = 2 * 1000;
    connect(_readyReadBackupTimer, &QTimer::timeout, this, &Socket::checkForReadyReadBackup);
    _readyReadBackupTimer->start(READY_READ_BACKUP_CHECK_MSECS);
}

void Socket::bind(const QHostAddress& address, quint16 port) {

    _udpSocket.bind(address, port);

    if (_shouldChangeSocketOptions) {
        setSystemBufferSizes();

#if defined(Q_OS_LINUX)
        auto sd = _udpSocket.socketDescriptor();
        int val = IP_PMTUDISC_DONT;
        setsockopt(sd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
#elif defined(Q_OS_WIN)
        auto sd = _udpSocket.socketDescriptor();
        int val = 0; // false
        if (setsockopt(sd, IPPROTO_IP, IP_DONTFRAGMENT, (const char *)&val, sizeof(val))) {
            auto wsaErr = WSAGetLastError();
            qCWarning(networking) << "Socket::bind Cannot setsockopt IP_DONTFRAGMENT" << wsaErr;
        }
#endif
    }
}

void Socket::rebind() {
    rebind(_udpSocket.localPort());
}

void Socket::rebind(quint16 localPort) {
    _udpSocket.abort();
    bind(QHostAddress::AnyIPv4, localPort);
}

void Socket::setSystemBufferSizes() {
    for (int i = 0; i < 2; i++) {
        QAbstractSocket::SocketOption bufferOpt;
        QString bufferTypeString;

        int numBytes = 0;

        if (i == 0) {
            bufferOpt = QAbstractSocket::SendBufferSizeSocketOption;
            numBytes = udt::UDP_SEND_BUFFER_SIZE_BYTES;
            bufferTypeString = "send";

        } else {
            bufferOpt = QAbstractSocket::ReceiveBufferSizeSocketOption;
            numBytes = udt::UDP_RECEIVE_BUFFER_SIZE_BYTES;
            bufferTypeString = "receive";
        }

        int oldBufferSize = _udpSocket.socketOption(bufferOpt).toInt();

        if (oldBufferSize < numBytes) {
            _udpSocket.setSocketOption(bufferOpt, QVariant(numBytes));
            int newBufferSize = _udpSocket.socketOption(bufferOpt).toInt();

            qCDebug(networking) << "Changed socket" << bufferTypeString << "buffer size from" << oldBufferSize << "to"
                << newBufferSize << "bytes";
        } else {
            // don't make the buffer smaller
            qCDebug(networking) << "Did not change socket" << bufferTypeString << "buffer size from" << oldBufferSize
                << "since it is larger than desired size of" << numBytes;
        }
    }
}

qint64 Socket::writeBasePacket(const udt::BasePacket& packet, const HifiSockAddr &sockAddr) {
    // Since this is a base packet we have no way to know if this is reliable or not - we just fire it off

    // this should not be called with an instance of Packet
    Q_ASSERT_X(!dynamic_cast<const Packet*>(&packet),
               "Socket::writeBasePacket", "Cannot send a Packet/NLPacket via writeBasePacket");

    return writeDatagram(packet.getData(), packet.getDataSize(), sockAddr);
}

qint64 Socket::writePacket(const Packet& packet, const HifiSockAddr& sockAddr) {
    Q_ASSERT_X(!packet.isReliable(), "Socket::writePacket", "Cannot send a reliable packet unreliably");

    SequenceNumber sequenceNumber;
    {
        Lock lock(_unreliableSequenceNumbersMutex);
        sequenceNumber = ++_unreliableSequenceNumbers[sockAddr];
    }

    auto connection = findOrCreateConnection(sockAddr, true);
    if (connection) {
        connection->recordSentUnreliablePackets(packet.getWireSize(),
                                                packet.getPayloadSize());
    }

    // write the correct sequence number to the Packet here
    packet.writeSequenceNumber(sequenceNumber);

    return writeDatagram(packet.getData(), packet.getDataSize(), sockAddr);
}

qint64 Socket::writePacket(std::unique_ptr<Packet> packet, const HifiSockAddr& sockAddr) {

    if (packet->isReliable()) {
        // hand this packet off to writeReliablePacket
        // because Qt can't invoke with the unique_ptr we have to release it here and re-construct in writeReliablePacket

        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, "writeReliablePacket", Qt::QueuedConnection,
                                      Q_ARG(Packet*, packet.release()),
                                      Q_ARG(HifiSockAddr, sockAddr));
        } else {
            writeReliablePacket(packet.release(), sockAddr);
        }

        return 0;
    }

    return writePacket(*packet, sockAddr);
}

qint64 Socket::writePacketList(std::unique_ptr<PacketList> packetList, const HifiSockAddr& sockAddr) {

    if (packetList->getNumPackets() == 0) {
        qCWarning(networking) << "Trying to send packet list with 0 packets, bailing.";
        return 0;
    }

    if (packetList->isReliable()) {
        // hand this packetList off to writeReliablePacketList
        // because Qt can't invoke with the unique_ptr we have to release it here and re-construct in writeReliablePacketList

        if (QThread::currentThread() != thread()) {
            auto ptr = packetList.release();
            QMetaObject::invokeMethod(this, "writeReliablePacketList", Qt::AutoConnection,
                                      Q_ARG(PacketList*, ptr),
                                      Q_ARG(HifiSockAddr, sockAddr));
        } else {
            writeReliablePacketList(packetList.release(), sockAddr);
        }

        return 0;
    }

    // Unerliable and Unordered
    qint64 totalBytesSent = 0;
    while (!packetList->_packets.empty()) {
        totalBytesSent += writePacket(packetList->takeFront<Packet>(), sockAddr);
    }
    return totalBytesSent;
}

void Socket::writeReliablePacket(Packet* packet, const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendReliablePacket(std::unique_ptr<Packet>(packet));
    }
#ifdef UDT_CONNECTION_DEBUG
    else {
        qCDebug(networking) << "Socket::writeReliablePacket refusing to send packet - no connection was created";
    }
#endif

}

void Socket::writeReliablePacketList(PacketList* packetList, const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendReliablePacketList(std::unique_ptr<PacketList>(packetList));
    }
#ifdef UDT_CONNECTION_DEBUG
    else {
        qCDebug(networking) << "Socket::writeReliablePacketList refusing to send packet list - no connection was created";
    }
#endif
}

qint64 Socket::writeDatagram(const char* data, qint64 size, const HifiSockAddr& sockAddr) {
    return writeDatagram(QByteArray::fromRawData(data, size), sockAddr);
}

// A test knob: TIVOLI_PACKET_LOSS=0.05 in the environment drops that fraction of the datagrams this process sends
// and receives, at random, so a client can be watched under loss (PHYSICS.md round 5's acceptance test). Off
// unless set; never set it for a server.
static float simulatedPacketLoss() {
    static const float rate = []{
        QByteArray value = qgetenv("TIVOLI_PACKET_LOSS");
        bool ok = false;
        float parsed = value.isEmpty() ? 0.0f : value.toFloat(&ok);
        float clamped = ok ? glm::clamp(parsed, 0.0f, 0.99f) : 0.0f;
        if (clamped > 0.0f) {
            qCWarning(networking) << "TIVOLI_PACKET_LOSS set: dropping" << clamped * 100.0f << "% of datagrams, in and out";
        }
        return clamped;
    }();
    return rate;
}

static bool dropForTest() {
    float rate = simulatedPacketLoss();
    return rate > 0.0f && randFloat() < rate;
}

// A second test knob: TIVOLI_PACKET_DELAY_MS=100 holds every datagram this process sends for that long before it goes
// out, so a client can be watched with latency (PHYSICS.md round 6: the driver's prediction). Off unless set; never
// set it for a server.
static int simulatedDelayMs() {
    static const int delay = []{
        QByteArray value = qgetenv("TIVOLI_PACKET_DELAY_MS");
        bool ok = false;
        int parsed = value.isEmpty() ? 0 : value.toInt(&ok);
        int clamped = ok ? glm::clamp(parsed, 0, 5000) : 0;
        if (clamped > 0) {
            qCWarning(networking) << "TIVOLI_PACKET_DELAY_MS set: every datagram out is held" << clamped << "ms";
        }
        return clamped;
    }();
    return delay;
}

qint64 Socket::writeDatagram(const QByteArray& datagram, const HifiSockAddr& sockAddr) {

    if (dropForTest()) {
        return datagram.size();   // lost on the way out, as far as the sender knows
    }
    int delay = simulatedDelayMs();
    if (delay > 0) {
        // held on this socket's thread, which has the event loop, and sent from there; a deep copy, since the caller's
        // array usually wraps a packet's buffer without owning it (QByteArray::fromRawData) and the packet is gone by then
        QByteArray copy(datagram.constData(), datagram.size());
        QTimer::singleShot(delay, this, [this, copy, sockAddr] { writeDatagramNow(copy, sockAddr); });
        return datagram.size();
    }
    return writeDatagramNow(datagram, sockAddr);
}

qint64 Socket::writeDatagramNow(const QByteArray& datagram, const HifiSockAddr& sockAddr) {

    // don't attempt to write the datagram if we're unbound.  Just drop it.
    // _udpSocket.writeDatagram will return an error anyway, but there are
    // potential crashes in Qt when that happens.
    if (_udpSocket.state() != QAbstractSocket::BoundState) {
        qCDebug(networking) << "Attempt to writeDatagram when in unbound state to" << sockAddr;
        return -1;
    }
    qint64 bytesWritten = _udpSocket.writeDatagram(datagram, sockAddr.getAddress(), sockAddr.getPort());
    int pending = _udpSocket.bytesToWrite();
    if (bytesWritten < 0 || pending) {
        int wsaError = 0;
        static std::atomic<int> previousWsaError (0);
#ifdef WIN32
        wsaError = WSAGetLastError();
#endif
        QString errorString;
        QDebug(&errorString) << "udt::writeDatagram (" << _udpSocket.state() << sockAddr << ") error - "
            << wsaError << _udpSocket.error() << "(" << _udpSocket.errorString() << ")"
            << (pending ? "pending bytes:" : "pending:") << pending;

        if (previousWsaError.exchange(wsaError) != wsaError) {
            qCDebug(networking).noquote() << errorString;
#ifdef DEBUG_EVENT_QUEUE
            int nodeListQueueSize = ::hifi::qt::getEventQueueSize(thread());
            qCDebug(networking) << "Networking queue size - " << nodeListQueueSize << "writing datagram to" << sockAddr;
#endif  // DEBUG_EVENT_QUEUE
        } else {
            HIFI_FCDEBUG(networking(), errorString.toLatin1().constData());
        }
    }

    return bytesWritten;
}

Connection* Socket::findOrCreateConnection(const HifiSockAddr& sockAddr, bool filterCreate) {
    Lock connectionsLock(_connectionsHashMutex);
    auto it = _connectionsHash.find(sockAddr);

    if (it == _connectionsHash.end()) {
        // we did not have a matching connection, time to see if we should make one

        if (filterCreate && _connectionCreationFilterOperator && !_connectionCreationFilterOperator(sockAddr)) {
            // the connection creation filter did not allow us to create a new connection
#ifdef UDT_CONNECTION_DEBUG
            qCDebug(networking) << "Socket::findOrCreateConnection refusing to create Connection class for" << sockAddr
                << "due to connection creation filter";
#endif // UDT_CONNECTION_DEBUG
            return nullptr;
        } else {
            auto congestionControl = _ccFactory->create();
            congestionControl->setMaxBandwidth(_maxBandwidth);
            auto connection = std::unique_ptr<Connection>(new Connection(this, sockAddr, std::move(congestionControl)));
            if (QThread::currentThread() != thread()) {
                qCDebug(networking) << "Moving new Connection to NodeList thread";
                connection->moveToThread(thread());
            }
            // allow higher-level classes to find out when connections have completed a handshake
            QObject::connect(connection.get(), &Connection::receiverHandshakeRequestComplete,
                             this, &Socket::clientHandshakeRequestComplete);

            qCDebug(networking) << "Creating new Connection class for" << sockAddr;

            it = _connectionsHash.insert(it, std::make_pair(sockAddr, std::move(connection)));
        }
    }

    return it->second.get();
}

void Socket::clearConnections() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "clearConnections");
        return;
    }

    Lock connectionsLock(_connectionsHashMutex);
    if (_connectionsHash.size() > 0) {
        // clear all of the current connections in the socket
        qCDebug(networking) << "Clearing all remaining connections in Socket.";
        _connectionsHash.clear();
    }
}

void Socket::cleanupConnection(HifiSockAddr sockAddr) {
    Lock connectionsLock(_connectionsHashMutex);
    auto numErased = _connectionsHash.erase(sockAddr);

    if (numErased > 0) {
#ifdef UDT_CONNECTION_DEBUG
        qCDebug(networking) << "Socket::cleanupConnection called for UDT connection to" << sockAddr;
#endif
    }
}

void Socket::messageReceived(std::unique_ptr<Packet> packet) {
    if (_messageHandler) {
        _messageHandler(std::move(packet));
    }
}

void Socket::messageFailed(Connection* connection, Packet::MessageNumber messageNumber) {
    if (_messageFailureHandler) {
        _messageFailureHandler(connection->getDestination(), messageNumber);
    }
}

void Socket::checkForReadyReadBackup() {
    if (_udpSocket.hasPendingDatagrams()) {
        qCDebug(networking) << "Socket::checkForReadyReadBackup() detected blocked readyRead signal. Flushing pending datagrams.";

        // so that birarda can possibly figure out how the heck we get into this state in the first place
        // output the sequence number and socket address of the last processed packet
        qCDebug(networking) << "Socket::checkForReadyReadyBackup() last sequence number"
            << (uint32_t) _lastReceivedSequenceNumber << "from" << _lastPacketSockAddr << "-"
            << _lastPacketSizeRead << "bytes";
#ifdef DEBUG_EVENT_QUEUE
        qCDebug(networking) << "NodeList event queue size:" << ::hifi::qt::getEventQueueSize(thread());
#endif

        // drop all of the pending datagrams on the floor
        int droppedCount = 0;
        while (_udpSocket.hasPendingDatagrams()) {
            _udpSocket.readDatagram(nullptr, 0);
            ++droppedCount;
        }
        qCDebug(networking) << "Flushed" << droppedCount << "Packets";
    }
}

void Socket::readPendingDatagrams() {
    using namespace std::chrono;
    static const auto MAX_PROCESS_TIME { 100ms };
    const auto abortTime = system_clock::now() + MAX_PROCESS_TIME;
    int packetSizeWithHeader = -1;

    while (_udpSocket.hasPendingDatagrams() &&
           (packetSizeWithHeader = _udpSocket.pendingDatagramSize()) != -1) {
        if (system_clock::now() > abortTime) {
            // We've been running for too long, stop processing packets for now
            // Once we've processed the event queue, we'll come back to packet processing
#ifdef DEBUG_EVENT_QUEUE
            int nodeListQueueSize = ::hifi::qt::getEventQueueSize(thread());
            qCDebug(networking) << "Overran timebox by" << duration_cast<milliseconds>(system_clock::now() - abortTime).count()
                << "ms; NodeList thread event queue size =" << nodeListQueueSize;
#endif
            break;
        }

        // we're reading a packet so re-start the readyRead backup timer
        _readyReadBackupTimer->start();

        // grab a time point we can mark as the receive time of this packet
        auto receiveTime = p_high_resolution_clock::now();

        // setup a HifiSockAddr to read into
        HifiSockAddr senderSockAddr;

        // setup a buffer to read the packet into
        auto buffer = std::unique_ptr<char[]>(new char[packetSizeWithHeader]);

        // pull the datagram
        auto sizeRead = _udpSocket.readDatagram(buffer.get(), packetSizeWithHeader,
                                                senderSockAddr.getAddressPointer(), senderSockAddr.getPortPointer());

        // save information for this packet, in case it is the one that sticks readyRead
        _lastPacketSizeRead = sizeRead;
        _lastPacketSockAddr = senderSockAddr;

        if (sizeRead <= 0) {
            // we either didn't pull anything for this packet or there was an error reading (this seems to trigger
            // on windows even if there's not a packet available)
            continue;
        }

        if (dropForTest()) {
            continue;   // lost on the way in
        }

        auto it = _unfilteredHandlers.find(senderSockAddr);

        if (it != _unfilteredHandlers.end()) {
            // we have a registered unfiltered handler for this HifiSockAddr - call that and return
            if (it->second) {
                auto basePacket = BasePacket::fromReceivedPacket(std::move(buffer), sizeRead, senderSockAddr);
                basePacket->setReceiveTime(receiveTime);
                it->second(std::move(basePacket));
            }

            continue;
        }

        // a datagram is size-checked against its header before it is read as a packet
        if (sizeRead < (qint64)sizeof(uint32_t)) {
            dropMalformedDatagram(senderSockAddr, sizeRead);
            continue;
        }

        // check if this was a control packet or a data packet
        uint32_t firstWord;
        memcpy(&firstWord, buffer.get(), sizeof(firstWord));
        bool isControlPacket = firstWord & CONTROL_BIT_MASK;

        if (isControlPacket) {
            // setup a control packet from the data we just read
            auto controlPacket = ControlPacket::fromReceivedPacket(std::move(buffer), sizeRead, senderSockAddr);
            if (!controlPacket) {
                dropMalformedDatagram(senderSockAddr, sizeRead);
                continue;
            }
            controlPacket->setReceiveTime(receiveTime);

            // move this control packet to the matching connection, if there is one
            auto connection = findOrCreateConnection(senderSockAddr, true);

            if (connection) {
                connection->processControl(move(controlPacket));
            }

        } else {
            // setup a Packet from the data we just read
            auto packet = Packet::fromReceivedPacket(std::move(buffer), sizeRead, senderSockAddr);
            if (!packet) {
                dropMalformedDatagram(senderSockAddr, sizeRead);
                continue;
            }
            packet->setReceiveTime(receiveTime);

            // save the sequence number in case this is the packet that sticks readyRead
            _lastReceivedSequenceNumber = packet->getSequenceNumber();

            // call our verification operator to see if this packet is verified
            if (!_packetFilterOperator || _packetFilterOperator(*packet)) {
                auto connection = findOrCreateConnection(senderSockAddr, true);

                if (packet->isReliable()) {
                    // if this was a reliable packet then signal the matching connection with the sequence number

                    if (!connection || !connection->processReceivedSequenceNumber(packet->getSequenceNumber(),
                                                                                  packet->getDataSize(),
                                                                                  packet->getPayloadSize())) {
                        // the connection could not be created or indicated that we should not continue processing this packet
#ifdef UDT_CONNECTION_DEBUG
                        qCDebug(networking) << "Can't process packet: version" << (unsigned int)NLPacket::versionInHeader(*packet)
                            << ", type" << NLPacket::typeInHeader(*packet);
#endif
                        continue;
                    }
                } else if (connection) {
                    connection->recordReceivedUnreliablePackets(packet->getWireSize(),
                                                                packet->getPayloadSize());
                }

                if (packet->isPartOfMessage()) {
                    auto connection = findOrCreateConnection(senderSockAddr, true);
                    if (connection) {
                        connection->queueReceivedMessagePacket(std::move(packet));
                    }
                } else if (_packetHandler) {
                    // call the verified packet callback to let it handle this packet
                    _packetHandler(std::move(packet));
                }
            }
        }
    }
}

// one log line at most every 10 s, with how many were dropped
void Socket::dropMalformedDatagram(const HifiSockAddr& senderSockAddr, qint64 size) {
    static const auto LOG_INTERVAL = std::chrono::seconds(10);
    ++_malformedDatagramsDropped;
    auto now = std::chrono::steady_clock::now();
    if (now - _lastMalformedDatagramLog >= LOG_INTERVAL) {
        qCDebug(networking) << "Dropped" << _malformedDatagramsDropped
            << "datagram(s) too short for a packet header; the last from" << senderSockAddr << "with" << size << "bytes";
        _malformedDatagramsDropped = 0;
        _lastMalformedDatagramLog = now;
    }
}

void Socket::connectToSendSignal(const HifiSockAddr& destinationAddr, QObject* receiver, const char* slot) {
    Lock connectionsLock(_connectionsHashMutex);
    auto it = _connectionsHash.find(destinationAddr);
    if (it != _connectionsHash.end()) {
        connect(it->second.get(), SIGNAL(packetSent()), receiver, slot);
    }
}

void Socket::setCongestionControlFactory(std::unique_ptr<CongestionControlVirtualFactory> ccFactory) {
    // swap the current unique_ptr for the new factory
    _ccFactory.swap(ccFactory);
}


void Socket::setConnectionMaxBandwidth(int maxBandwidth) {
    qInfo() << "Setting socket's maximum bandwith to" << maxBandwidth << "bps. ("
            << _connectionsHash.size() << "live connections)";
    _maxBandwidth = maxBandwidth;
    Lock connectionsLock(_connectionsHashMutex);
    for (auto& pair : _connectionsHash) {
        auto& connection = pair.second;
        connection->setMaxBandwidth(_maxBandwidth);
    }
}

ConnectionStats::Stats Socket::sampleStatsForConnection(const HifiSockAddr& destination) {
    auto it = _connectionsHash.find(destination);
    if (it != _connectionsHash.end()) {
        return it->second->sampleStats();
    } else {
        return ConnectionStats::Stats();
    }
}

Socket::StatsVector Socket::sampleStatsForAllConnections() {
    StatsVector result;
    Lock connectionsLock(_connectionsHashMutex);

    result.reserve(_connectionsHash.size());
    for (const auto& connectionPair : _connectionsHash) {
        result.emplace_back(connectionPair.first, connectionPair.second->sampleStats());
    }
    return result;
}


std::vector<HifiSockAddr> Socket::getConnectionSockAddrs() {
    std::vector<HifiSockAddr> addr;
    Lock connectionsLock(_connectionsHashMutex);

    addr.reserve(_connectionsHash.size());

    for (const auto& connectionPair : _connectionsHash) {
        addr.push_back(connectionPair.first);
    }
    return addr;
}

void Socket::handleSocketError(QAbstractSocket::SocketError socketError) {
    int wsaError = 0;
    static std::atomic<int> previousWsaError(0);
#ifdef WIN32
    wsaError = WSAGetLastError();
#endif
    int pending = _udpSocket.bytesToWrite();
    QString errorString;
    QDebug(&errorString) << "udt::Socket (" << _udpSocket.state() << ") error - " << wsaError << socketError <<
        "(" << _udpSocket.errorString() << ")" << (pending ? "pending bytes:" : "pending:")
        << pending;

    if (previousWsaError.exchange(wsaError) != wsaError) {
        qCDebug(networking).noquote() << errorString;
#ifdef DEBUG_EVENT_QUEUE
        int nodeListQueueSize = ::hifi::qt::getEventQueueSize(thread());
        qCDebug(networking) << "Networking queue size - " << nodeListQueueSize;
#endif  // DEBUG_EVENT_QUEUE
    } else {
        HIFI_FCDEBUG(networking(), errorString.toLatin1().constData());
    }
}

void Socket::handleStateChanged(QAbstractSocket::SocketState socketState) {
    if (socketState != QAbstractSocket::BoundState) {
        qCDebug(networking) << "udt::Socket state changed - state is now" << socketState;
    }
}

void Socket::handleRemoteAddressChange(HifiSockAddr previousAddress, HifiSockAddr currentAddress) {
    {
        Lock connectionsLock(_connectionsHashMutex);

        const auto connectionIter = _connectionsHash.find(previousAddress);
        // Don't move classes that are unused so far.
        if (connectionIter != _connectionsHash.end() && connectionIter->second->hasReceivedHandshake()) {
            auto connection = move(connectionIter->second);
            _connectionsHash.erase(connectionIter);
            connection->setDestinationAddress(currentAddress);
            _connectionsHash[currentAddress] = move(connection);
            connectionsLock.unlock();
            qCDebug(networking) << "Moved Connection class from" << previousAddress << "to" << currentAddress;

            Lock sequenceNumbersLock(_unreliableSequenceNumbersMutex);
            const auto sequenceNumbersIter = _unreliableSequenceNumbers.find(previousAddress);
            if (sequenceNumbersIter != _unreliableSequenceNumbers.end()) {
                auto sequenceNumbers = sequenceNumbersIter->second;
                _unreliableSequenceNumbers.erase(sequenceNumbersIter);
                _unreliableSequenceNumbers[currentAddress] = sequenceNumbers;
            }

        }
    }
}

#if (PR_BUILD || DEV_BUILD)

void Socket::sendFakedHandshakeRequest(const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendHandshakeRequest();
    }
}

#endif
