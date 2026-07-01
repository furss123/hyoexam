// Exam schedule data model: an exam type (모의고사/내신/custom) owns an ordered
// list of periods and break announcements for "today". Loaded from data/schedules.json.
#pragma once
#include <string>
#include <vector>
#include <optional>

namespace hyo {

// Minutes since local midnight, e.g. 08:40 -> 520.
struct TimeOfDay {
    int hour = 0;
    int minute = 0;
    int totalMinutes() const { return hour * 60 + minute; }
    std::wstring format() const;
    static TimeOfDay parse(const std::string& hhmm); // "08:40"
};

struct Period {
    std::wstring label;       // "1교시", "한국사"
    std::wstring subject;     // "국어"
    int durationMinutes = 0;  // 80
    TimeOfDay start;
    TimeOfDay end;
};

struct BreakSlot {
    TimeOfDay start;
    TimeOfDay end;
    std::wstring note; // "한국사 문답지 회수, 사탐 문답지 배부"
};

struct ExamSchedule {
    std::wstring id;      // "mock_exam", "midterm"
    std::wstring name;    // "모의고사", "중간고사"
    std::wstring grade;   // "3학년" (optional label)
    std::vector<Period> periods;
    std::vector<BreakSlot> breaks;
    std::vector<std::wstring> notices; // fixed bottom banner lines
};

class ScheduleStore {
public:
    bool loadFromFile(const std::wstring& path);
    bool saveToFile(const std::wstring& path) const;

    const std::vector<ExamSchedule>& all() const { return schedules_; }
    const ExamSchedule* find(const std::wstring& id) const;
    void setActive(const std::wstring& id) { activeId_ = id; }
    const std::wstring& activeId() const { return activeId_; }
    const ExamSchedule* active() const { return find(activeId_); }
    ExamSchedule* activeMutable() {
        for (auto& sc : schedules_) if (sc.id == activeId_) return &sc;
        return schedules_.empty() ? nullptr : &schedules_[0];
    }

private:
    std::vector<ExamSchedule> schedules_;
    std::wstring activeId_;
};

// Given "now" as minutes-since-midnight, find current period / break / status text.
struct ScheduleStatus {
    bool inPeriod = false;
    bool inBreak = false;
    const Period* currentPeriod = nullptr;
    const BreakSlot* currentBreak = nullptr;
    const Period* nextPeriod = nullptr;
};

ScheduleStatus evaluate(const ExamSchedule& schedule, int nowMinutes);

} // namespace hyo
