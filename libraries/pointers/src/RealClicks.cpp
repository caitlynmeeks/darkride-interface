//
//  RealClicks.cpp
//  libraries/pointers/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "RealClicks.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    struct Press {
        QUuid entityID;
        Clock::time_point when;
    };

    // a handful at most: a press is forgotten when it's older than any caller may use (the longest maxAge asked for
    // is a few seconds; the list is pruned on every note and take)
    const size_t MOST_KEPT = 16;
    const auto KEPT_FOR = std::chrono::seconds(10);

    std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    std::vector<Press>& presses() {
        static std::vector<Press> p;
        return p;
    }
    RealClicks::OwnershipCheck& ownershipCheck() {
        static RealClicks::OwnershipCheck check;
        return check;
    }

    void prune(Clock::time_point now) {
        auto& list = presses();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Press& press) {
            return now - press.when > KEPT_FOR;
        }), list.end());
        while (list.size() > MOST_KEPT) {
            list.erase(list.begin());
        }
    }
}

void RealClicks::note(const QUuid& entityID) {
    if (entityID.isNull()) {
        return;
    }
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex());
    prune(now);
    // one entry per entity: the latest press
    auto& list = presses();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Press& press) {
        return press.entityID == entityID;
    }), list.end());
    list.push_back({ entityID, now });
}

void RealClicks::setOwnershipCheck(OwnershipCheck check) {
    std::lock_guard<std::mutex> lock(mutex());
    ownershipCheck() = std::move(check);
}

bool RealClicks::takeForCaller(std::chrono::milliseconds maxAge) {
    const auto now = Clock::now();
    std::vector<QUuid> candidates;
    OwnershipCheck check;
    {
        std::lock_guard<std::mutex> lock(mutex());
        prune(now);
        check = ownershipCheck();
        for (const Press& press : presses()) {
            if (now - press.when <= maxAge) {
                candidates.push_back(press.entityID);
            }
        }
    }
    if (!check) {
        return false;
    }
    // the ownership check may take locks of its own (the entity tree): outside ours
    for (const QUuid& entityID : candidates) {
        if (check(entityID)) {
            std::lock_guard<std::mutex> lock(mutex());
            auto& list = presses();
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Press& press) {
                return press.entityID == entityID;
            }), list.end());
            return true;
        }
    }
    return false;
}

void RealClicks::clear() {
    std::lock_guard<std::mutex> lock(mutex());
    presses().clear();
}
