#include "update.h"
#include "json.hpp"

#include <urlmon.h>
#include <bcrypt.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr wchar_t kManifestUrl[] = L"https://hyot.dev/updates/hyoexam.json";

std::wstring tempDir() {
    wchar_t base[MAX_PATH]{};
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"HyoT\\hyoexam-updates\\";
    CreateDirectoryW((std::wstring(base) + L"HyoT").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string readFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::string narrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(ch <= 0x7f ? (char)ch : '?');
    return out;
}

std::vector<int> versionParts(const std::wstring& value) {
    std::vector<int> out;
    std::wstring token;
    for (wchar_t ch : value) {
        if (ch == L'v' || ch == L'V') continue;
        if (ch == L'.') {
            out.push_back(token.empty() ? 0 : _wtoi(token.c_str()));
            token.clear();
            continue;
        }
        if (ch == L'-' || ch == L'+') break;
        if (ch >= L'0' && ch <= L'9') token.push_back(ch);
    }
    out.push_back(token.empty() ? 0 : _wtoi(token.c_str()));
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool newerThan(const std::wstring& latest, const std::wstring& current) {
    auto a = versionParts(latest);
    auto b = versionParts(current);
    for (size_t i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

std::string sha256File(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLen = 0, hashLen = 0, cb = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectLen, sizeof(objectLen), &cb, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
    std::vector<UCHAR> object(objectLen), digest(hashLen);
    if (BCryptCreateHash(alg, &hash, object.data(), objectLen, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return "";
    }

    char buffer[64 * 1024];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        auto count = file.gcount();
        if (count > 0) BCryptHashData(hash, (PUCHAR)buffer, (ULONG)count, 0);
    }
    BCryptFinishHash(hash, digest.data(), hashLen, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (auto b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
}

} // namespace

void StartUpdateCheck(HWND hwnd, const wchar_t* currentVersion) {
    std::thread([hwnd, current = std::wstring(currentVersion)] {
        try {
            auto dir = tempDir();
            auto manifestPath = dir + L"manifest.json";
            if (URLDownloadToFileW(nullptr, kManifestUrl, manifestPath.c_str(), 0, nullptr) != S_OK) return;

            auto root = hyo::json::parse(readFileUtf8(manifestPath));
            auto latest = widen(root["latest"]["stable"].asString());
            if (latest.empty() || !newerThan(latest, current)) return;

            const std::string latestUtf8 = narrowAscii(latest);
            const hyo::json::Value* release = nullptr;
            for (const auto& item : root["releases"].arr) {
                if (item["version"].asString() == latestUtf8) {
                    release = &item;
                    break;
                }
            }
            if (!release) return;
            const auto& asset = (*release)["primaryAsset"];
            auto url = widen(asset["url"].asString());
            auto filename = widen(asset["filename"].asString());
            auto expectedSha = asset["sha256"].asString();
            if (url.empty() || filename.empty()) return;

            auto assetPath = dir + filename;
            if (URLDownloadToFileW(nullptr, url.c_str(), assetPath.c_str(), 0, nullptr) != S_OK) return;
            if (!expectedSha.empty() && sha256File(assetPath) != expectedSha) {
                DeleteFileW(assetPath.c_str());
                return;
            }

            auto* update = new PreparedUpdate{ latest, assetPath };
            if (!PostMessageW(hwnd, WM_HYO_UPDATE_READY, 0, reinterpret_cast<LPARAM>(update))) {
                delete update;
            }
        } catch (...) {
        }
    }).detach();
}
