//
//  TCPVegasCC.h
//  libraries/networking/src/udt
//
//  Created by Stephen Birarda on 2016-09-20.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_TCPVegasCC_h
#define hifi_TCPVegasCC_h

#include <algorithm>
#include <map>

#include "CongestionControl.h"
#include "Constants.h"

namespace udt {
    

class TCPVegasCC : public CongestionControl {
public:
    // PHYSICS.md round 10: the retransmission timeout never falls below this (Linux TCP's floor, 200 ms); before, it was
    // the smoothed round trip plus four deviations with no floor, tens of microseconds on this machine, so one
    // acknowledgement later than that (a busy receiver, a public address reached through the router) retransmitted
    // every packet in flight, which made every later acknowledgement a duplicate and Karn's rule left the round-trip
    // estimate empty, for ever: the congestion window sat at two, every packet went out six to twenty-seven times
    static constexpr int MIN_RTO_USECS = 200000;
    // round trips under a millisecond are scheduling noise, not queueing: Vegas's comparison of the round trip with
    // its base is made with both floored to this
    static constexpr int VEGAS_RTT_FLOOR_USECS = 1000;
    // the base round trip is the least seen over a sliding window (two buckets of this length), as BBR's min_rtt
    // is, not the least ever: a receiver that answers slower than it once did is not a queue
    static constexpr int BASE_RTT_BUCKET_USECS = 5000000;

    TCPVegasCC();

    virtual bool onACK(SequenceNumber ackNum, p_high_resolution_clock::time_point receiveTime) override;
    virtual void onTimeout() override {};

    virtual void onPacketSent(int wireSize, SequenceNumber seqNum, p_high_resolution_clock::time_point timePoint) override;
    virtual void onPacketReSent(int wireSize, SequenceNumber seqNum, p_high_resolution_clock::time_point timePoint) override;

    virtual int estimatedTimeout() const override;
    
protected:
    virtual void performCongestionAvoidance(SequenceNumber ack);
    virtual void setInitialSendSequenceNumber(SequenceNumber seqNum) override { _lastACK = seqNum - 1; }
private:
    bool calculateRTT(p_high_resolution_clock::time_point sendTime, p_high_resolution_clock::time_point receiveTime);
    bool needsFastRetransmit(SequenceNumber ack, bool wasDuplicateACK);

    bool isCongestionWindowLimited();
    void performRenoCongestionAvoidance(SequenceNumber ack);

    struct SentPacketData {
        SentPacketData(SequenceNumber seqNum, p_high_resolution_clock::time_point tPoint)
            : sequenceNumber(seqNum), timePoint(tPoint) {};
        
        SequenceNumber sequenceNumber;
        p_high_resolution_clock::time_point timePoint;
        bool wasResent { false };
    };

    using PacketTimeList = std::vector<SentPacketData>;
    PacketTimeList _sentPacketDatas; // association of sequence numbers to sent time, for RTT calc

    p_high_resolution_clock::time_point _lastAdjustmentTime; // Time of last congestion control adjustment

    bool _slowStart { true }; // Marker for slow start phase

    SequenceNumber _lastACK; // Sequence number of last packet that was ACKed

    int _numACKSinceFastRetransmit { 3 }; // Number of ACKs received since fast re-transmit, default avoids immediate re-transmit

    int baseRTT() const { return std::min(_baseBucketMin[0], _baseBucketMin[1]); }
    void resetBaseRTT();
    void noteBaseRTT(int rtt, p_high_resolution_clock::time_point now);

    int _currentMinRTT; // Current min RTT during last RTT (since last congestion avoidance check), in microseconds
    int _baseBucketMin[2]; // Lowest RTT in the current and the previous bucket of the sliding window, in microseconds
    p_high_resolution_clock::time_point _baseBucketStart;
    int _ewmaRTT { -1 }; // Exponential weighted moving average RTT
    int _rttVariance { 0 }; // Variance in collected RTT values

    int _numRTTs { 0 }; // Number of RTTs calculated during the last RTT (since last performed congestion avoidance)

    int _ackAICount { 0 }; // Counter for number of ACKs received for Reno additive increase
    int _duplicateACKCount { 0 }; // Counter for duplicate ACKs received

    int _slowStartOddAdjust { 0 }; // Marker for every window adjustment every other RTT in slow-start

    // the trace (TIVOLI_UDT_TRACE)
    int _traceCount { 0 };
    int _traceLastSample { -1 };
    int _traceBaseRTT { -1 };
    int _traceSinceSend { -1 };
};

}





#endif // hifi_TCPVegasCC_h
