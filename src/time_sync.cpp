#include "time_sync.h"

// winsock2.h must come before windows.h in this translation unit.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

namespace hyo {

namespace {
    constexpr auto kSyncInterval = std::chrono::seconds(3);
    constexpr DWORD kRequestTimeoutMs = 3000;
    constexpr double kNtpToUnixEpochSeconds = 2208988800.0;   // 1900 -> 1970
    constexpr double kUnixToFileTimeEpochSeconds = 11644473600.0; // 1970 -> 1601

    struct WinsockInit {
        WinsockInit() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
        ~WinsockInit() { WSACleanup(); }
    };
}

TimeSync::TimeSync() : host_(L"time.navyism.com") {}
TimeSync::~TimeSync() { stop(); }

void TimeSync::setHost(const std::wstring& host) {
    std::lock_guard<std::mutex> lock(hostMutex_);
    if (host_ != host) {
        host_ = host;
        synced_.store(false, std::memory_order_relaxed);
    }
}

std::wstring TimeSync::currentHost() {
    std::lock_guard<std::mutex> lock(hostMutex_);
    return host_;
}

void TimeSync::start() {
    if (running_.exchange(true)) return;
    // One blocking fetch before the background loop starts, so the app opens
    // already synced instead of showing "동기화 대기중" for the first tick.
    // Bounded by kRequestTimeoutMs (3s) on failure/no network.
    long long offset = 0;
    if (fetchSntpOffset(currentHost(), offset)) {
        offsetMs_.store(offset, std::memory_order_relaxed);
        synced_.store(true, std::memory_order_relaxed);
    }
    worker_ = std::thread([this] { run(); });
}

void TimeSync::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void TimeSync::run() {
    static WinsockInit wsInit; // one-time WSAStartup/WSACleanup for the process
    while (running_.load(std::memory_order_relaxed)) {
        long long offset = 0;
        if (fetchSntpOffset(currentHost(), offset)) {
            offsetMs_.store(offset, std::memory_order_relaxed);
            synced_.store(true, std::memory_order_relaxed);
        }
        // Sleep in short slices so stop() reacts quickly instead of blocking ~10s.
        for (int i = 0; i < 100 && running_.load(std::memory_order_relaxed); i++) {
            std::this_thread::sleep_for(kSyncInterval / 100);
        }
    }
}

bool TimeSync::fetchSntpOffset(const std::wstring& host, long long& outOffsetMs) {
    bool ok = false;

    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    ADDRINFOW* result = nullptr;
    if (GetAddrInfoW(host.c_str(), L"123", &hints, &result) != 0 || !result) return false;

    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock != INVALID_SOCKET) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&kRequestTimeoutMs, sizeof(kRequestTimeoutMs));

        unsigned char packet[48] = {0};
        packet[0] = 0x1B; // LI=0, VN=3, Mode=3 (client)

        FILETIME sentUtc;
        GetSystemTimeAsFileTime(&sentUtc);

        int sent = sendto(sock, (const char*)packet, sizeof(packet), 0, result->ai_addr, (int)result->ai_addrlen);
        if (sent == sizeof(packet)) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            unsigned char response[48] = {0};
            int received = recvfrom(sock, (char*)response, sizeof(response), 0, (sockaddr*)&from, &fromLen);
            if (received == sizeof(response)) {
                FILETIME recvUtc;
                GetSystemTimeAsFileTime(&recvUtc);

                uint32_t ntpSeconds = (uint32_t(response[40]) << 24) | (uint32_t(response[41]) << 16) |
                    (uint32_t(response[42]) << 8) | uint32_t(response[43]);
                uint32_t ntpFraction = (uint32_t(response[44]) << 24) | (uint32_t(response[45]) << 16) |
                    (uint32_t(response[46]) << 8) | uint32_t(response[47]);

                double serverUnixSeconds = (double)ntpSeconds - kNtpToUnixEpochSeconds + (double)ntpFraction / 4294967296.0;
                double serverFileTimeTicks = (serverUnixSeconds + kUnixToFileTimeEpochSeconds) * 10000000.0;

                ULARGE_INTEGER sentU, recvU;
                sentU.LowPart = sentUtc.dwLowDateTime; sentU.HighPart = sentUtc.dwHighDateTime;
                recvU.LowPart = recvUtc.dwLowDateTime; recvU.HighPart = recvUtc.dwHighDateTime;

                // Assume the server timestamp lands at the midpoint of the round trip.
                double midpointTicks = ((double)sentU.QuadPart + (double)recvU.QuadPart) / 2.0;
                double diffTicks = serverFileTimeTicks - midpointTicks;
                outOffsetMs = (long long)(diffTicks / 10000.0);
                ok = true;
            }
        }
        closesocket(sock);
    }
    FreeAddrInfoW(result);
    return ok;
}

std::chrono::system_clock::time_point TimeSync::now() const {
    auto sys = std::chrono::system_clock::now();
    return sys + std::chrono::milliseconds(offsetMs_.load(std::memory_order_relaxed));
}

} // namespace hyo
