//
//  NodeAddresses.h
//  domain-server/src
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Whether a packet comes from one of this domain's nodes, for the queries only a connected app or assignment client
//  makes (a path's viewpoint, the domain's settings), and a once-a-minute log line for those not answered
//  (security #60).
//

#ifndef hifi_NodeAddresses_h
#define hifi_NodeAddresses_h

#include <atomic>

#include <QtCore/QDebug>

#include <DependencyManager.h>
#include <HifiSockAddr.h>
#include <LimitedNodeList.h>
#include <NumericalConstants.h>
#include <SharedUtil.h>

#include "DomainServerNodeData.h"

namespace node_addresses {

/// A node is known by the address its connect request came from (an app or assignment client on this machine has no
/// public address on record, and a NAT may change the port), or by an address it gave (public, local, symmetric).
inline bool isFromNode(const HifiSockAddr& sender) {
    auto nodeList = DependencyManager::get<LimitedNodeList>();
    bool fromNode = false;
    nodeList->eachNodeBreakable([&](const SharedNodePointer& node) {
        auto nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());
        fromNode = nodeData && nodeData->getSendingSockAddr() == sender;
        return !fromNode;
    });
    return fromNode || nodeList->findNodeWithAddr(sender);
}

/// One line a minute per kind of query, with how many weren't answered since the last line.
class UnansweredLog {
public:
    explicit UnansweredLog(const char* what) : _what(what) {}
    void note(const HifiSockAddr& sender) {
        const int count = ++_sinceLog;
        const quint64 now = usecTimestampNow();
        quint64 last = _lastLogUsecs.load();
        if ((last == 0 || now - last >= 60 * USECS_PER_SECOND) && _lastLogUsecs.compare_exchange_strong(last, now)) {
            _sinceLog -= count;
            qDebug() << "Not answering a" << _what << "from" << sender << "which isn't a node of this domain ("
                     << count << "since the last line)";
        }
    }

private:
    const char* _what;
    std::atomic<quint64> _lastLogUsecs { 0 };
    std::atomic<int> _sinceLog { 0 };
};

}

#endif // hifi_NodeAddresses_h
