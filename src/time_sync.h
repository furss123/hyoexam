// Keeps a running offset between the local system clock and a real time
// server (SNTP, UDP/123), refreshed on a background thread every 3s so the
// on-screen clock tracks an authoritative source instead of a possibly-
// drifted local clock. start() also does one blocking fetch before returning,
// so the app opens already synced instead of showing "동기화 대기중" for the
// first frame. Falls back silently to the system clock whenever a sync
// request fails (no network, DNS hiccup, etc). The server host can be
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
    // (within ~3s) and immediately marks the offset as unsynced.
    void setHost(const std::wstring& host);

    // Forces an immediate re-sync of the current host (the toolbar refresh
    // button): marks unsynced and wakes the worker to fetch right away instead
    // of waiting out the remaining slice of the 3s interval.
    void refresh();

    // Current wall-clock time = system time + last-known offset to the server.
    std::chrono::system_clock::time_point now() const;

    bool isSynced() const { return synced_.load(std::memory_order_relaxed); }
    long long offsetMillis() const { return offsetMs_.load(std::memory_order_relaxed); }

    // One-shot SNTP probe against an arbitrary host, independent of any
    // TimeSync instance's state. Public/static so the app can race several
    // candidate hosts at startup (see raceTimeSources() in main.cpp) and adopt
    // whichever answers first, before committing to a single TimeSync instance.
    static bool fetchSntpOffset(const std::wstring& host, long long& outOffsetMs);

private:
    void run();
    std::wstring currentHost();

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> synced_{false};
    std::atomic<bool> forceFetch_{false}; // set by refresh(); breaks the worker's sleep for an immediate fetch
    std::atomic<long long> offsetMs_{0};

    mutable std::mutex hostMutex_;
    std::wstring host_;
};

} // namespace hyo
