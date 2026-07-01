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

    bool settingsOpen = false;
    bool fullscreen = false;
    bool editMode = false; // placeholder toggle; behavior TBD with the user
    WINDOWPLACEMENT prevPlacement{ sizeof(WINDOWPLACEMENT) };

    // Hit-test rects for the settings modal, recomputed each frame it's drawn.
    D2D1_RECT_F rectClose{};
    D2D1_RECT_F rectGear{};
    D2D1_RECT_F rectEditMode{};
    D2D1_RECT_F rectFullscreenBtn{};
    std::vector<std::pair<D2D1_RECT_F, std::wstring>> scheduleButtons;
    D2D1_RECT_F rectThemeDark{}, rectThemeLight{}, rectThemeAuto{};
};

AppState* g = nullptr;

std::wstring wideExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

const Palette currentPalette() {
    return g->settings.theme == Theme::Light ? lightPalette() : darkPalette();
}

const wchar_t* koreanWeekday(int wday) {
    static const wchar_t* names[] = { L"일", L"월", L"화", L"수", L"목", L"금", L"토" };
    return names[wday % 7];
}

void createDeviceIndependentResources() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g->d2dFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&g->dwriteFactory));

    auto makeFormat = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
        IDWriteTextFormat* fmt = nullptr;
        g->dwriteFactory->CreateTextFormat(family, nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-KR", &fmt);
        return fmt;
    };

    // Pretendard for UI text, JetBrains Mono for the clock/version (falls back to a
    // system font automatically via DirectWrite's font-fallback if not installed).
    g->fmtClock = makeFormat(L"JetBrains Mono", 150.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtDate = makeFormat(L"Pretendard", 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g->fmtHeading = makeFormat(L"Pretendard", 22.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtBody = makeFormat(L"Pretendard", 20.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    g->fmtSmall = makeFormat(L"Pretendard", 15.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g->fmtVersion = makeFormat(L"JetBrains Mono", 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    // Segoe MDL2 Assets ships with Windows 10/11 and is what Explorer/Settings use
    // for their own toolbar glyphs — crisper and more reliable than emoji fallback.
    g->fmtIcon = makeFormat(L"Segoe MDL2 Assets", 17.0f, DWRITE_FONT_WEIGHT_NORMAL);

    g->fmtClock->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
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

// ---- Settings modal ----
void drawSettingsModal(D2D1_SIZE_F size) {
    Palette pal = currentPalette();
    g->renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush(hex(0x000000, 0.55f)));

    float w = 640, h = 420;
    D2D1_RECT_F card = D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2);
    roundedRect(card, 16, pal.surface, &pal.cardBorder);

    float pad = 28;
    float x = card.left + pad;
    float y = card.top + pad;

    text(L"설정", D2D1::RectF(x, y, card.right - pad, y + 36), g->fmtHeading, pal.textPrimary);

    g->rectClose = D2D1::RectF(card.right - pad - 90, y - 4, card.right - pad, y + 32);
    roundedRect(g->rectClose, 8, hex(kHyoBlue, 0.12f));
    text(L"✕ 닫기", g->rectClose, g->fmtSmall, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

    y += 56;

    // Section: exam type
    text(L"시험 유형", D2D1::RectF(x, y, card.right - pad, y + 26), g->fmtBody, pal.textSecondary);
    y += 34;
    g->scheduleButtons.clear();
    float bx = x;
    for (auto& sc : g->scheduleStore.all()) {
        float bw = 150;
        D2D1_RECT_F r = D2D1::RectF(bx, y, bx + bw, y + 44);
        bool active = sc.id == g->scheduleStore.activeId();
        roundedRect(r, 10, active ? hex(kHyoBlue, 0.22f) : hex(0x808080, 0.08f), active ? nullptr : &pal.cardBorder);
        text(sc.name, r, g->fmtSmall, active ? hex(kHyoBlue) : pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        g->scheduleButtons.push_back({ r, sc.id });
        bx += bw + 12;
    }
    y += 44 + 28;

    // Section: theme
    text(L"테마", D2D1::RectF(x, y, card.right - pad, y + 26), g->fmtBody, pal.textSecondary);
    y += 34;
    auto themeBtn = [&](float bx2, const wchar_t* label, Theme t) {
        D2D1_RECT_F r = D2D1::RectF(bx2, y, bx2 + 120, y + 40);
        bool active = g->settings.theme == t;
        roundedRect(r, 10, active ? hex(kHyoBlue, 0.22f) : hex(0x808080, 0.08f), active ? nullptr : &pal.cardBorder);
        text(label, r, g->fmtSmall, active ? hex(kHyoBlue) : pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        return r;
    };
    g->rectThemeDark = themeBtn(x, L"다크", Theme::Dark);
    g->rectThemeLight = themeBtn(x + 132, L"라이트", Theme::Light);
    g->rectThemeAuto = themeBtn(x + 264, L"자동", Theme::Auto);

    text(L"HyoExam v" + std::wstring(kAppVersion) + L" | © 2026 HyoT. All rights reserved.",
        D2D1::RectF(x, card.bottom - pad - 20, card.right - pad, card.bottom - pad), g->fmtVersion, pal.textTertiary);
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
    float footerHeight = 40;

    // Content area = everything above the bottom notices/footer strip.
    D2D1_RECT_F content = D2D1::RectF(pad, pad, size.width - pad, size.height - pad - noticeHeight - footerHeight);

    // Left column (70%): clock. Right column (30%): today's period timetable.
    float totalWidth = content.right - content.left;
    float leftWidth = totalWidth * 0.70f - gap / 2;
    D2D1_RECT_F leftCard = D2D1::RectF(content.left, content.top, content.left + leftWidth, content.bottom);
    D2D1_RECT_F rightCard = D2D1::RectF(leftCard.right + gap, content.top, content.right, content.bottom);

    const ExamSchedule* active = g->scheduleStore.active();
    std::wstring examLabel = active ? (active->name + (active->grade.empty() ? L"" : L" · " + active->grade)) : L"시험 일정 없음";

    // ---- Left: clock ----
    roundedRect(leftCard, 16, pal.surface, &pal.cardBorder);
    text(examLabel, D2D1::RectF(leftCard.left + 32, leftCard.top + 24, leftCard.right - 220, leftCard.top + 56),
        g->fmtBody, hex(kHyoBlue));
    text(g->timeSync.isSynced() ? L"● 네이버 시간 동기화됨" : L"● 로컬 시간 (동기화 대기중)",
        D2D1::RectF(leftCard.right - 260, leftCard.top + 24, leftCard.right - 32, leftCard.top + 48),
        g->fmtSmall, g->timeSync.isSynced() ? hex(kTeal) : hex(kOrange), DWRITE_TEXT_ALIGNMENT_TRAILING);

    float clockCenterY = leftCard.top + (leftCard.bottom - leftCard.top) / 2.0f;
    text(formatClock(st), D2D1::RectF(leftCard.left, clockCenterY - 110, leftCard.right, clockCenterY + 70),
        g->fmtClock, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
    text(formatDate(st), D2D1::RectF(leftCard.left, clockCenterY + 78, leftCard.right, clockCenterY + 120),
        g->fmtDate, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- Right: 교시 및 시험 시간 ----
    roundedRect(rightCard, 16, pal.surface, &pal.cardBorder);
    float rx = rightCard.left + 24;
    float ry = rightCard.top + 20;

    // Top-right icon row: edit mode / fullscreen / settings — same pill style as
    // each other so they read as one control group, ordered by how often they're used.
    float iconSize = 36, iconGap = 8;
    float iconsRight = rightCard.right - 24;
    float iconsLeft = iconsRight - (iconSize * 3 + iconGap * 2);
    float iconTop = ry - 6;

    text(L"교시별 시험 시간", D2D1::RectF(rx, ry, iconsLeft - 12, ry + 30), g->fmtHeading, pal.textPrimary);

    g->rectEditMode = D2D1::RectF(iconsLeft, iconTop, iconsLeft + iconSize, iconTop + iconSize);
    roundedRect(g->rectEditMode, 10, g->editMode ? hex(kHyoBlue, 0.22f) : hex(kHyoBlue, 0.12f));
    text(L"", g->rectEditMode, g->fmtIcon, g->editMode ? hex(kHyoBlue) : pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

    g->rectFullscreenBtn = D2D1::RectF(iconsLeft + iconSize + iconGap, iconTop, iconsLeft + iconSize * 2 + iconGap, iconTop + iconSize);
    roundedRect(g->rectFullscreenBtn, 10, hex(kHyoBlue, 0.12f));
    text(g->fullscreen ? L"" : L"", g->rectFullscreenBtn, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

    g->rectGear = D2D1::RectF(iconsLeft + (iconSize + iconGap) * 2, iconTop, iconsRight, iconTop + iconSize);
    roundedRect(g->rectGear, 10, hex(kHyoBlue, 0.12f));
    text(L"", g->rectGear, g->fmtIcon, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
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

        float rowH = std::min(84.0f, (rightCard.bottom - 16 - ry) / (float)std::max<size_t>(1, active->periods.size()));
        for (auto& p : active->periods) {
            bool isCurrent = status.currentPeriod == &p;
            D2D1_RECT_F row = D2D1::RectF(rx, ry, rightCard.right - 24, ry + rowH - 8);
            if (isCurrent) roundedRect(row, 10, hex(kHyoBlue, 0.16f));

            text(p.label + L" · " + p.subject, D2D1::RectF(row.left + 14, row.top + 6, row.right - 14, row.top + 34),
                g->fmtBody, isCurrent ? hex(kHyoBlue) : pal.textPrimary);
            text(p.start.format() + L" ~ " + p.end.format() + L"  (" + std::to_wstring(p.durationMinutes) + L"분)",
                D2D1::RectF(row.left + 14, row.top + 36, row.right - 14, row.top + rowH - 14),
                g->fmtSmall, pal.textSecondary);

            ry += rowH;
        }
    } else {
        text(L"시험 일정 없음", D2D1::RectF(rx, ry, rightCard.right - 24, ry + 30), g->fmtBody, pal.textTertiary);
    }

    // ---- Bottom fixed notices ----
    if (active && !active->notices.empty()) {
        std::wstring notices;
        for (size_t i = 0; i < active->notices.size(); i++) {
            notices += active->notices[i];
            if (i + 1 < active->notices.size()) notices += L"   ·   ";
        }
        float noticeY = content.bottom + 14;
        text(notices, D2D1::RectF(pad, noticeY, size.width - pad - 260, noticeY + noticeHeight), g->fmtSmall, pal.textTertiary);
    }

    // Footer / about (verbatim brand format).
    std::wstring footer = L"HyoExam v" + std::wstring(kAppVersion) + L" | © 2026 HyoT. All rights reserved.";
    text(footer, D2D1::RectF(size.width - pad - 420, size.height - pad - 24, size.width - pad, size.height - pad),
        g->fmtVersion, pal.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (g->settingsOpen) drawSettingsModal(size);

    HRESULT hr = g->renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardDeviceResources();
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
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g->fullscreen = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, kTickTimerId, kTickIntervalMs, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
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
        if (g->settingsOpen) {
            if (ptInRect(pt, g->rectClose)) { g->settingsOpen = false; g->settings.save(); }
            else if (ptInRect(pt, g->rectThemeDark)) { g->settings.theme = Theme::Dark; g->settings.save(); }
            else if (ptInRect(pt, g->rectThemeLight)) { g->settings.theme = Theme::Light; g->settings.save(); }
            else if (ptInRect(pt, g->rectThemeAuto)) { g->settings.theme = Theme::Auto; g->settings.save(); }
            else {
                for (auto& [r, id] : g->scheduleButtons) {
                    if (ptInRect(pt, r)) { g->scheduleStore.setActive(id); g->settings.activeScheduleId = id; g->settings.save(); break; }
                }
            }
        } else if (ptInRect(pt, g->rectGear)) {
            g->settingsOpen = true;
        } else if (ptInRect(pt, g->rectFullscreenBtn)) {
            toggleFullscreen(hwnd);
        } else if (ptInRect(pt, g->rectEditMode)) {
            g->editMode = !g->editMode;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_F11) toggleFullscreen(hwnd);
        else if (wParam == VK_ESCAPE && g->settingsOpen) { g->settingsOpen = false; g->settings.save(); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->fullscreen) { toggleFullscreen(hwnd); }
        else if (wParam == VK_F2) { g->settingsOpen = !g->settingsOpen; InvalidateRect(hwnd, nullptr, FALSE); }
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
    std::wstring dataPath = wideExeDir() + L"\\data\\schedules.json";
    g->scheduleStore.loadFromFile(dataPath);
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

    HWND hwnd = CreateWindowEx(0, kWindowClass, L"HyoExam", WS_OVERLAPPEDWINDOW,
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
