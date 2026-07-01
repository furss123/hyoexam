// Keeps a running offset between the local system clock and a real time
// server (SNTP, UDP/123), refreshed on a background thread every 10s so the
// on-screen clock tracks an authoritative source instead of a possibly-
// drifted local clock. Falls back silently to the system clock whenever a
// sync request fails (no network, DNS hiccup, etc). The server host can be
// switched at runtime (see the time-source dropdown in the toolbar).
#pragma once
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <mutex>

namespace hyo {

class TimeSync {
public:
    TimeSync();
    ~TimeSync();

    void start();
    void stop();

    // Changes which server to sync against; takes effect on the next tick
    // (within ~10s) and immediately marks the offset as unsynced.
    void setHost(const std::wstring& host);

    // Current wall-clock time = system time + last-known offset to the server.
    std::chrono::system_clock::time_point now() const;

    bool isSynced() const { return synced_.load(std::memory_order_relaxed); }
    long long offsetMillis() const { return offsetMs_.load(std::memory_order_relaxed); }

private:
    void run();
    bool fetchSntpOffset(const std::wstring& host, long long& outOffsetMs);
    std::wstring currentHost();

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> synced_{false};
    std::atomic<long long> offsetMs_{0};

    mutable std::mutex hostMutex_;
    std::wstring host_;
};

} // namespace hyo
