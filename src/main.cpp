// HyoExam — full-screen exam-day clock & timetable for TV output.
// Win32 + Direct2D/DirectWrite: no UI framework overhead, single ~1-2MB static
// binary, instant startup. See README.md for build instructions.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <ctime>
#include <cwchar>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>

#include "schedule.h"
#include "settings.h"
#include "time_sync.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using namespace hyo;

namespace {

constexpr wchar_t kAppVersion[] = L"1.0.0";
constexpr wchar_t kWindowClass[] = L"HyoExamWindowClass";
constexpr UINT_PTR kTickTimerId = 1;
constexpr UINT kTickIntervalMs = 250;

// Native child control IDs (Edit/ComboBox) used for text entry — Direct2D has
// no text-input widgets of its own, so real typing/Korean IME and dropdown
// selection go through these.
constexpr int kIdEditLabel = 101;
constexpr int kIdEditSubject = 102;
constexpr int kIdEditNotices = 105;
constexpr int kIdComboStartHour = 106;
constexpr int kIdComboStartMinute = 107;
constexpr int kIdComboEndHour = 108;
constexpr int kIdComboEndMinute = 109;
constexpr int kIdEditProfileName = 110;

// Malgun Gothic ships with every Windows 10/11 install (it's the OS's own
// Korean UI font) — no bundling, no missing-font fallback surprises.
constexpr wchar_t kUiFontFamily[] = L"맑은 고딕";

// Time servers the clock can sync against (SNTP, UDP/123). Index persisted
// in Settings::timeSourceIndex.
struct TimeSourceOption { const wchar_t* host; const wchar_t* label; const wchar_t* description; };
const TimeSourceOption kTimeSources[] = {
    { L"time.navyism.com", L"네이버 시계", L"네이버에서 제공하는 표준시" },
    { L"ntp.kriss.re.kr", L"KRISS", L"한국표준과학연구원 표준시" },
    { L"time.bora.net", L"KISA", L"한국인터넷진흥원 표준시" },
};
constexpr int kTimeSourceCount = (int)(sizeof(kTimeSources) / sizeof(kTimeSources[0]));

// ---- Brand palette (HyoT-brand-kit.md — do not introduce new hues) ----
struct Palette {
    D2D1_COLOR_F base, surface, cardBorder;
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary;
    D2D1_COLOR_F error; // theme-tuned delete/error accent (brighter on dark bg for contrast)
};

D2D1_COLOR_F hex(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
}

// Same color, different alpha — for tinting a palette color's fill vs. its solid text/icon use.
D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a) {
    return D2D1::ColorF(c.r, c.g, c.b, a);
}

constexpr UINT32 kHyoBlue = 0x4A9FE0;
constexpr UINT32 kHyoBlueDark = 0x2B7CC7;
constexpr UINT32 kHyoBlueLight = 0x7BBFED;
constexpr UINT32 kPurple = 0x8B4FCC;
constexpr UINT32 kOrange = 0xE87820;
constexpr UINT32 kTeal = 0x2A9B8A;
constexpr UINT32 kErrorDark = 0xFF7070;
constexpr UINT32 kErrorLight = 0xC42B1C;

Palette darkPalette() {
    Palette p{};
    p.base = hex(0x07090C);
    p.surface = hex(0x0D1117);
    p.cardBorder = hex(kHyoBlue, 0.18f);
    p.textPrimary = hex(0xEEF2FF);
    p.textSecondary = hex(0x8896AA);
    p.textTertiary = hex(0x4A5870);
    p.error = hex(kErrorDark);
    return p;
}

Palette lightPalette() {
    Palette p{};
    p.base = hex(0xF6F8FC);
    p.surface = hex(0xFFFFFF);
    p.cardBorder = hex(kHyoBlue, 0.25f);
    p.textPrimary = hex(0x12151C);
    p.textSecondary = hex(0x45526B);
    p.textTertiary = hex(0x8896AA);
    p.error = hex(kErrorLight);
    return p;
}

// A named, saved snapshot of the whole app: settings (theme/split/time source)
// plus every exam schedule's full data. Raw JSON values, since this is just
// stored and restored wholesale, never inspected field-by-field here.
struct SavedProfile {
    std::wstring name;
    hyo::json::Value settingsJson;
    hyo::json::Value schedulesJson;
};

// ---- App state ----
struct AppState {
    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* renderTarget = nullptr;

    IDWriteTextFormat* fmtClock = nullptr;
    IDWriteTextFormat* fmtDate = nullptr;
    IDWriteTextFormat* fmtHeading = nullptr;
    IDWriteTextFormat* fmtBody = nullptr;
    IDWriteTextFormat* fmtSmall = nullptr;
    IDWriteTextFormat* fmtVersion = nullptr;
    IDWriteTextFormat* fmtIcon = nullptr;

    // Fullscreen (TV/projector) variants of clock/date/body/small — sized off the
    // actual monitor resolution so the student-facing display fills the frame
    // instead of reusing windowed-desktop-sized text on a huge canvas.
    IDWriteTextFormat* fmtClockFS = nullptr;
    IDWriteTextFormat* fmtDateFS = nullptr;
    IDWriteTextFormat* fmtBodyFS = nullptr;
    IDWriteTextFormat* fmtSmallFS = nullptr;
    float fsFontsBuiltForHeight = -1.0f;
    float fsClockCorrectedForWidth = -1.0f;

    Settings settings;
    ScheduleStore scheduleStore;
    TimeSync timeSync;
    std::wstring dataPath;

    bool fullscreen = false;
    WINDOWPLACEMENT prevPlacement{ sizeof(WINDOWPLACEMENT) };

    // Hit-test rects for the admin toolbar, recomputed each frame it's drawn.
    D2D1_RECT_F rectFullscreenBtn{};
    D2D1_RECT_F rectThemeToggle{};
    std::vector<std::pair<D2D1_RECT_F, std::wstring>> scheduleButtons;

    // Footer site link.
    D2D1_RECT_F rectSiteLink{};
    bool hoveredSiteLink = false;

    // Time-source dropdown (right of the exam-type buttons).
    D2D1_RECT_F rectTimeSourceButton{};
    bool timeSourceDropdownOpen = false;
    D2D1_RECT_F timeSourceOptionRects[kTimeSourceCount]{};
    int hoveredTimeSourceOption = -1;

    // Layout editing: draggable divider between the clock and timetable panels.
    D2D1_RECT_F rectSplitHandle{};
    bool draggingSplit = false;
    float contentLeft = 0, contentWidth = 0;

    // Timetable editing.
    int editingPeriodIndex = -1; // -1 = closed, -2 = new period, >=0 = editing that index
    std::vector<D2D1_RECT_F> periodRowRects;
    std::vector<D2D1_RECT_F> periodDeleteRects;
    D2D1_RECT_F rectAddPeriod{};
    D2D1_RECT_F rectPeriodSave{}, rectPeriodCancel{}, rectPeriodDelete{};
    D2D1_RECT_F rectFieldLabel{}, rectFieldSubject{};
    D2D1_RECT_F rectFieldStartHour{}, rectFieldStartMinute{}, rectFieldEndHour{}, rectFieldEndMinute{};

    // Period row hover (click affordance) and drag-to-reorder.
    int hoveredPeriodRow = -1;
    bool pendingIsPeriodClick = false;
    int pendingClickPeriodIndex = -1;
    POINT mouseDownPt{};
    bool isDraggingPeriod = false;
    int draggingPeriodIndex = -1;
    std::vector<int> dragOrder; // preview order, holds original indices into active->periods
    float periodListTop = 0, periodRowHeight = 0;

    // Bottom notice text editing.
    bool editingNotices = false;
    D2D1_RECT_F rectNoticesEditIcon{};
    D2D1_RECT_F rectNoticeSave{}, rectNoticeCancel{};
    D2D1_RECT_F rectNoticesField{};

    // Named profiles: a full snapshot of settings + schedules, saved/loaded as
    // one unit (not a file picker — an in-app named-slot list).
    std::vector<SavedProfile> profiles;
    D2D1_RECT_F rectSaveIcon{}, rectLoadIcon{};
    bool savingProfile = false;
    bool loadingProfile = false;
    D2D1_RECT_F rectProfileNameField{}, rectProfileSaveBtn{}, rectProfileSaveCancel{};
    D2D1_RECT_F rectProfileLoadClose{};
    std::vector<D2D1_RECT_F> profileRowRects, profileRowDeleteRects;

    HWND hEditLabel = nullptr, hEditSubject = nullptr;
    HWND hComboStartHour = nullptr, hComboStartMinute = nullptr, hComboEndHour = nullptr, hComboEndMinute = nullptr;
    HWND hEditNotices = nullptr;
    HWND hEditProfileName = nullptr;
    HFONT hUiFont = nullptr;
};

AppState* g = nullptr;

std::wstring wideExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

// Windows exposes the user's light/dark choice via this registry value (the same
// one Explorer/Settings read) — 0 = dark, 1 = light. Missing/unreadable = dark.
bool isSystemLightTheme() {
    DWORD value = 0, size = sizeof(value);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS && value != 0;
}

bool isEffectivelyLight() {
    if (g->settings.theme == Theme::Auto) return isSystemLightTheme();
    return g->settings.theme == Theme::Light;
}

const Palette currentPalette() {
    return isEffectivelyLight() ? lightPalette() : darkPalette();
}

const wchar_t* koreanWeekday(int wday) {
    static const wchar_t* names[] = { L"일", L"월", L"화", L"수", L"목", L"금", L"토" };
    return names[wday % 7];
}

IDWriteTextFormat* makeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* fmt = nullptr;
    g->dwriteFactory->CreateTextFormat(family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-KR", &fmt);
    return fmt;
}

// Builds every UI text format from the fixed display font (kUiFontFamily —
// Malgun Gothic, always present on Windows 10/11). The footer version line
// stays JetBrains Mono and icons stay Segoe MDL2 Assets, per brand rule / for
// guaranteed glyph coverage respectively.
void buildFonts() {
    if (g->fmtClock) g->fmtClock->Release();
    if (g->fmtDate) g->fmtDate->Release();
    if (g->fmtHeading) g->fmtHeading->Release();
    if (g->fmtBody) g->fmtBody->Release();
    if (g->fmtSmall) g->fmtSmall->Release();

    const wchar_t* family = kUiFontFamily;
    g->fmtClock = makeFormat(family, 150.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtDate = makeFormat(family, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g->fmtHeading = makeFormat(family, 22.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtBody = makeFormat(family, 20.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    g->fmtSmall = makeFormat(family, 15.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g->fmtClock->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    // Headings are single-line titles — clip rather than wrap-and-overlap the row below
    // when the window gets narrow (e.g. windowed edit mode at a small size).
    g->fmtHeading->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    g->fmtHeading->SetTrimming(&trimming, nullptr);
}

// Fullscreen (TV/projector) text sizes, scaled off actual window height instead
// of the fixed windowed-desktop sizes in buildFonts(). Rebuilt only when the
// fullscreen canvas height actually changes (monitor swap / DPI change), not
// every frame, since CreateTextFormat is not free.
void buildFullscreenFonts(float height) {
    if (g->fmtClockFS) g->fmtClockFS->Release();
    if (g->fmtDateFS) g->fmtDateFS->Release();
    if (g->fmtBodyFS) g->fmtBodyFS->Release();
    if (g->fmtSmallFS) g->fmtSmallFS->Release();

    const wchar_t* family = kUiFontFamily;
    float clockSize = std::clamp(height * 0.34f, 120.0f, 520.0f);
    float dateSize = std::clamp(height * 0.034f, 26.0f, 64.0f);
    float bodySize = std::clamp(height * 0.032f, 20.0f, 56.0f);
    float smallSize = std::clamp(height * 0.024f, 16.0f, 40.0f);

    g->fmtClockFS = makeFormat(family, clockSize, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtClockFS->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    // The clock is a single "HH:MM:SS" line — never let it wrap. Actual width-fit
    // is re-checked/corrected against the real card width each frame in drawFrame,
    // since that's not known yet at this height-only sizing pass.
    g->fmtClockFS->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g->fmtDateFS = makeFormat(family, dateSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g->fmtDateFS->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g->fmtBodyFS = makeFormat(family, bodySize, DWRITE_FONT_WEIGHT_MEDIUM);
    g->fmtSmallFS = makeFormat(family, smallSize, DWRITE_FONT_WEIGHT_NORMAL);
    g->fsFontsBuiltForHeight = height;
    g->fsClockCorrectedForWidth = -1.0f; // force the width-fit re-check in drawFrame against the fresh clock format
}

void createDeviceIndependentResources() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g->d2dFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&g->dwriteFactory));

    buildFonts();
    g->fmtVersion = makeFormat(L"JetBrains Mono", 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    // Segoe MDL2 Assets ships with Windows 10/11 and is what Explorer/Settings use
    // for their own toolbar glyphs — crisper and more reliable than emoji fallback.
    g->fmtIcon = makeFormat(L"Segoe MDL2 Assets", 17.0f, DWRITE_FONT_WEIGHT_NORMAL);
}

void discardDeviceResources() {
    if (g->renderTarget) { g->renderTarget->Release(); g->renderTarget = nullptr; }
}

void createDeviceResources(HWND hwnd) {
    if (g->renderTarget) return;
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    g->d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        &g->renderTarget);
}

ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
    static ID2D1SolidColorBrush* b = nullptr;
    if (b) { b->Release(); b = nullptr; }
    g->renderTarget->CreateSolidColorBrush(c, &b);
    return b;
}

void roundedRect(D2D1_RECT_F r, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F* border = nullptr) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    g->renderTarget->FillRoundedRectangle(rr, brush(fill));
    if (border) g->renderTarget->DrawRoundedRectangle(rr, brush(*border), 1.5f);
}

void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F color,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    fmt->SetTextAlignment(align);
    g->renderTarget->DrawText(s.c_str(), (UINT32)s.size(), fmt, r, brush(color));
}

std::wstring formatClock(const SYSTEMTIME& st) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring formatDate(const SYSTEMTIME& st) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%d월 %d일 (%s)", st.wMonth, st.wDay, koreanWeekday(st.wDayOfWeek));
    return buf;
}

// ---- Period add/edit popup ----
void drawPeriodEditor(D2D1_SIZE_F size) {
    Palette pal = currentPalette();
    g->renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush(hex(0x000000, 0.55f)));

    float w = 460, h = 380;
    D2D1_RECT_F card = D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2);
    roundedRect(card, 16, pal.surface, &pal.cardBorder);

    float pad = 28;
    float x = card.left + pad;
    float y = card.top + pad;
    bool isNew = g->editingPeriodIndex == -2;

    text(isNew ? L"교시 추가" : L"교시 편집", D2D1::RectF(x, y, card.right - pad, y + 32), g->fmtHeading, pal.textPrimary);
    y += 52;

    auto field = [&](const wchar_t* caption, float fieldW) {
        text(caption, D2D1::RectF(x, y, card.right - pad, y + 22), g->fmtSmall, pal.textSecondary);
        y += 26;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + fieldW, y + 34);
        y += 34 + 20;
        return r;
    };

    g->rectFieldLabel = field(L"교시명 (예: 1교시)", card.right - pad - x);
    g->rectFieldSubject = field(L"과목 (예: 국어)", card.right - pad - x);

    // 시:분 드롭다운 두 쌍 (시작/종료) — 자유 텍스트 대신 선택으로 입력 실수 방지.
    text(L"시작", D2D1::RectF(x, y, x + 156, y + 22), g->fmtSmall, pal.textSecondary);
    text(L"종료", D2D1::RectF(x + 180, y, x + 336, y + 22), g->fmtSmall, pal.textSecondary);
    y += 26;
    float comboW = 70, colonW = 16, groupGap = 24;
    g->rectFieldStartHour = D2D1::RectF(x, y, x + comboW, y + 34);
    D2D1_RECT_F colon1 = D2D1::RectF(x + comboW, y, x + comboW + colonW, y + 34);
    g->rectFieldStartMinute = D2D1::RectF(colon1.right, y, colon1.right + comboW, y + 34);
    text(L":", colon1, g->fmtBody, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

    float x2 = g->rectFieldStartMinute.right + groupGap;
    g->rectFieldEndHour = D2D1::RectF(x2, y, x2 + comboW, y + 34);
    D2D1_RECT_F colon2 = D2D1::RectF(x2 + comboW, y, x2 + comboW + colonW, y + 34);
    g->rectFieldEndMinute = D2D1::RectF(colon2.right, y, colon2.right + comboW, y + 34);
    text(L":", colon2, g->fmtBody, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 34 + 28;

    float btnY = card.bottom - pad - 40;
    if (!isNew) {
        g->rectPeriodDelete = D2D1::RectF(x, btnY, x + 90, btnY + 40);
        roundedRect(g->rectPeriodDelete, 10, withAlpha(pal.error, 0.14f));
        text(L"삭제", g->rectPeriodDelete, g->fmtSmall, pal.error, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        g->rectPeriodDelete = D2D1::RectF(0, 0, 0, 0);
    }
    g->rectPeriodCancel = D2D1::RectF(card.right - pad - 200, btnY, card.right - pad - 104, btnY + 40);
    roundedRect(g->rectPeriodCancel, 10, hex(0x808080, 0.10f), &pal.cardBorder);
    text(L"취소", g->rectPeriodCancel, g->fmtSmall, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    g->rectPeriodSave = D2D1::RectF(card.right - pad - 96, btnY, card.right - pad, btnY + 40);
    roundedRect(g->rectPeriodSave, 10, hex(kHyoBlue, 0.85f));
    text(L"저장", g->rectPeriodSave, g->fmtSmall, hex(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- Bottom notice text editor popup ----
void drawNoticeEditor(D2D1_SIZE_F size) {
    Palette pal = currentPalette();
    g->renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush(hex(0x000000, 0.55f)));

    float w = 640, h = 440;
    D2D1_RECT_F card = D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2);
    roundedRect(card, 16, pal.surface, &pal.cardBorder);

    float pad = 28;
    float x = card.left + pad;
    float y = card.top + pad;

    text(L"안내 문구 편집", D2D1::RectF(x, y, card.right - pad, y + 32), g->fmtHeading, pal.textPrimary);
    y += 44;
    text(L"한 줄에 문구 하나씩 입력하세요. 하단에 점(·)으로 구분되어 표시됩니다.",
        D2D1::RectF(x, y, card.right - pad, y + 22), g->fmtSmall, pal.textTertiary);
    y += 32;

    float btnY = card.bottom - pad - 40;
    g->rectNoticesField = D2D1::RectF(x, y, card.right - pad, btnY - 16);

    g->rectNoticeCancel = D2D1::RectF(card.right - pad - 200, btnY, card.right - pad - 104, btnY + 40);
    roundedRect(g->rectNoticeCancel, 10, hex(0x808080, 0.10f), &pal.cardBorder);
    text(L"취소", g->rectNoticeCancel, g->fmtSmall, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    g->rectNoticeSave = D2D1::RectF(card.right - pad - 96, btnY, card.right - pad, btnY + 40);
    roundedRect(g->rectNoticeSave, 10, hex(kHyoBlue, 0.85f));
    text(L"저장", g->rectNoticeSave, g->fmtSmall, hex(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Floating list under the time-source button — not a full modal, just a small
// card, so the rest of the toolbar stays visible while choosing.
void drawTimeSourceDropdown() {
    Palette pal = currentPalette();
    float rowH = 44;
    float w = 260;
    D2D1_RECT_F card = D2D1::RectF(g->rectTimeSourceButton.left, g->rectTimeSourceButton.bottom + 6,
        g->rectTimeSourceButton.left + w, g->rectTimeSourceButton.bottom + 6 + rowH * kTimeSourceCount);
    roundedRect(card, 12, pal.surface, &pal.cardBorder);

    for (int i = 0; i < kTimeSourceCount; i++) {
        D2D1_RECT_F row = D2D1::RectF(card.left + 6, card.top + 6 + i * rowH, card.right - 6, card.top + 6 + (i + 1) * rowH - 4);
        bool isActive = i == g->settings.timeSourceIndex;
        bool isHovered = i == g->hoveredTimeSourceOption;
        if (isActive) roundedRect(row, 8, hex(kHyoBlue, 0.18f));
        else if (isHovered) roundedRect(row, 8, hex(kHyoBlue, 0.08f));
        text(kTimeSources[i].label, D2D1::RectF(row.left + 12, row.top, row.left + 100, row.bottom),
            g->fmtSmall, isActive ? hex(kHyoBlue) : pal.textPrimary);
        text(kTimeSources[i].host, D2D1::RectF(row.left + 100, row.top, row.right - 12, row.bottom),
            g->fmtVersion, pal.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g->timeSourceOptionRects[i] = row;
    }

    // Hover tooltip: brief description, floating to the right of the list.
    if (g->hoveredTimeSourceOption >= 0 && g->hoveredTimeSourceOption < kTimeSourceCount) {
        const D2D1_RECT_F& row = g->timeSourceOptionRects[g->hoveredTimeSourceOption];
        std::wstring desc = kTimeSources[g->hoveredTimeSourceOption].description;
        float tipW = 200;
        D2D1_RECT_F tip = D2D1::RectF(card.right + 10, row.top, card.right + 10 + tipW, row.top + rowH - 4);
        roundedRect(tip, 8, pal.surface, &pal.cardBorder);
        text(desc, D2D1::RectF(tip.left + 12, tip.top, tip.right - 12, tip.bottom), g->fmtVersion, pal.textSecondary);
    }
}

// ---- Save-as-profile popup: one name field, that's the whole interface ----
void drawSaveProfilePopup(D2D1_SIZE_F size) {
    Palette pal = currentPalette();
    g->renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush(hex(0x000000, 0.55f)));

    float w = 420, h = 200;
    D2D1_RECT_F card = D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2);
    roundedRect(card, 16, pal.surface, &pal.cardBorder);

    float pad = 28;
    float x = card.left + pad;
    float y = card.top + pad;
    text(L"현재 설정 저장", D2D1::RectF(x, y, card.right - pad, y + 32), g->fmtHeading, pal.textPrimary);
    y += 48;
    text(L"저장 이름", D2D1::RectF(x, y, card.right - pad, y + 22), g->fmtSmall, pal.textSecondary);
    y += 26;
    g->rectProfileNameField = D2D1::RectF(x, y, card.right - pad, y + 36);
    y += 36 + 24;

    float btnY = card.bottom - pad - 40;
    g->rectProfileSaveCancel = D2D1::RectF(card.right - pad - 200, btnY, card.right - pad - 104, btnY + 40);
    roundedRect(g->rectProfileSaveCancel, 10, hex(0x808080, 0.10f), &pal.cardBorder);
    text(L"취소", g->rectProfileSaveCancel, g->fmtSmall, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    g->rectProfileSaveBtn = D2D1::RectF(card.right - pad - 96, btnY, card.right - pad, btnY + 40);
    roundedRect(g->rectProfileSaveBtn, 10, hex(kHyoBlue, 0.85f));
    text(L"저장", g->rectProfileSaveBtn, g->fmtSmall, hex(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- Load popup: just a list of saved names, click one to restore it ----
void drawLoadProfilePopup(D2D1_SIZE_F size) {
    Palette pal = currentPalette();
    g->renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush(hex(0x000000, 0.55f)));

    float w = 420;
    float rowH = 52;
    float listH = g->profiles.empty() ? 60.0f : (float)g->profiles.size() * rowH;
    float h = 120 + listH;
    D2D1_RECT_F card = D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2);
    roundedRect(card, 16, pal.surface, &pal.cardBorder);

    float pad = 28;
    float x = card.left + pad;
    float y = card.top + pad;
    text(L"저장된 설정 불러오기", D2D1::RectF(x, y, card.right - pad, y + 32), g->fmtHeading, pal.textPrimary);
    y += 48;

    g->profileRowRects.assign(g->profiles.size(), D2D1::RectF(0, 0, 0, 0));
    g->profileRowDeleteRects.assign(g->profiles.size(), D2D1::RectF(0, 0, 0, 0));

    if (g->profiles.empty()) {
        text(L"저장된 항목이 없습니다.", D2D1::RectF(x, y, card.right - pad, y + 26), g->fmtSmall, pal.textTertiary);
    } else {
        for (size_t i = 0; i < g->profiles.size(); i++) {
            D2D1_RECT_F row = D2D1::RectF(x, y, card.right - pad, y + rowH - 10);
            roundedRect(row, 10, hex(kHyoBlue, 0.08f), &pal.cardBorder);
            text(g->profiles[i].name, D2D1::RectF(row.left + 14, row.top, row.right - 46, row.bottom),
                g->fmtBody, pal.textPrimary);

            D2D1_RECT_F delBtn = D2D1::RectF(row.right - 34, row.top + (row.bottom - row.top - 26) / 2,
                row.right - 8, row.top + (row.bottom - row.top - 26) / 2 + 26);
            roundedRect(delBtn, 8, withAlpha(pal.error, 0.14f));
            text(L"", delBtn, g->fmtIcon, pal.error, DWRITE_TEXT_ALIGNMENT_CENTER);

            g->profileRowRects[i] = row;
            g->profileRowDeleteRects[i] = delBtn;
            y += rowH;
        }
    }

    float btnY = card.bottom - pad - 40;
    g->rectProfileLoadClose = D2D1::RectF(card.right - pad - 96, btnY, card.right - pad, btnY + 40);
    roundedRect(g->rectProfileLoadClose, 10, hex(0x808080, 0.10f), &pal.cardBorder);
    text(L"닫기", g->rectProfileLoadClose, g->fmtSmall, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Shows/hides/positions the native Edit child controls to match whatever
// Direct2D popup is currently open. Content (SetWindowText) is set once, at the
// moment a popup opens — this only handles visibility and placement each tick.
void syncNativeControls() {
    bool showPeriodFields = g->editingPeriodIndex != -1;
    int periodShow = showPeriodFields ? SW_SHOWNA : SW_HIDE;
    ShowWindow(g->hEditLabel, periodShow);
    ShowWindow(g->hEditSubject, periodShow);
    ShowWindow(g->hComboStartHour, periodShow);
    ShowWindow(g->hComboStartMinute, periodShow);
    ShowWindow(g->hComboEndHour, periodShow);
    ShowWindow(g->hComboEndMinute, periodShow);
    if (showPeriodFields) {
        auto place = [](HWND h, D2D1_RECT_F r) {
            SetWindowPos(h, nullptr, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top),
                SWP_NOZORDER | SWP_NOACTIVATE);
        };
        // Dropdown-list combos need extra window height for the popup list itself —
        // only the top sliver (matching the field rect) shows while closed.
        auto placeCombo = [](HWND h, D2D1_RECT_F r) {
            SetWindowPos(h, nullptr, (int)r.left, (int)r.top, (int)(r.right - r.left), 300, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        place(g->hEditLabel, g->rectFieldLabel);
        place(g->hEditSubject, g->rectFieldSubject);
        placeCombo(g->hComboStartHour, g->rectFieldStartHour);
        placeCombo(g->hComboStartMinute, g->rectFieldStartMinute);
        placeCombo(g->hComboEndHour, g->rectFieldEndHour);
        placeCombo(g->hComboEndMinute, g->rectFieldEndMinute);
    }

    bool showNotices = g->editingNotices;
    ShowWindow(g->hEditNotices, showNotices ? SW_SHOWNA : SW_HIDE);
    if (showNotices) {
        SetWindowPos(g->hEditNotices, nullptr, (int)g->rectNoticesField.left, (int)g->rectNoticesField.top,
            (int)(g->rectNoticesField.right - g->rectNoticesField.left),
            (int)(g->rectNoticesField.bottom - g->rectNoticesField.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    bool showProfileName = g->savingProfile;
    ShowWindow(g->hEditProfileName, showProfileName ? SW_SHOWNA : SW_HIDE);
    if (showProfileName) {
        SetWindowPos(g->hEditProfileName, nullptr, (int)g->rectProfileNameField.left, (int)g->rectProfileNameField.top,
            (int)(g->rectProfileNameField.right - g->rectProfileNameField.left),
            (int)(g->rectProfileNameField.bottom - g->rectProfileNameField.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ---- Main frame ----
void drawFrame(HWND hwnd) {
    createDeviceResources(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_F size = g->renderTarget->GetSize();
    Palette pal = currentPalette();

    // Fullscreen uses its own, monitor-scaled text sizes (see buildFullscreenFonts)
    // so the student-facing display fills the frame instead of reusing windowed
    // desktop-sized text. Rebuilt lazily only when the canvas height changes.
    if (g->fullscreen && g->fsFontsBuiltForHeight != size.height) buildFullscreenFonts(size.height);
    IDWriteTextFormat* fClock = g->fullscreen ? g->fmtClockFS : g->fmtClock;
    IDWriteTextFormat* fDate = g->fullscreen ? g->fmtDateFS : g->fmtDate;
    IDWriteTextFormat* fBody = g->fullscreen ? g->fmtBodyFS : g->fmtBody;
    IDWriteTextFormat* fSmall = g->fullscreen ? g->fmtSmallFS : g->fmtSmall;

    g->renderTarget->BeginDraw();
    g->renderTarget->Clear(pal.base);

    // Current time (Naver-synced, converted to local KST wall clock).
    auto tp = g->timeSync.now();
    time_t tt = std::chrono::system_clock::to_time_t(tp);
    tm local{};
    localtime_s(&local, &tt);
    SYSTEMTIME st{};
    st.wYear = local.tm_year + 1900; st.wMonth = local.tm_mon + 1; st.wDay = local.tm_mday;
    st.wHour = local.tm_hour; st.wMinute = local.tm_min; st.wSecond = local.tm_sec;
    st.wDayOfWeek = local.tm_wday;
    int nowMinutes = st.wHour * 60 + st.wMinute;

    float pad = 48;
    float gap = 24;
    // Notice strip stays visible in fullscreen (legible-from-across-the-room
    // reminders), so its height scales with fSmall's fullscreen-sized font
    // instead of staying fixed at the small windowed value.
    float noticeHeight = g->fullscreen ? fSmall->GetFontSize() * 2.0f : 56.0f;
    // Fullscreen is the clean student-facing display: no admin toolbar, no footer.
    float footerHeight = g->fullscreen ? 0.0f : 40.0f;
    float topBarHeight = g->fullscreen ? 0.0f : 52.0f;

    // Content area = everything below the admin toolbar and above the bottom
    // notices/footer strip.
    D2D1_RECT_F content = D2D1::RectF(pad, pad + topBarHeight, size.width - pad, size.height - pad - noticeHeight - footerHeight);

    // Admin toolbar sits above both cards -- global app controls, not tied to
    // either panel. Hidden entirely in fullscreen (student-facing display).
    if (!g->fullscreen) {
        // Unified 12px spacing grid for the whole toolbar row (icon-to-icon,
        // tab-to-tab, tab-to-dropdown), and icons/tabs share one vertically
        // centered baseline within the topBarHeight band instead of both
        // being pinned to its top edge.
        float iconSize = 36, iconGap = 12;
        float iconsRight = size.width - pad;
        float iconTop = pad + (topBarHeight - iconSize) / 2.0f;

        // Right side: theme toggle immediately left of fullscreen.
        g->rectFullscreenBtn = D2D1::RectF(iconsRight - iconSize, iconTop, iconsRight, iconTop + iconSize);
        roundedRect(g->rectFullscreenBtn, 10, hex(kHyoBlue, 0.12f));
        text(g->fullscreen ? L"" : L"", g->rectFullscreenBtn, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        g->rectThemeToggle = D2D1::RectF(iconsRight - iconSize * 2 - iconGap, iconTop, iconsRight - iconSize - iconGap, iconTop + iconSize);
        roundedRect(g->rectThemeToggle, 10, hex(kHyoBlue, 0.12f));
        text(isEffectivelyLight() ? L"" : L"", g->rectThemeToggle, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Save / load named profiles, left of the theme toggle.
        g->rectLoadIcon = D2D1::RectF(iconsRight - iconSize * 3 - iconGap * 2, iconTop, iconsRight - iconSize * 2 - iconGap * 2, iconTop + iconSize);
        roundedRect(g->rectLoadIcon, 10, hex(kHyoBlue, 0.12f));
        text(L"", g->rectLoadIcon, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        g->rectSaveIcon = D2D1::RectF(iconsRight - iconSize * 4 - iconGap * 3, iconTop, iconsRight - iconSize * 3 - iconGap * 3, iconTop + iconSize);
        roundedRect(g->rectSaveIcon, 10, hex(kHyoBlue, 0.12f));
        text(L"", g->rectSaveIcon, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Left side: exam-type selector (모의고사 / 지필평가).
        g->scheduleButtons.clear();
        float bx = pad;
        for (auto& sc : g->scheduleStore.all()) {
            float bw = 140;
            D2D1_RECT_F r = D2D1::RectF(bx, iconTop, bx + bw, iconTop + iconSize);
            bool isActive = sc.id == g->scheduleStore.activeId();
            roundedRect(r, 10, isActive ? hex(kHyoBlue, 0.22f) : hex(kHyoBlue, 0.10f), isActive ? nullptr : &pal.cardBorder);
            text(sc.name, r, g->fmtSmall, isActive ? hex(kHyoBlue) : pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
            g->scheduleButtons.push_back({ r, sc.id });
            bx += bw + iconGap;
        }

        // Time-source dropdown button, right after the exam-type buttons.
        bx += iconGap;
        float dropdownW = 150;
        g->rectTimeSourceButton = D2D1::RectF(bx, iconTop, bx + dropdownW, iconTop + iconSize);
        roundedRect(g->rectTimeSourceButton, 10, hex(kHyoBlue, 0.10f), &pal.cardBorder);
        text(kTimeSources[g->settings.timeSourceIndex].label,
            D2D1::RectF(g->rectTimeSourceButton.left + 14, g->rectTimeSourceButton.top,
                g->rectTimeSourceButton.right - 24, g->rectTimeSourceButton.bottom),
            g->fmtSmall, pal.textSecondary);
        text(L"", D2D1::RectF(g->rectTimeSourceButton.right - 26, g->rectTimeSourceButton.top,
                g->rectTimeSourceButton.right - 4, g->rectTimeSourceButton.bottom),
            g->fmtIcon, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        g->rectFullscreenBtn = D2D1::RectF(0, 0, 0, 0);
        g->rectThemeToggle = D2D1::RectF(0, 0, 0, 0);
        g->scheduleButtons.clear();
        g->rectTimeSourceButton = D2D1::RectF(0, 0, 0, 0);
        g->timeSourceDropdownOpen = false;
    }

    // Left column: clock. Right column: today's period timetable. Split is
    // user-adjustable (drag handle, windowed only) via settings.splitRatio.
    float totalWidth = content.right - content.left;
    g->contentLeft = content.left;
    g->contentWidth = totalWidth;
    float leftWidth = totalWidth * g->settings.splitRatio - gap / 2;
    D2D1_RECT_F leftCard = D2D1::RectF(content.left, content.top, content.left + leftWidth, content.bottom);
    D2D1_RECT_F rightCard = D2D1::RectF(leftCard.right + gap, content.top, content.right, content.bottom);

    // The height-only pass in buildFullscreenFonts() can size the clock too wide
    // for narrower card ratios / aspect ratios (it doesn't know the card width
    // yet). Re-check against the real left-card width here and shrink if needed
    // — this is what actually prevents the "HH:MM:SS" line from wrapping/clipping.
    if (g->fullscreen) {
        float cardWidth = leftCard.right - leftCard.left;
        if (g->fsClockCorrectedForWidth != cardWidth) {
            const float kClockCharCount = 8.0f;      // "HH:MM:SS"
            const float kClockAdvanceEm = 0.58f;     // approx. digit advance as a fraction of em, Malgun Gothic Bold
            float widthFitSize = (cardWidth * 0.92f) / (kClockCharCount * kClockAdvanceEm);
            float appliedSize = std::min(fClock->GetFontSize(), std::clamp(widthFitSize, 60.0f, 520.0f));
            if (appliedSize != fClock->GetFontSize()) {
                fClock->Release();
                fClock = makeFormat(kUiFontFamily, appliedSize, DWRITE_FONT_WEIGHT_BOLD);
                fClock->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                g->fmtClockFS = fClock;
            }
            g->fsClockCorrectedForWidth = cardWidth;
        }
    }

    if (!g->fullscreen) {
        g->rectSplitHandle = D2D1::RectF(leftCard.right + gap / 2 - 6, content.top, leftCard.right + gap / 2 + 6, content.bottom);
        roundedRect(g->rectSplitHandle, 6, hex(kHyoBlue, g->draggingSplit ? 0.55f : 0.30f));
    } else {
        g->rectSplitHandle = D2D1::RectF(0, 0, 0, 0);
    }

    const ExamSchedule* active = g->scheduleStore.active();

    // ---- Left: clock ----
    roundedRect(leftCard, 16, pal.surface, &pal.cardBorder);
    float cardPad = g->fullscreen ? 32.0f : 24.0f;
    float syncLabelH = fSmall->GetFontSize() * 1.6f;
    std::wstring syncLabel = std::wstring(kTimeSources[g->settings.timeSourceIndex].label) +
        (g->timeSync.isSynced() ? L" 시간 동기화됨" : L" 동기화 대기중 (로컬 시간)");
    text(L"● " + syncLabel,
        D2D1::RectF(leftCard.left + cardPad, leftCard.top + cardPad, leftCard.right - cardPad, leftCard.top + cardPad + syncLabelH),
        fSmall, g->timeSync.isSynced() ? hex(kTeal) : hex(kOrange), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Date/weekday on top, big clock below it — the whole block is centered as
    // a unit within the card (both vertically and horizontally) using the
    // actual chosen font sizes, so fullscreen's much larger clock digits stay
    // visually balanced instead of sitting at windowed-mode offsets.
    float clockCenterY = leftCard.top + (leftCard.bottom - leftCard.top) / 2.0f;
    float dateLineH = fDate->GetFontSize() * 1.3f;
    float clockLineH = fClock->GetFontSize() * 1.15f;
    float blockGap = fDate->GetFontSize() * 0.6f;
    float blockTotalH = dateLineH + blockGap + clockLineH;
    float blockTop = clockCenterY - blockTotalH / 2.0f;
    text(formatDate(st), D2D1::RectF(leftCard.left, blockTop, leftCard.right, blockTop + dateLineH),
        fDate, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(formatClock(st), D2D1::RectF(leftCard.left, blockTop + dateLineH + blockGap, leftCard.right, blockTop + blockTotalH),
        fClock, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- Right: timetable ----
    roundedRect(rightCard, 16, pal.surface, &pal.cardBorder);
    float rx = rightCard.left + cardPad;
    float ry = rightCard.top + cardPad - 4.0f;
    float bannerH = g->fullscreen ? fSmall->GetFontSize() * 3.6f : 64.0f;

    ScheduleStatus status{};
    if (active) {
        status = evaluate(*active, nowMinutes);

        if (status.inBreak) {
            D2D1_RECT_F banner = D2D1::RectF(rx, ry, rightCard.right - cardPad, ry + bannerH);
            roundedRect(banner, 10, hex(kOrange, 0.16f));
            text(status.currentBreak->start.format() + L" ~ " + status.currentBreak->end.format(),
                D2D1::RectF(banner.left + 14, banner.top + 6, banner.right - 14, banner.top + bannerH * 0.45f),
                fSmall, hex(kOrange));
            text(status.currentBreak->note, D2D1::RectF(banner.left + 14, banner.top + bannerH * 0.45f, banner.right - 14, banner.bottom - 6),
                fSmall, hex(kOrange));
            ry = banner.bottom + 12;
        }

        // Windowed mode caps row height so more periods just scroll-fit; fullscreen
        // instead fills whatever vertical space is available (no cap) so 3-4 periods
        // on a projector read as large, evenly distributed cards instead of a small
        // packed list floating in empty space.
        size_t slotCount = active->periods.size() + (!g->fullscreen ? 1 : 0);
        float rowCap = g->fullscreen ? 100000.0f : 84.0f;
        float rowH = std::min(rowCap, (rightCard.bottom - 16 - ry) / (float)std::max<size_t>(1, slotCount));
        g->periodListTop = ry;
        g->periodRowHeight = rowH;

        g->periodRowRects.assign(active->periods.size(), D2D1::RectF(0, 0, 0, 0));
        g->periodDeleteRects.assign(active->periods.size(), D2D1::RectF(0, 0, 0, 0));

        std::vector<int> order;
        if (g->isDraggingPeriod && g->dragOrder.size() == active->periods.size()) {
            order = g->dragOrder;
        } else {
            order.resize(active->periods.size());
            for (size_t k = 0; k < order.size(); k++) order[k] = (int)k;
        }

        for (size_t slot = 0; slot < order.size(); slot++) {
            int idx = order[slot];
            const Period& p = active->periods[idx];
            bool isCurrent = status.currentPeriod == &p;
            bool isHovered = !g->fullscreen && !g->isDraggingPeriod && g->hoveredPeriodRow == idx;
            bool isBeingDragged = g->isDraggingPeriod && g->draggingPeriodIndex == idx;

            D2D1_RECT_F row = D2D1::RectF(rx, ry, rightCard.right - 24, ry + rowH - 8);

            if (isBeingDragged) {
                // Soft drop-shadow + bright outline for a "lifted card" feel while dragging.
                D2D1_RECT_F shadow = D2D1::RectF(row.left + 2, row.top + 5, row.right + 2, row.bottom + 5);
                roundedRect(shadow, 10, hex(0x000000, 0.30f));
                roundedRect(row, 10, hex(kHyoBlue, 0.24f));
                g->renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(row, 10, 10), brush(hex(kHyoBlue, 0.9f)), 2.0f);
            } else if (isCurrent) {
                roundedRect(row, 10, hex(kHyoBlue, 0.16f));
            } else if (isHovered) {
                // Subtle "raised" hover cue: brighter fill + visible border, signals it's clickable.
                roundedRect(row, 10, hex(kHyoBlue, 0.09f), &pal.cardBorder);
            }

            float textRight = !g->fullscreen ? row.right - 40 : row.right - 14;
            // Title/time line positions scale off the actual chosen font sizes
            // (rather than fixed 6/34/36/14px offsets) so fullscreen's larger
            // fBody/fSmall don't clip or crowd inside the also-larger rowH.
            float titleTop = row.top + rowH * 0.10f;
            float titleH = fBody->GetFontSize() * 1.3f;
            float timeTop = titleTop + titleH + fBody->GetFontSize() * 0.15f;
            text(p.label + L" · " + p.subject, D2D1::RectF(row.left + 14, titleTop, textRight, titleTop + titleH),
                fBody, isCurrent ? hex(kHyoBlue) : pal.textPrimary);
            text(p.start.format() + L" ~ " + p.end.format() + L"  (" + std::to_wstring(p.durationMinutes) + L"분)",
                D2D1::RectF(row.left + 14, timeTop, textRight, row.bottom - rowH * 0.05f),
                fSmall, pal.textSecondary);

            if (!g->fullscreen) {
                float rowCenterY = (row.top + row.bottom) / 2.0f;
                D2D1_RECT_F delBtn = D2D1::RectF(row.right - 32, rowCenterY - 13, row.right - 6, rowCenterY + 13);
                roundedRect(delBtn, 8, withAlpha(pal.error, 0.14f));
                text(L"", delBtn, g->fmtIcon, pal.error, DWRITE_TEXT_ALIGNMENT_CENTER);
                g->periodDeleteRects[idx] = delBtn;
            }
            g->periodRowRects[idx] = row;

            ry += rowH;
        }

        if (!g->fullscreen) {
            g->rectAddPeriod = D2D1::RectF(rx, ry, rightCard.right - 24, ry + rowH - 8);
            roundedRect(g->rectAddPeriod, 10, hex(kHyoBlue, 0.10f), &pal.cardBorder);
            text(L"+  교시 추가", g->rectAddPeriod, g->fmtBody, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            g->rectAddPeriod = D2D1::RectF(0, 0, 0, 0);
        }
    } else {
        text(L"시험 일정 없음", D2D1::RectF(rx, ry, rightCard.right - cardPad, ry + fBody->GetFontSize() * 1.5f), fBody, pal.textTertiary);
    }

    // ---- Bottom fixed notices ----
    float noticeY = content.bottom + 14;
    float noticeTextLeft = pad;
    if (!g->fullscreen && active) {
        g->rectNoticesEditIcon = D2D1::RectF(pad, noticeY, pad + 28, noticeY + 28);
        roundedRect(g->rectNoticesEditIcon, 8, hex(kHyoBlue, 0.14f));
        text(L"", g->rectNoticesEditIcon, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
        noticeTextLeft = pad + 38;
    } else {
        g->rectNoticesEditIcon = D2D1::RectF(0, 0, 0, 0);
    }
    if (active && !active->notices.empty()) {
        std::wstring notices;
        for (size_t i = 0; i < active->notices.size(); i++) {
            notices += active->notices[i];
            if (i + 1 < active->notices.size()) notices += L"   ·   ";
        }
        float noticeRightMargin = g->fullscreen ? pad : 260.0f;
        text(notices, D2D1::RectF(noticeTextLeft, noticeY, size.width - noticeRightMargin, noticeY + noticeHeight), fSmall, pal.textTertiary);
    }

    // Footer / about (verbatim brand format) — windowed/admin view only; the
    // fullscreen student display stays clean with no small print. The site
    // link is a separate clickable segment so its hit-rect is exact.
    if (!g->fullscreen) {
        float footerY = size.height - pad - 24;
        float footerBottom = size.height - pad;

        g->rectSiteLink = D2D1::RectF(size.width - pad - 78, footerY, size.width - pad, footerBottom);
        text(L"hyot.dev", D2D1::RectF(g->rectSiteLink.left, footerY, g->rectSiteLink.left + 58, footerBottom),
            g->fmtVersion, g->hoveredSiteLink ? hex(kHyoBlue) : pal.textTertiary);
        text(L"", D2D1::RectF(g->rectSiteLink.left + 58, footerY, g->rectSiteLink.right, footerBottom),
            g->fmtIcon, g->hoveredSiteLink ? hex(kHyoBlue) : pal.textTertiary);

        std::wstring footer = L"HyoExam v" + std::wstring(kAppVersion) + L" | © 2026 HyoT. All rights reserved.  ·";
        text(footer, D2D1::RectF(size.width - pad - 480, footerY, g->rectSiteLink.left - 4, footerBottom),
            g->fmtVersion, pal.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (g->editingPeriodIndex != -1) drawPeriodEditor(size);
    if (g->editingNotices) drawNoticeEditor(size);
    if (g->savingProfile) drawSaveProfilePopup(size);
    if (g->loadingProfile) drawLoadProfilePopup(size);
    if (g->timeSourceDropdownOpen) drawTimeSourceDropdown();

    HRESULT hr = g->renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardDeviceResources();

    syncNativeControls();
}

bool ptInRect(POINT pt, D2D1_RECT_F r) {
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

void toggleFullscreen(HWND hwnd) {
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g->fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &g->prevPlacement) && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        g->fullscreen = true;
        // Fullscreen is the student-facing display — always land on a clean view.
        g->editingPeriodIndex = -1;
        g->editingNotices = false;
        g->draggingSplit = false;
        g->timeSourceDropdownOpen = false;
        g->savingProfile = false;
        g->loadingProfile = false;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g->fullscreen = false;
    }
}

std::wstring getEditText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring s(len, L'\0');
    if (len > 0) GetWindowTextW(h, s.data(), len + 1);
    return s;
}

void openPeriodEditor(int index) {
    g->editingPeriodIndex = index;
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (index >= 0 && sc && index < (int)sc->periods.size()) {
        const Period& p = sc->periods[index];
        SetWindowTextW(g->hEditLabel, p.label.c_str());
        SetWindowTextW(g->hEditSubject, p.subject.c_str());
        SendMessage(g->hComboStartHour, CB_SETCURSEL, p.start.hour, 0);
        SendMessage(g->hComboStartMinute, CB_SETCURSEL, p.start.minute, 0);
        SendMessage(g->hComboEndHour, CB_SETCURSEL, p.end.hour, 0);
        SendMessage(g->hComboEndMinute, CB_SETCURSEL, p.end.minute, 0);
    } else {
        SetWindowTextW(g->hEditLabel, L"");
        SetWindowTextW(g->hEditSubject, L"");
        SendMessage(g->hComboStartHour, CB_SETCURSEL, 9, 0);
        SendMessage(g->hComboStartMinute, CB_SETCURSEL, 0, 0);
        SendMessage(g->hComboEndHour, CB_SETCURSEL, 9, 0);
        SendMessage(g->hComboEndMinute, CB_SETCURSEL, 50, 0);
    }
}

void closePeriodEditor() {
    g->editingPeriodIndex = -1;
}

void savePeriodEditor() {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (!sc) { closePeriodEditor(); return; }

    Period p;
    p.label = getEditText(g->hEditLabel);
    p.subject = getEditText(g->hEditSubject);
    p.start.hour = (int)SendMessage(g->hComboStartHour, CB_GETCURSEL, 0, 0);
    p.start.minute = (int)SendMessage(g->hComboStartMinute, CB_GETCURSEL, 0, 0);
    p.end.hour = (int)SendMessage(g->hComboEndHour, CB_GETCURSEL, 0, 0);
    p.end.minute = (int)SendMessage(g->hComboEndMinute, CB_GETCURSEL, 0, 0);
    p.durationMinutes = std::max(0, p.end.totalMinutes() - p.start.totalMinutes());

    if (g->editingPeriodIndex == -2) {
        sc->periods.push_back(p);
    } else if (g->editingPeriodIndex >= 0 && g->editingPeriodIndex < (int)sc->periods.size()) {
        sc->periods[g->editingPeriodIndex] = p;
    }
    std::sort(sc->periods.begin(), sc->periods.end(),
        [](const Period& a, const Period& b) { return a.start.totalMinutes() < b.start.totalMinutes(); });
    g->scheduleStore.saveToFile(g->dataPath);
    closePeriodEditor();
}

void deletePeriodEditor() {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (sc && g->editingPeriodIndex >= 0 && g->editingPeriodIndex < (int)sc->periods.size()) {
        sc->periods.erase(sc->periods.begin() + g->editingPeriodIndex);
        g->scheduleStore.saveToFile(g->dataPath);
    }
    closePeriodEditor();
}

void deletePeriodAt(int index) {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (sc && index >= 0 && index < (int)sc->periods.size()) {
        sc->periods.erase(sc->periods.begin() + index);
        g->scheduleStore.saveToFile(g->dataPath);
    }
}

// Recomputes the preview slot for the dragged period from the cursor's Y
// position and moves it within dragOrder — the classic "list shuffles live
// as you drag" reorder feel, without needing a separately floating card.
void updatePeriodDragOrder(int cursorY) {
    if (g->dragOrder.empty() || g->periodRowHeight <= 0) return;
    int count = (int)g->dragOrder.size();
    int slot = (int)((cursorY - g->periodListTop) / g->periodRowHeight);
    slot = std::max(0, std::min(count - 1, slot));

    int curPos = -1;
    for (int i = 0; i < count; i++) if (g->dragOrder[i] == g->draggingPeriodIndex) { curPos = i; break; }
    if (curPos == -1 || curPos == slot) return;

    int val = g->dragOrder[curPos];
    g->dragOrder.erase(g->dragOrder.begin() + curPos);
    g->dragOrder.insert(g->dragOrder.begin() + slot, val);
}

void commitPeriodDragReorder() {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (sc && g->dragOrder.size() == sc->periods.size()) {
        std::vector<Period> reordered;
        reordered.reserve(sc->periods.size());
        for (int idx : g->dragOrder) reordered.push_back(sc->periods[idx]);
        sc->periods = std::move(reordered);
        g->scheduleStore.saveToFile(g->dataPath);
    }
    g->isDraggingPeriod = false;
    g->draggingPeriodIndex = -1;
    g->dragOrder.clear();
}

void openNoticeEditor() {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    std::wstring joined;
    if (sc) {
        for (size_t i = 0; i < sc->notices.size(); i++) {
            joined += sc->notices[i];
            if (i + 1 < sc->notices.size()) joined += L"\r\n";
        }
    }
    SetWindowTextW(g->hEditNotices, joined.c_str());
    g->editingNotices = true;
}

void saveNoticeEditor() {
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (sc) {
        std::wstring text = getEditText(g->hEditNotices);
        sc->notices.clear();
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == L'\r')) line.pop_back();
            if (!line.empty()) sc->notices.push_back(line);
            start = end + 1;
        }
        g->scheduleStore.saveToFile(g->dataPath);
    }
    g->editingNotices = false;
}

// ---- Named profiles (save/load everything as one named snapshot) ----

std::wstring utf8ToWideMain(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::string wideToUtf8Main(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring profilesFilePath() {
    PWSTR appData = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = appData;
        CoTaskMemFree(appData);
    }
    dir += L"\\HyoExam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\profiles.json";
}

void loadProfilesFromDisk() {
    g->profiles.clear();
    std::ifstream f(profilesFilePath(), std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    auto root = hyo::json::parse(ss.str());
    for (auto& pv : root["profiles"].arr) {
        SavedProfile sp;
        sp.name = utf8ToWideMain(pv["name"].asString());
        sp.settingsJson = pv["settings"];
        sp.schedulesJson = pv["schedules"];
        g->profiles.push_back(sp);
    }
}

void saveProfilesToDisk() {
    using namespace hyo::json;
    Value root = Value::makeObject();
    Value arr = Value::makeArray();
    for (auto& sp : g->profiles) {
        Value pv = Value::makeObject();
        pv.set("name", Value::makeString(wideToUtf8Main(sp.name)));
        pv.set("settings", sp.settingsJson);
        pv.set("schedules", sp.schedulesJson);
        arr.arr.push_back(pv);
    }
    root.set("profiles", arr);
    std::ofstream f(profilesFilePath(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    std::string text = hyo::json::dump(root);
    f.write(text.data(), (std::streamsize)text.size());
}

hyo::json::Value buildSettingsSnapshotJson() {
    using namespace hyo::json;
    Value v = Value::makeObject();
    v.set("theme", Value::makeString(g->settings.theme == Theme::Light ? "light" : g->settings.theme == Theme::Dark ? "dark" : "auto"));
    v.set("splitRatio", Value::makeNumber(g->settings.splitRatio));
    v.set("timeSourceIndex", Value::makeNumber(g->settings.timeSourceIndex));
    return v;
}

void applySettingsSnapshotJson(const hyo::json::Value& v) {
    std::string themeStr = v["theme"].asString("auto");
    g->settings.theme = themeStr == "light" ? Theme::Light : themeStr == "dark" ? Theme::Dark : Theme::Auto;
    g->settings.splitRatio = (float)v["splitRatio"].asNumber(0.70);
    g->settings.timeSourceIndex = (int)v["timeSourceIndex"].asNumber(0);
    if (g->settings.timeSourceIndex < 0 || g->settings.timeSourceIndex >= kTimeSourceCount) g->settings.timeSourceIndex = 0;
}

void saveCurrentAsProfile(const std::wstring& name) {
    if (name.empty()) return;
    SavedProfile sp;
    sp.name = name;
    sp.settingsJson = buildSettingsSnapshotJson();
    sp.schedulesJson = g->scheduleStore.toJson();

    bool replaced = false;
    for (auto& existing : g->profiles) {
        if (existing.name == name) { existing = sp; replaced = true; break; }
    }
    if (!replaced) g->profiles.push_back(sp);
    saveProfilesToDisk();
}

void loadProfileByIndex(int idx) {
    if (idx < 0 || idx >= (int)g->profiles.size()) return;
    SavedProfile& sp = g->profiles[idx];
    applySettingsSnapshotJson(sp.settingsJson);
    g->scheduleStore.fromJson(sp.schedulesJson);
    g->settings.save();
    g->scheduleStore.saveToFile(g->dataPath);
    g->timeSync.setHost(kTimeSources[g->settings.timeSourceIndex].host);
}

void deleteProfileAt(int idx) {
    if (idx < 0 || idx >= (int)g->profiles.size()) return;
    g->profiles.erase(g->profiles.begin() + idx);
    saveProfilesToDisk();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, kTickTimerId, kTickIntervalMs, nullptr);

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
        g->hUiFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"맑은 고딕");

        auto makeEdit = [&](int id, DWORD extraStyle) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | extraStyle, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g->hUiFont, TRUE);
            return h;
        };
        g->hEditLabel = makeEdit(kIdEditLabel, ES_AUTOHSCROLL);
        g->hEditSubject = makeEdit(kIdEditSubject, ES_AUTOHSCROLL);
        g->hEditNotices = makeEdit(kIdEditNotices, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);
        g->hEditProfileName = makeEdit(kIdEditProfileName, ES_AUTOHSCROLL);

        auto makeCombo = [&](int id, int itemCount) {
            HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g->hUiFont, TRUE);
            wchar_t buf[4];
            for (int i = 0; i < itemCount; i++) {
                swprintf(buf, 4, L"%02d", i);
                SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            return h;
        };
        g->hComboStartHour = makeCombo(kIdComboStartHour, 24);
        g->hComboStartMinute = makeCombo(kIdComboStartMinute, 60);
        g->hComboEndHour = makeCombo(kIdComboEndHour, 24);
        g->hComboEndMinute = makeCombo(kIdComboEndMinute, 60);

        return 0;
    }
    case WM_TIMER:
        // Skip the tick redraw while a native text field is up for editing: Direct2D's
        // full-surface present on every repaint fights the child Edit control's own
        // paint and blanks out whatever the user just typed. The explicit
        // InvalidateRect calls around each click already cover state changes.
        if (g->editingPeriodIndex == -1 && !g->editingNotices && !g->savingProfile) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        drawFrame(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        if (g->renderTarget) {
            UINT w = LOWORD(lParam), h = HIWORD(lParam);
            g->renderTarget->Resize(D2D1::SizeU(w, h));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        if (g->editingPeriodIndex != -1) {
            if (ptInRect(pt, g->rectPeriodSave)) savePeriodEditor();
            else if (ptInRect(pt, g->rectPeriodCancel)) closePeriodEditor();
            else if (ptInRect(pt, g->rectPeriodDelete)) deletePeriodEditor();
        } else if (g->editingNotices) {
            if (ptInRect(pt, g->rectNoticeSave)) saveNoticeEditor();
            else if (ptInRect(pt, g->rectNoticeCancel)) g->editingNotices = false;
        } else if (g->savingProfile) {
            if (ptInRect(pt, g->rectProfileSaveBtn)) {
                saveCurrentAsProfile(getEditText(g->hEditProfileName));
                g->savingProfile = false;
            } else if (ptInRect(pt, g->rectProfileSaveCancel)) {
                g->savingProfile = false;
            }
        } else if (g->loadingProfile) {
            if (ptInRect(pt, g->rectProfileLoadClose)) {
                g->loadingProfile = false;
            } else {
                bool handled = false;
                for (size_t i = 0; i < g->profileRowDeleteRects.size() && !handled; i++) {
                    if (ptInRect(pt, g->profileRowDeleteRects[i])) { deleteProfileAt((int)i); handled = true; }
                }
                for (size_t i = 0; i < g->profileRowRects.size() && !handled; i++) {
                    if (ptInRect(pt, g->profileRowRects[i])) { loadProfileByIndex((int)i); g->loadingProfile = false; handled = true; }
                }
            }
        } else if (ptInRect(pt, g->rectSaveIcon)) {
            g->savingProfile = true;
            SetWindowTextW(g->hEditProfileName, L"");
        } else if (ptInRect(pt, g->rectLoadIcon)) {
            g->loadingProfile = true;
        } else if (g->timeSourceDropdownOpen) {
            for (int i = 0; i < kTimeSourceCount; i++) {
                if (ptInRect(pt, g->timeSourceOptionRects[i])) {
                    g->settings.timeSourceIndex = i;
                    g->settings.save();
                    g->timeSync.setHost(kTimeSources[i].host);
                    break;
                }
            }
            g->timeSourceDropdownOpen = false;
        } else if (ptInRect(pt, g->rectTimeSourceButton)) {
            g->timeSourceDropdownOpen = true;
        } else if (!g->fullscreen && ptInRect(pt, g->rectSiteLink)) {
            ShellExecuteW(nullptr, L"open", L"https://hyot.dev", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (ptInRect(pt, g->rectFullscreenBtn)) {
            toggleFullscreen(hwnd);
        } else if (ptInRect(pt, g->rectThemeToggle)) {
            g->settings.theme = isEffectivelyLight() ? Theme::Dark : Theme::Light;
            g->settings.save();
        } else if (!g->fullscreen && ptInRect(pt, g->rectSplitHandle)) {
            g->draggingSplit = true;
        } else if (!g->fullscreen && ptInRect(pt, g->rectAddPeriod)) {
            openPeriodEditor(-2);
        } else if (!g->fullscreen && ptInRect(pt, g->rectNoticesEditIcon)) {
            openNoticeEditor();
        } else if (!g->fullscreen) {
            bool handled = false;
            for (auto& [r, id] : g->scheduleButtons) {
                if (ptInRect(pt, r)) { g->scheduleStore.setActive(id); g->settings.activeScheduleId = id; g->settings.save(); handled = true; break; }
            }
            for (size_t i = 0; i < g->periodDeleteRects.size() && !handled; i++) {
                if (ptInRect(pt, g->periodDeleteRects[i])) { deletePeriodAt((int)i); handled = true; }
            }
            // A period row starts as a "pending click"; WM_MOUSEMOVE promotes it to a
            // drag once the cursor moves past a small threshold, otherwise WM_LBUTTONUP
            // treats it as a click and opens the editor.
            for (size_t i = 0; i < g->periodRowRects.size() && !handled; i++) {
                if (ptInRect(pt, g->periodRowRects[i])) {
                    g->pendingIsPeriodClick = true;
                    g->pendingClickPeriodIndex = (int)i;
                    g->mouseDownPt = pt;
                    handled = true;
                }
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (g->draggingSplit) {
            float ratio = (pt.x - g->contentLeft) / g->contentWidth;
            g->settings.splitRatio = std::max(0.40f, std::min(0.85f, ratio));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g->pendingIsPeriodClick) {
            int dx = pt.x - g->mouseDownPt.x, dy = pt.y - g->mouseDownPt.y;
            if (!g->isDraggingPeriod && (std::abs(dx) > 6 || std::abs(dy) > 6)) {
                g->isDraggingPeriod = true;
                g->draggingPeriodIndex = g->pendingClickPeriodIndex;
                g->dragOrder.resize(g->periodRowRects.size());
                for (size_t k = 0; k < g->dragOrder.size(); k++) g->dragOrder[k] = (int)k;
            }
            if (g->isDraggingPeriod) {
                updatePeriodDragOrder(pt.y);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        if (!g->fullscreen && !g->isDraggingPeriod) {
            int newHover = -1;
            for (size_t i = 0; i < g->periodRowRects.size(); i++) {
                if (ptInRect(pt, g->periodRowRects[i])) { newHover = (int)i; break; }
            }
            if (newHover != g->hoveredPeriodRow) {
                g->hoveredPeriodRow = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        if (g->timeSourceDropdownOpen) {
            int newHover = -1;
            for (int i = 0; i < kTimeSourceCount; i++) {
                if (ptInRect(pt, g->timeSourceOptionRects[i])) { newHover = i; break; }
            }
            if (newHover != g->hoveredTimeSourceOption) {
                g->hoveredTimeSourceOption = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        {
            bool nowHovered = !g->fullscreen && ptInRect(pt, g->rectSiteLink);
            if (nowHovered != g->hoveredSiteLink) {
                g->hoveredSiteLink = nowHovered;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, (g->hoveredPeriodRow != -1 || g->isDraggingPeriod || g->hoveredSiteLink) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_MOUSELEAVE:
        if (g->hoveredPeriodRow != -1) { g->hoveredPeriodRow = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        if (g->hoveredSiteLink) { g->hoveredSiteLink = false; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (g->draggingSplit) { g->draggingSplit = false; g->settings.save(); }
        else if (g->isDraggingPeriod) { commitPeriodDragReorder(); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (g->pendingIsPeriodClick) { openPeriodEditor(g->pendingClickPeriodIndex); InvalidateRect(hwnd, nullptr, FALSE); }
        g->pendingIsPeriodClick = false;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F11) toggleFullscreen(hwnd);
        else if (wParam == VK_ESCAPE && g->editingPeriodIndex != -1) { closePeriodEditor(); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->editingNotices) { g->editingNotices = false; InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->savingProfile) { g->savingProfile = false; InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->loadingProfile) { g->loadingProfile = false; InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->fullscreen) { toggleFullscreen(hwnd); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTickTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    AppState state;
    g = &state;

    g->settings.load();
    g->dataPath = wideExeDir() + L"\\data\\schedules.json";
    g->scheduleStore.loadFromFile(g->dataPath);
    if (!g->settings.activeScheduleId.empty()) g->scheduleStore.setActive(g->settings.activeScheduleId);
    loadProfilesFromDisk();

    if (g->settings.timeSourceIndex < 0 || g->settings.timeSourceIndex >= kTimeSourceCount) g->settings.timeSourceIndex = 0;
    g->timeSync.setHost(kTimeSources[g->settings.timeSourceIndex].host);
    g->timeSync.start();

    createDeviceIndependentResources();

    WNDCLASSEX wc{ sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(0, kWindowClass, L"HyoExam", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    toggleFullscreen(hwnd); // TV signage display: launch fullscreen by default (Esc/F11 to leave).

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g->timeSync.stop();
    discardDeviceResources();
    return 0;
}
