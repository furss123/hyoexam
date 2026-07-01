// Keeps a running offset between the local system clock and the time reported
// by the HTTP "Date" header from www.naver.com, refreshed on a background
// thread every 10s so on-screen time tracks "네이버 시간" instead of a
// possibly-drifted local clock. Falls back silently to the system clock
// whenever a sync request fails (no network, DNS hiccup, etc).
#pragma once
#include <atomic>
#include <thread>
#include <chrono>

namespace hyo {

class TimeSync {
public:
    TimeSync();
    ~TimeSync();

    void start();
    void stop();

    // Current wall-clock time = system time + last-known offset to Naver.
    std::chrono::system_clock::time_point now() const;

    bool isSynced() const { return synced_.load(std::memory_order_relaxed); }
    long long offsetMillis() const { return offsetMs_.load(std::memory_order_relaxed); }

private:
    void run();
    bool fetchNaverOffset(long long& outOffsetMs);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> synced_{false};
    std::atomic<long long> offsetMs_{0};
};

} // namespace hyo
