#include "time_sync.h"
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace hyo {

namespace {
    constexpr auto kSyncInterval = std::chrono::seconds(10);
    constexpr auto kRequestTimeoutMs = 4000;
}

TimeSync::TimeSync() = default;
TimeSync::~TimeSync() { stop(); }

void TimeSync::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run(); });
}

void TimeSync::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void TimeSync::run() {
    while (running_.load(std::memory_order_relaxed)) {
        long long offset = 0;
        if (fetchNaverOffset(offset)) {
            offsetMs_.store(offset, std::memory_order_relaxed);
            synced_.store(true, std::memory_order_relaxed);
        }
        // Sleep in short slices so stop() reacts quickly instead of blocking ~10s.
        for (int i = 0; i < 100 && running_.load(std::memory_order_relaxed); i++) {
            std::this_thread::sleep_for(kSyncInterval / 100);
        }
    }
}

bool TimeSync::fetchNaverOffset(long long& outOffsetMs) {
    bool ok = false;
    HINTERNET hSession = WinHttpOpen(L"HyoExam/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, kRequestTimeoutMs, kRequestTimeoutMs, kRequestTimeoutMs, kRequestTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.naver.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", L"/",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hRequest) {
            FILETIME requestSentUtc;
            GetSystemTimeAsFileTime(&requestSentUtc);

            BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
                wchar_t dateBuf[128] = {0};
                DWORD size = sizeof(dateBuf);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_DATE, WINHTTP_HEADER_NAME_BY_INDEX,
                    dateBuf, &size, WINHTTP_NO_HEADER_INDEX)) {
                    SYSTEMTIME serverSt{};
                    if (WinHttpTimeToSystemTime(dateBuf, &serverSt)) {
                        FILETIME serverFt{};
                        SystemTimeToFileTime(&serverSt, &serverFt);

                        FILETIME responseUtc;
                        GetSystemTimeAsFileTime(&responseUtc);

                        ULARGE_INTEGER sentU, respU, servU;
                        sentU.LowPart = requestSentUtc.dwLowDateTime; sentU.HighPart = requestSentUtc.dwHighDateTime;
                        respU.LowPart = responseUtc.dwLowDateTime; respU.HighPart = responseUtc.dwHighDateTime;
                        servU.LowPart = serverFt.dwLowDateTime; servU.HighPart = serverFt.dwHighDateTime;

                        // Assume the server timestamp lands at the midpoint of the round trip.
                        ULONGLONG midpoint100ns = (sentU.QuadPart + respU.QuadPart) / 2;
                        long long diff100ns = (long long)(servU.QuadPart - midpoint100ns);
                        outOffsetMs = diff100ns / 10000; // 100ns -> ms
                        ok = true;
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

std::chrono::system_clock::time_point TimeSync::now() const {
    auto sys = std::chrono::system_clock::now();
    return sys + std::chrono::milliseconds(offsetMs_.load(std::memory_order_relaxed));
}

} // namespace hyo
