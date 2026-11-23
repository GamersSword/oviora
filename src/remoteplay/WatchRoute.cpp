// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/remoteplay/WatchRoute.h"

namespace oviora::remoteplay {

void WatchRoute::pushRemote(std::string json) {
    {
        std::lock_guard lock(mutex);
        remoteQueue_.push_back(std::move(json));
    }
    cv.notify_all();
}

bool WatchRoute::popRemote(std::string& out) {
    std::lock_guard lock(mutex);
    if (remoteQueue_.empty()) return false;
    out = std::move(remoteQueue_.front());
    remoteQueue_.pop_front();
    return true;
}

void WatchRoute::setEnded(std::string reason) {
    {
        std::lock_guard lock(mutex);
        endReason = std::move(reason);
        ended.store(true, std::memory_order_release);
    }
    cv.notify_all();
}

std::string WatchRoute::getEndReason() const {
    std::lock_guard lock(mutex);
    return endReason;
}

} // namespace oviora::remoteplay
