// HyoExam — full-screen exam-day clock & timetable for TV output.
// Win32 + Direct2D/DirectWrite: no UI framework overhead, single ~1-2MB static
// binary, instant startup. See README.md for build instructions.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <ctime>
#include <cwchar>
#include <cstdlib>
#include <algorithm>

#include "schedule.h"
#include "settings.h"
#include "time_sync.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace hyo;

namespace {

constexpr wchar_t kAppVersion[] = L"1.0.0";
constexpr wchar_t kWindowClass[] = L"HyoExamWindowClass";
constexpr UINT_PTR kTickTimerId = 1;
constexpr UINT kTickIntervalMs = 250;

// Native child control IDs (Edit) used for text entry — Direct2D has no
// text-input widgets of its own, so real typing/Korean IME goes through these.
constexpr int kIdEditLabel = 101;
constexpr int kIdEditSubject = 102;
constexpr int kIdEditStart = 103;
constexpr int kIdEditEnd = 104;
constexpr int kIdEditNotices = 105;

// Malgun Gothic ships with every Windows 10/11 install (it's the OS's own
// Korean UI font) — no bundling, no missing-font fallback surprises.
constexpr wchar_t kUiFontFamily[] = L"맑은 고딕";

// ---- Brand palette (HyoT-brand-kit.md — do not introduce new hues) ----
struct Palette {
    D2D1_COLOR_F base, surface, cardBorder;
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary;
};

D2D1_COLOR_F hex(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
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
    return p;
}

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
    D2D1_RECT_F rectFieldLabel{}, rectFieldSubject{}, rectFieldStart{}, rectFieldEnd{};

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

    HWND hEditLabel = nullptr, hEditSubject = nullptr, hEditStart = nullptr, hEditEnd = nullptr;
    HWND hEditNotices = nullptr;
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

    text(L"시작 (HH:MM)", D2D1::RectF(x, y, x + 180, y + 22), g->fmtSmall, pal.textSecondary);
    text(L"종료 (HH:MM)", D2D1::RectF(x + 200, y, x + 380, y + 22), g->fmtSmall, pal.textSecondary);
    y += 26;
    g->rectFieldStart = D2D1::RectF(x, y, x + 180, y + 34);
    g->rectFieldEnd = D2D1::RectF(x + 200, y, x + 380, y + 34);
    y += 34 + 28;

    float btnY = card.bottom - pad - 40;
    if (!isNew) {
        g->rectPeriodDelete = D2D1::RectF(x, btnY, x + 90, btnY + 40);
        roundedRect(g->rectPeriodDelete, 10, hex(kErrorLight, 0.14f));
        text(L"삭제", g->rectPeriodDelete, g->fmtSmall, hex(kErrorLight), DWRITE_TEXT_ALIGNMENT_CENTER);
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

// Shows/hides/positions the native Edit child controls to match whatever
// Direct2D popup is currently open. Content (SetWindowText) is set once, at the
// moment a popup opens — this only handles visibility and placement each tick.
void syncNativeControls() {
    bool showPeriodFields = g->editingPeriodIndex != -1;
    int periodShow = showPeriodFields ? SW_SHOWNA : SW_HIDE;
    ShowWindow(g->hEditLabel, periodShow);
    ShowWindow(g->hEditSubject, periodShow);
    ShowWindow(g->hEditStart, periodShow);
    ShowWindow(g->hEditEnd, periodShow);
    if (showPeriodFields) {
        auto place = [](HWND h, D2D1_RECT_F r) {
            SetWindowPos(h, nullptr, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top),
                SWP_NOZORDER | SWP_NOACTIVATE);
        };
        place(g->hEditLabel, g->rectFieldLabel);
        place(g->hEditSubject, g->rectFieldSubject);
        place(g->hEditStart, g->rectFieldStart);
        place(g->hEditEnd, g->rectFieldEnd);
    }

    bool showNotices = g->editingNotices;
    ShowWindow(g->hEditNotices, showNotices ? SW_SHOWNA : SW_HIDE);
    if (showNotices) {
        SetWindowPos(g->hEditNotices, nullptr, (int)g->rectNoticesField.left, (int)g->rectNoticesField.top,
            (int)(g->rectNoticesField.right - g->rectNoticesField.left),
            (int)(g->rectNoticesField.bottom - g->rectNoticesField.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ---- Main frame ----
void drawFrame(HWND hwnd) {
    createDeviceResources(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_F size = g->renderTarget->GetSize();
    Palette pal = currentPalette();

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
    float noticeHeight = 56;
    // Fullscreen is the clean student-facing display: no admin toolbar, no footer.
    float footerHeight = g->fullscreen ? 0.0f : 40.0f;
    float topBarHeight = g->fullscreen ? 0.0f : 52.0f;

    // Content area = everything below the admin toolbar and above the bottom
    // notices/footer strip.
    D2D1_RECT_F content = D2D1::RectF(pad, pad + topBarHeight, size.width - pad, size.height - pad - noticeHeight - footerHeight);

    // Admin toolbar sits above both cards -- global app controls, not tied to
    // either panel. Hidden entirely in fullscreen (student-facing display).
    if (!g->fullscreen) {
        float iconSize = 36, iconGap = 8;
        float iconsRight = size.width - pad;
        float iconTop = pad;

        // Right side: theme toggle immediately left of fullscreen.
        g->rectFullscreenBtn = D2D1::RectF(iconsRight - iconSize, iconTop, iconsRight, iconTop + iconSize);
        roundedRect(g->rectFullscreenBtn, 10, hex(kHyoBlue, 0.12f));
        text(g->fullscreen ? L"" : L"", g->rectFullscreenBtn, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        g->rectThemeToggle = D2D1::RectF(iconsRight - iconSize * 2 - iconGap, iconTop, iconsRight - iconSize - iconGap, iconTop + iconSize);
        roundedRect(g->rectThemeToggle, 10, hex(kHyoBlue, 0.12f));
        text(isEffectivelyLight() ? L"" : L"", g->rectThemeToggle, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

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
            bx += bw + 10;
        }
    } else {
        g->rectFullscreenBtn = D2D1::RectF(0, 0, 0, 0);
        g->rectThemeToggle = D2D1::RectF(0, 0, 0, 0);
        g->scheduleButtons.clear();
    }

    // Left column: clock. Right column: today's period timetable. Split is
    // user-adjustable (drag handle, windowed only) via settings.splitRatio.
    float totalWidth = content.right - content.left;
    g->contentLeft = content.left;
    g->contentWidth = totalWidth;
    float leftWidth = totalWidth * g->settings.splitRatio - gap / 2;
    D2D1_RECT_F leftCard = D2D1::RectF(content.left, content.top, content.left + leftWidth, content.bottom);
    D2D1_RECT_F rightCard = D2D1::RectF(leftCard.right + gap, content.top, content.right, content.bottom);

    if (!g->fullscreen) {
        g->rectSplitHandle = D2D1::RectF(leftCard.right + gap / 2 - 6, content.top, leftCard.right + gap / 2 + 6, content.bottom);
        roundedRect(g->rectSplitHandle, 6, hex(kHyoBlue, g->draggingSplit ? 0.55f : 0.30f));
    } else {
        g->rectSplitHandle = D2D1::RectF(0, 0, 0, 0);
    }

    const ExamSchedule* active = g->scheduleStore.active();
    std::wstring examLabel = active ? (active->name + (active->grade.empty() ? L"" : L" · " + active->grade)) : L"시험 일정 없음";

    // ---- Left: clock ----
    roundedRect(leftCard, 16, pal.surface, &pal.cardBorder);
    text(examLabel, D2D1::RectF(leftCard.left + 32, leftCard.top + 24, leftCard.right - 220, leftCard.top + 56),
        g->fmtBody, hex(kHyoBlue));
    text(g->timeSync.isSynced() ? L"● 네이버 시간 동기화됨" : L"● 로컬 시간 (동기화 대기중)",
        D2D1::RectF(leftCard.right - 260, leftCard.top + 24, leftCard.right - 32, leftCard.top + 48),
        g->fmtSmall, g->timeSync.isSynced() ? hex(kTeal) : hex(kOrange), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Date/weekday on top, big clock below it.
    float clockCenterY = leftCard.top + (leftCard.bottom - leftCard.top) / 2.0f;
    text(formatDate(st), D2D1::RectF(leftCard.left, clockCenterY - 148, leftCard.right, clockCenterY - 106),
        g->fmtDate, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(formatClock(st), D2D1::RectF(leftCard.left, clockCenterY - 90, leftCard.right, clockCenterY + 90),
        g->fmtClock, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- Right: 교시 및 시험 시간 ----
    roundedRect(rightCard, 16, pal.surface, &pal.cardBorder);
    float rx = rightCard.left + 24;
    float ry = rightCard.top + 20;

    text(L"교시별 시험 시간", D2D1::RectF(rx, ry, rightCard.right - 24, ry + 30), g->fmtHeading, pal.textPrimary);
    ry += 50;

    ScheduleStatus status{};
    if (active) {
        status = evaluate(*active, nowMinutes);

        if (status.inBreak) {
            D2D1_RECT_F banner = D2D1::RectF(rx, ry, rightCard.right - 24, ry + 64);
            roundedRect(banner, 10, hex(kOrange, 0.16f));
            text(status.currentBreak->start.format() + L" ~ " + status.currentBreak->end.format(),
                D2D1::RectF(banner.left + 14, banner.top + 6, banner.right - 14, banner.top + 28),
                g->fmtSmall, hex(kOrange));
            text(status.currentBreak->note, D2D1::RectF(banner.left + 14, banner.top + 28, banner.right - 14, banner.bottom - 6),
                g->fmtSmall, hex(kOrange));
            ry = banner.bottom + 12;
        }

        size_t slotCount = active->periods.size() + (!g->fullscreen ? 1 : 0);
        float rowH = std::min(84.0f, (rightCard.bottom - 16 - ry) / (float)std::max<size_t>(1, slotCount));
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
            text(p.label + L" · " + p.subject, D2D1::RectF(row.left + 14, row.top + 6, textRight, row.top + 34),
                g->fmtBody, isCurrent ? hex(kHyoBlue) : pal.textPrimary);
            text(p.start.format() + L" ~ " + p.end.format() + L"  (" + std::to_wstring(p.durationMinutes) + L"분)",
                D2D1::RectF(row.left + 14, row.top + 36, textRight, row.top + rowH - 14),
                g->fmtSmall, pal.textSecondary);

            if (!g->fullscreen) {
                float rowCenterY = (row.top + row.bottom) / 2.0f;
                D2D1_RECT_F delBtn = D2D1::RectF(row.right - 32, rowCenterY - 13, row.right - 6, rowCenterY + 13);
                roundedRect(delBtn, 8, hex(kErrorLight, 0.14f));
                text(L"", delBtn, g->fmtIcon, hex(kErrorLight), DWRITE_TEXT_ALIGNMENT_CENTER);
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
        text(L"시험 일정 없음", D2D1::RectF(rx, ry, rightCard.right - 24, ry + 30), g->fmtBody, pal.textTertiary);
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
        text(notices, D2D1::RectF(noticeTextLeft, noticeY, size.width - pad - 260, noticeY + noticeHeight), g->fmtSmall, pal.textTertiary);
    }

    // Footer / about (verbatim brand format) — windowed/admin view only; the
    // fullscreen student display stays clean with no small print.
    if (!g->fullscreen) {
        std::wstring footer = L"HyoExam v" + std::wstring(kAppVersion) + L" | © 2026 HyoT. All rights reserved.";
        text(footer, D2D1::RectF(size.width - pad - 420, size.height - pad - 24, size.width - pad, size.height - pad),
            g->fmtVersion, pal.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (g->editingPeriodIndex != -1) drawPeriodEditor(size);
    if (g->editingNotices) drawNoticeEditor(size);

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

TimeOfDay parseTimeInput(const std::wstring& s) {
    int h = 0, m = 0;
    swscanf_s(s.c_str(), L"%d:%d", &h, &m);
    TimeOfDay t;
    t.hour = std::max(0, std::min(23, h));
    t.minute = std::max(0, std::min(59, m));
    return t;
}

void openPeriodEditor(int index) {
    g->editingPeriodIndex = index;
    ExamSchedule* sc = g->scheduleStore.activeMutable();
    if (index >= 0 && sc && index < (int)sc->periods.size()) {
        const Period& p = sc->periods[index];
        SetWindowTextW(g->hEditLabel, p.label.c_str());
        SetWindowTextW(g->hEditSubject, p.subject.c_str());
        SetWindowTextW(g->hEditStart, p.start.format().c_str());
        SetWindowTextW(g->hEditEnd, p.end.format().c_str());
    } else {
        SetWindowTextW(g->hEditLabel, L"");
        SetWindowTextW(g->hEditSubject, L"");
        SetWindowTextW(g->hEditStart, L"");
        SetWindowTextW(g->hEditEnd, L"");
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
    p.start = parseTimeInput(getEditText(g->hEditStart));
    p.end = parseTimeInput(getEditText(g->hEditEnd));
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
        g->hEditStart = makeEdit(kIdEditStart, ES_AUTOHSCROLL);
        g->hEditEnd = makeEdit(kIdEditEnd, ES_AUTOHSCROLL);
        g->hEditNotices = makeEdit(kIdEditNotices, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);

        return 0;
    }
    case WM_TIMER:
        // Skip the tick redraw while a native text field is up for editing: Direct2D's
        // full-surface present on every repaint fights the child Edit control's own
        // paint and blanks out whatever the user just typed. The explicit
        // InvalidateRect calls around each click already cover state changes.
        if (g->editingPeriodIndex == -1 && !g->editingNotices) {
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
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, (g->hoveredPeriodRow != -1 || g->isDraggingPeriod) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_MOUSELEAVE:
        if (g->hoveredPeriodRow != -1) { g->hoveredPeriodRow = -1; InvalidateRect(hwnd, nullptr, FALSE); }
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
