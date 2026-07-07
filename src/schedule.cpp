#include "schedule.h"
#include "json.hpp"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <cctype>

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

std::wstring TimeOfDay::format() const {
    wchar_t buf[8];
    swprintf(buf, 8, L"%02d:%02d", hour, minute);
    return buf;
}

TimeOfDay TimeOfDay::parse(const std::string& hhmm) {
    TimeOfDay t;
    if (hhmm.size() >= 5 &&
        std::isdigit((unsigned char)hhmm[0]) &&
        std::isdigit((unsigned char)hhmm[1]) &&
        hhmm[2] == ':' &&
        std::isdigit((unsigned char)hhmm[3]) &&
        std::isdigit((unsigned char)hhmm[4])) {
        t.hour = std::stoi(hhmm.substr(0, 2));
        t.minute = std::stoi(hhmm.substr(3, 2));
    }
    if (t.hour < 0 || t.hour > 23) t.hour = 0;
    if (t.minute < 0 || t.minute > 59) t.minute = 0;
    return t;
}

static ExamSchedule parseSchedule(const hyo::json::Value& v) {
    ExamSchedule sc;
    sc.id = utf8ToWide(v["id"].asString());
    sc.name = utf8ToWide(v["name"].asString());
    sc.grade = utf8ToWide(v["grade"].asString());

    for (auto& pv : v["periods"].arr) {
        Period p;
        p.label = utf8ToWide(pv["label"].asString());
        p.subject = utf8ToWide(pv["subject"].asString());
        p.durationMinutes = (int)pv["durationMinutes"].asNumber();
        p.start = TimeOfDay::parse(pv["start"].asString());
        p.end = TimeOfDay::parse(pv["end"].asString());
        sc.periods.push_back(p);
    }
    for (auto& bv : v["breaks"].arr) {
        BreakSlot b;
        b.start = TimeOfDay::parse(bv["start"].asString());
        b.end = TimeOfDay::parse(bv["end"].asString());
        b.note = utf8ToWide(bv["note"].asString());
        sc.breaks.push_back(b);
    }
    for (auto& nv : v["notices"].arr) {
        sc.notices.push_back(utf8ToWide(nv.asString()));
    }
    return sc;
}

static hyo::json::Value dumpSchedule(const ExamSchedule& sc) {
    using namespace hyo::json;
    Value v = Value::makeObject();
    v.set("id", Value::makeString(wideToUtf8(sc.id)));
    v.set("name", Value::makeString(wideToUtf8(sc.name)));
    v.set("grade", Value::makeString(wideToUtf8(sc.grade)));

    Value periods = Value::makeArray();
    for (auto& p : sc.periods) {
        Value pv = Value::makeObject();
        pv.set("label", Value::makeString(wideToUtf8(p.label)));
        pv.set("subject", Value::makeString(wideToUtf8(p.subject)));
        pv.set("durationMinutes", Value::makeNumber(p.durationMinutes));
        pv.set("start", Value::makeString(wideToUtf8(p.start.format())));
        pv.set("end", Value::makeString(wideToUtf8(p.end.format())));
        periods.arr.push_back(pv);
    }
    v.set("periods", periods);

    Value breaks = Value::makeArray();
    for (auto& b : sc.breaks) {
        Value bv = Value::makeObject();
        bv.set("start", Value::makeString(wideToUtf8(b.start.format())));
        bv.set("end", Value::makeString(wideToUtf8(b.end.format())));
        bv.set("note", Value::makeString(wideToUtf8(b.note)));
        breaks.arr.push_back(bv);
    }
    v.set("breaks", breaks);

    Value notices = Value::makeArray();
    for (auto& n : sc.notices) notices.arr.push_back(Value::makeString(wideToUtf8(n)));
    v.set("notices", notices);

    return v;
}

void ScheduleStore::fromJson(const hyo::json::Value& root) {
    schedules_.clear();
    for (auto& sv : root["schedules"].arr) {
        schedules_.push_back(parseSchedule(sv));
    }
    activeId_ = utf8ToWide(root["activeId"].asString());
    bool activeExists = false;
    for (const auto& sc : schedules_) {
        if (sc.id == activeId_) { activeExists = true; break; }
    }
    if (!activeExists && !schedules_.empty()) activeId_ = schedules_[0].id;
}

hyo::json::Value ScheduleStore::toJson() const {
    using namespace hyo::json;
    Value root = Value::makeObject();
    root.set("activeId", Value::makeString(wideToUtf8(activeId_)));
    Value arr = Value::makeArray();
    for (auto& sc : schedules_) arr.arr.push_back(dumpSchedule(sc));
    root.set("schedules", arr);
    return root;
}

bool ScheduleStore::loadFromFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    fromJson(hyo::json::parse(ss.str()));
    return true;
}

bool ScheduleStore::saveToFile(const std::wstring& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::string text = hyo::json::dump(toJson());
    f.write(text.data(), (std::streamsize)text.size());
    return true;
}

const ExamSchedule* ScheduleStore::find(const std::wstring& id) const {
    for (auto& sc : schedules_) if (sc.id == id) return &sc;
    return schedules_.empty() ? nullptr : &schedules_[0];
}

ScheduleStatus evaluate(const ExamSchedule& schedule, int nowMinutes) {
    ScheduleStatus st;
    for (auto& p : schedule.periods) {
        if (nowMinutes >= p.start.totalMinutes() && nowMinutes < p.end.totalMinutes()) {
            st.inPeriod = true;
            st.currentPeriod = &p;
        }
        if (p.start.totalMinutes() > nowMinutes && (!st.nextPeriod || p.start.totalMinutes() < st.nextPeriod->start.totalMinutes())) {
            st.nextPeriod = &p;
        }
    }
    for (auto& b : schedule.breaks) {
        if (nowMinutes >= b.start.totalMinutes() && nowMinutes < b.end.totalMinutes()) {
            st.inBreak = true;
            st.currentBreak = &b;
        }
    }
    return st;
}

} // namespace hyo
