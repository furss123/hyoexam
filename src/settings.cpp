#include "settings.h"
#include "json.hpp"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shell32.lib")

namespace hyo {

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Settings::settingsPath() {
    PWSTR appData = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = appData;
        CoTaskMemFree(appData);
    }
    dir += L"\\HyoExam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.json";
}

bool Settings::load() {
    std::ifstream f(settingsPath(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    auto root = hyo::json::parse(ss.str());

    std::string themeStr = root["theme"].asString("auto");
    theme = themeStr == "light" ? Theme::Light : themeStr == "dark" ? Theme::Dark : Theme::Auto;
    fontScale = (float)root["fontScale"].asNumber(1.0);
    timeSourceIndex = (int)root["timeSourceIndex"].asNumber(0);
    activeScheduleId = utf8ToWide(root["activeScheduleId"].asString());
    return true;
}

bool Settings::save() const {
    using namespace hyo::json;
    Value root = Value::makeObject();
    root.set("theme", Value::makeString(theme == Theme::Light ? "light" : theme == Theme::Auto ? "auto" : "dark"));
    root.set("fontScale", Value::makeNumber(fontScale));
    root.set("timeSourceIndex", Value::makeNumber(timeSourceIndex));
    root.set("activeScheduleId", Value::makeString(wideToUtf8(activeScheduleId)));

    std::ofstream f(settingsPath(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::string text = hyo::json::dump(root);
    f.write(text.data(), (std::streamsize)text.size());
    return true;
}

} // namespace hyo
