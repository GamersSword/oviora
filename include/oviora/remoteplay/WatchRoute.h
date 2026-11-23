// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace oviora::remoteplay {

struct WatchRoute {
    std::string codec;                    // "h264" | "h265"
    std::vector<std::string> iceServers;  // "stun:..." vb.
    std::function<void(std::string json)> sendSignal; // offer / {"candidate","mid"} JSON metni; herhangi bir iş parçacığından çağrılabilir
    // Claude'un tarafı uzak sinyalleri buraya iter (Qt ana iş parçacığından); alıcı kendi iş parçacığında çeker
    void pushRemote(std::string json);    // kilitli kuyruk
    bool popRemote(std::string& out);
    std::atomic<bool> ended{false};       // Claude yayın bitti/çıkarıldı derse true yapar → oturum kapanır (hata metniyle)
    std::string endReason;                // (kilitle) kullanıcıya gösterilecek Türkçe neden

    mutable std::mutex mutex;
    std::condition_variable cv;

    void setEnded(std::string reason);
    std::string getEndReason() const;

private:
    std::deque<std::string> remoteQueue_;
};

} // namespace oviora::remoteplay
