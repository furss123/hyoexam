// Persisted user preferences: theme, font scale, active exam schedule id.
// Stored as JSON under %APPDATA%\HyoExam\settings.json so a reinstall/update
// doesn't wipe the user's choices.
#pragma once
#include <string>

namespace hyo {

enum class Theme { Dark, Light, Auto };

struct Settings {
    Theme theme = Theme::Auto; // default: follow the Windows light/dark setting
    float fontScale = 1.0f;       // 0.8..1.5
    float splitRatio = 0.70f;     // left (clock) panel share of the content width
    std::wstring activeScheduleId;

    static std::wstring settingsPath();
    bool load();
    bool save() const;
};

} // namespace hyo
