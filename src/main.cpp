// HyoExam — full-screen exam-day clock & timetable for TV output.
// Win32 + Direct2D/DirectWrite: no UI framework overhead, single ~1-2MB static
// binary, instant startup. See README.md for build instructions.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <uxtheme.h> // SetWindowTheme — dark-mode re-skin for native Edit/ComboBox controls
#define _RICHEDIT_VER 0x0500 // CHARFORMAT2W / RICHEDIT50W (Msftedit.dll)
#include <richedit.h>
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

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

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
constexpr int kIdComboStartHour = 106;
constexpr int kIdComboStartMinute = 107;
constexpr int kIdComboEndHour = 108;
constexpr int kIdComboEndMinute = 109;
constexpr int kIdEditProfileName = 110;
constexpr int kIdRichMemo = 111;

// Use the invariant English family name so font lookup works regardless of OS
// display language/locale. ("맑은 고딕" can fail to resolve on non-Korean setups.)
constexpr wchar_t kUiFontFamily[] = L"Malgun Gothic";

// Time servers the clock can sync against (SNTP, UDP/123). Index persisted
// in Settings::timeSourceIndex.
struct TimeSourceOption { const wchar_t* host; const wchar_t* label; const wchar_t* description; };
const TimeSourceOption kTimeSources[] = {
    { L"time.navyism.com", L"네이버 시계", L"네이버에서 제공하는 표준시" },
    { L"ntp.kriss.re.kr", L"KRISS", L"한국표준과학연구원 표준시" },
    { L"time.bora.net", L"KISA", L"한국인터넷진흥원 표준시" },
};
constexpr int kTimeSourceCount = (int)(sizeof(kTimeSources) / sizeof(kTimeSources[0]));

// Curated set for the memo's emoji picker -- school/exam-day relevant only,
// deliberately not the full OS emoji panel (which has no content filtering).
const wchar_t* kSchoolEmojis[] = {
    L"📚", L"✏", L"📝", L"📖", L"🎓",
    L"⏰", L"✅", L"❌", L"⭐", L"📌",
    L"🔔", L"📢", L"💯", L"👍", L"😊",
    L"🎉", L"📅", L"❗", L"❓", L"💡",
};
constexpr int kSchoolEmojiCount = (int)(sizeof(kSchoolEmojis) / sizeof(kSchoolEmojis[0]));

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
    ID2D1SolidColorBrush* scratchBrush = nullptr; // one reusable fill/stroke brush, recolored per draw call

    IDWriteTextFormat* fmtClock = nullptr;
    IDWriteTextFormat* fmtDate = nullptr;
    IDWriteTextFormat* fmtHeading = nullptr;
    IDWriteTextFormat* fmtBody = nullptr;
    IDWriteTextFormat* fmtSmall = nullptr;
    IDWriteTextFormat* fmtSmallBold = nullptr; // memo toolbar's "가" (Bold) icon glyph
    IDWriteTextFormat* fmtVersion = nullptr;
    IDWriteTextFormat* fmtIcon = nullptr;      // inline glyphs (footer link arrow, dropdown chevron)
    IDWriteTextFormat* fmtIconBox = nullptr;   // larger glyphs inside boxed icon buttons (save/load/theme/fullscreen/delete)
    IDWriteTextFormat* fmtPeriodTimeWin = nullptr; // windowed period-row time text — fmtSmall + 10pt, kept separate so it doesn't resize fmtSmall everywhere else

    // Fullscreen (TV/projector) variants of clock/date/body/small — sized off the
    // actual monitor resolution so the student-facing display fills the frame
    // instead of reusing windowed-desktop-sized text on a huge canvas.
    IDWriteTextFormat* fmtClockFS = nullptr;
    IDWriteTextFormat* fmtDateFS = nullptr;
    IDWriteTextFormat* fmtBodyFS = nullptr;
    IDWriteTextFormat* fmtSmallFS = nullptr;
    float fsFontsBuiltForHeight = -1.0f;
    float fsClockCorrectedForWidth = -1.0f;
    // The height-only size computed in buildFullscreenFonts(), before any
    // width-fit shrink is applied — the width-fit correction re-derives from
    // this fixed baseline (not from the format's current, possibly already
    // shrunk, size) so the clock grows back when the 표시 panel toggles free
    // up width, instead of only ever being able to shrink further.
    float fsClockBaseSize = -1.0f;

    // Fullscreen period-row title/time text is sized off the actual row height
    // (not a fixed windowed size), so a handful of periods fill their large
    // rows and many periods still fit legibly — same idea as fmtClockFS.
    IDWriteTextFormat* fmtPeriodTitleFS = nullptr;
    IDWriteTextFormat* fmtPeriodTimeFS = nullptr;
    float fsPeriodBuiltForRowH = -1.0f;
    float fsPeriodTimeCorrectedForWidth = -1.0f; // re-check against the actual row width, like fsClockCorrectedForWidth
    float fsPeriodTimeBaseSize = -1.0f; // height-only baseline, same grow-back purpose as fsClockBaseSize

    Settings settings;
    ScheduleStore scheduleStore;
    TimeSync timeSync;
    std::wstring dataPath;

    bool fullscreen = false;
    WINDOWPLACEMENT prevPlacement{ sizeof(WINDOWPLACEMENT) };

    // Hit-test rects for the admin toolbar, recomputed each frame it's drawn.
    D2D1_RECT_F rectFullscreenBtn{};
    D2D1_RECT_F rectThemeToggle{};
    // Fullscreen-only: the return-to-window icon stays hidden until the mouse
    // moves, then fades back out after 5s idle. 0 = hidden (not moved yet since
    // entering fullscreen).
    ULONGLONG fullscreenIconLastMoveMs = 0;
    bool cursorHiddenInFullscreen = false; // mirrors the icon's visible/hidden state so the two disappear/reappear together
    std::vector<std::pair<D2D1_RECT_F, std::wstring>> scheduleButtons;

    // Footer site link.
    D2D1_RECT_F rectSiteLink{};
    bool hoveredSiteLink = false;

    // Time-source dropdown (right of the exam-type buttons).
    D2D1_RECT_F rectTimeSourceButton{};
    bool timeSourceDropdownOpen = false;
    D2D1_RECT_F timeSourceOptionRects[kTimeSourceCount]{};
    int hoveredTimeSourceOption = -1;

    // Fullscreen display-toggle dropdown (right of the time-source dropdown) --
    // the clock is always shown in fullscreen; these two checkboxes opt in the
    // schedule panel and the memo card independently.
    D2D1_RECT_F rectDisplaySettingsButton{};
    bool displaySettingsOpen = false;
    D2D1_RECT_F rectDisplayScheduleRow{}, rectDisplayMemoRow{};


    // Timetable editing.
    int editingPeriodIndex = -1; // -1 = closed, -2 = new period, >=0 = editing that index
    std::vector<D2D1_RECT_F> periodRowRects;
    std::vector<D2D1_RECT_F> periodDeleteRects;
    D2D1_RECT_F rectAddPeriod{};
    D2D1_RECT_F rectPeriodSave{}, rectPeriodCancel{}, rectPeriodDelete{};
    D2D1_RECT_F rectFieldLabel{}, rectFieldSubject{};
    D2D1_RECT_F rectFieldStartHour{}, rectFieldStartMinute{}, rectFieldEndHour{}, rectFieldEndMinute{};
    // Last rect/visibility actually applied to each period-field control, so a
    // redraw that doesn't actually move anything (e.g. mouse hover elsewhere
    // while the popup is up) skips the SetWindowPos call -- reapplying it on a
    // combo box that's currently dropped down closes the drop-down list out
    // from under the user before they can pick a value.
    D2D1_RECT_F rectFieldLabelApplied{ -1, -1, -1, -1 }, rectFieldSubjectApplied{ -1, -1, -1, -1 };
    D2D1_RECT_F rectFieldStartHourApplied{ -1, -1, -1, -1 }, rectFieldStartMinuteApplied{ -1, -1, -1, -1 };
    D2D1_RECT_F rectFieldEndHourApplied{ -1, -1, -1, -1 }, rectFieldEndMinuteApplied{ -1, -1, -1, -1 };
    int periodFieldsShownApplied = -1; // -1 = never set

    // Period row hover (click affordance) and drag-to-reorder.
    int hoveredPeriodRow = -1;
    bool pendingIsPeriodClick = false;
    int pendingClickPeriodIndex = -1;
    POINT mouseDownPt{};
    bool isDraggingPeriod = false;
    int draggingPeriodIndex = -1;
    std::vector<int> dragOrder; // preview order, holds original indices into active->periods
    float periodListTop = 0, periodRowHeight = 0;


    // Named profiles: a full snapshot of settings + schedules, saved/loaded as
    // one unit (not a file picker — an in-app named-slot list).
    std::vector<SavedProfile> profiles;
    D2D1_RECT_F rectSaveIcon{}, rectLoadIcon{};
    bool savingProfile = false;
    bool loadingProfile = false;
    D2D1_RECT_F rectProfileNameField{}, rectProfileSaveBtn{}, rectProfileSaveCancel{};
    D2D1_RECT_F rectProfileLoadClose{};
    std::vector<D2D1_RECT_F> profileRowRects, profileRowDeleteRects;

    // Small fade-out toast (e.g. "저장되었습니다") shown after an action like
    // profile save. toastShownAtMs == 0 means no toast is active.
    std::wstring toastText;
    ULONGLONG toastShownAtMs = 0;

    // Free-write memo under the clock (RichEdit control, so per-selection bold/
    // underline/size/colors come for free instead of hand-rolled rich text).
    // Always visible in both windowed and fullscreen; only editable/toolbar-
    // enabled in windowed mode.
    HWND hRichMemo = nullptr;
    WNDPROC richMemoDefaultProc = nullptr; // original RichEdit WndProc, saved by the fullscreen-caret subclass below
    D2D1_RECT_F rectMemoEdit{};
    D2D1_RECT_F rectMemoEditApplied{ -1, -1, -1, -1 }; // last rect actually SetWindowPos'd, so idle frames skip it
    int memoReadOnlyApplied = -1; // -1 = never set; avoids re-sending EM_SETREADONLY every idle frame
    int memoBorderApplied = -1;   // -1 = never set; the sunken WS_EX_CLIENTEDGE border is windowed-only (looks out of place on the clean fullscreen display)
    int memoHiddenApplied = -1;   // -1 = never set; avoids re-sending ShowWindow every idle frame (250ms timer + every mouse-move repaint add up fast)
    D2D1_RECT_F rectMemoBold{}, rectMemoUnderline{}, rectMemoSizeDown{}, rectMemoSizeUp{}, rectMemoFontColor{}, rectMemoBgColor{}, rectMemoEmoji{};
    D2D1_RECT_F rectMemoAlignLeft{}, rectMemoAlignCenter{}, rectMemoAlignRight{};
    bool memoBoldActive = false, memoUnderlineActive = false;
    WORD memoAlign = PFA_LEFT;
    bool memoEmojiPickerOpen = false;
    D2D1_RECT_F memoEmojiRects[kSchoolEmojiCount]{};
    COLORREF memoCustomColors[16]{}; // persists the ChooseColor "custom colors" row across both pickers

    HWND hEditLabel = nullptr, hEditSubject = nullptr;
    HWND hComboStartHour = nullptr, hComboStartMinute = nullptr, hComboEndHour = nullptr, hComboEndMinute = nullptr;
    HWND hEditProfileName = nullptr;
    HFONT hUiFont = nullptr;
    HBRUSH hNativeCtlBgBrush = nullptr; // theme-matched background for WM_CTLCOLOREDIT/WM_CTLCOLORLISTBOX
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
    // text() only ever sets horizontal alignment per call, so every format needs
    // vertical centering here -- otherwise anything drawn into a rect taller than
    // its text (icon boxes, dropdown buttons, popup buttons) sits glued to the
    // top instead of centered in the box.
    if (fmt) fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    if (g->fmtSmallBold) g->fmtSmallBold->Release();
    if (g->fmtPeriodTimeWin) g->fmtPeriodTimeWin->Release();

    const wchar_t* family = kUiFontFamily;
    g->fmtClock = makeFormat(family, 150.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtDate = makeFormat(family, 70.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD); // 2.5x the original 28pt, per explicit request
    g->fmtHeading = makeFormat(family, 22.0f, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtBody = makeFormat(family, 20.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    g->fmtSmall = makeFormat(family, 15.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g->fmtSmallBold = makeFormat(family, 15.0f, DWRITE_FONT_WEIGHT_BOLD); // memo toolbar's "가" (Bold) icon
    g->fmtPeriodTimeWin = makeFormat(family, 25.0f, DWRITE_FONT_WEIGHT_NORMAL); // windowed period-row time text: fmtSmall (15pt) + 10pt
    g->fmtPeriodTimeWin->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
    float dateSize = std::clamp(height * 0.085f, 65.0f, 160.0f); // 2.5x the original 0.034/26-64 range
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
    g->fsClockBaseSize = clockSize;
    g->fsClockCorrectedForWidth = -1.0f; // force the width-fit re-check in drawFrame against the fresh clock format
}

// Fullscreen period-row title/time text, sized off the actual row height so a
// handful of periods (large rows) fill them instead of leaving a fixed small
// font floating in a mostly-empty box, while many periods (small rows) still
// shrink to fit legibly. Rebuilt only when rowH actually changes.
void buildPeriodRowFonts(float rowH) {
    if (g->fmtPeriodTitleFS) g->fmtPeriodTitleFS->Release();
    if (g->fmtPeriodTimeFS) g->fmtPeriodTimeFS->Release();
    float titleSize = std::clamp(rowH * 0.32f, 26.0f, 130.0f);
    // Height-only guess, tuned bigger than before -- the actual applied size is
    // re-checked against the real row width in drawFrame (fsPeriodTimeCorrectedForWidth)
    // and shrunk if it would wrap the "HH:MM ~ HH:MM(NN분)" line to two lines.
    float timeSize = std::clamp(rowH * 0.30f, 18.0f, 110.0f);
    // Bold title reads better at a glance from across a classroom.
    g->fmtPeriodTitleFS = makeFormat(kUiFontFamily, titleSize, DWRITE_FONT_WEIGHT_BOLD);
    g->fmtPeriodTimeFS = makeFormat(kUiFontFamily, timeSize, DWRITE_FONT_WEIGHT_MEDIUM);
    g->fmtPeriodTimeFS->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g->fsPeriodBuiltForRowH = rowH;
    g->fsPeriodTimeBaseSize = timeSize;
    g->fsPeriodTimeCorrectedForWidth = -1.0f; // force the width-fit re-check against the fresh format
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
    // Larger variant for glyphs inside boxed icon buttons (toolbar/delete), which
    // got 1.5x bigger hit targets — kept separate from fmtIcon so inline glyphs
    // (footer link arrow, dropdown chevron) stay at their original small size.
    g->fmtIconBox = makeFormat(L"Segoe MDL2 Assets", 26.0f, DWRITE_FONT_WEIGHT_NORMAL);
}

void discardDeviceResources() {
    // scratchBrush is created against renderTarget, so it must die with it --
    // otherwise the next brush() call would hand back a brush bound to a
    // released device (D2DERR_RECREATE_TARGET path).
    if (g->scratchBrush) { g->scratchBrush->Release(); g->scratchBrush = nullptr; }
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

// Every text()/roundedRect() call needs a solid brush; rather than create and
// release one per call (dozens per frame), keep a single brush alive for the
// life of the render target and just recolor it. Freed in discardDeviceResources
// so it never outlives the device it was created against.
ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
    if (!g->scratchBrush) g->renderTarget->CreateSolidColorBrush(c, &g->scratchBrush);
    else g->scratchBrush->SetColor(c);
    return g->scratchBrush;
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

// ---- Fullscreen display toggles: clock is always on, schedule/memo opt in ----
void drawDisplaySettingsDropdown() {
    Palette pal = currentPalette();
    float rowH = 44;
    float w = 220;
    D2D1_RECT_F card = D2D1::RectF(g->rectDisplaySettingsButton.left, g->rectDisplaySettingsButton.bottom + 6,
        g->rectDisplaySettingsButton.left + w, g->rectDisplaySettingsButton.bottom + 6 + rowH * 2);
    roundedRect(card, 12, pal.surface, &pal.cardBorder);

    auto checkRow = [&](int i, const wchar_t* label, bool checked, D2D1_RECT_F& outRow) {
        D2D1_RECT_F row = D2D1::RectF(card.left + 6, card.top + 6 + i * rowH, card.right - 6, card.top + 6 + (i + 1) * rowH - 4);
        outRow = row;
        D2D1_RECT_F box = D2D1::RectF(row.left + 8, row.top + (row.bottom - row.top - 20) / 2,
            row.left + 28, row.top + (row.bottom - row.top - 20) / 2 + 20);
        roundedRect(box, 5, checked ? hex(kHyoBlue, 0.85f) : hex(kHyoBlue, 0.10f), checked ? nullptr : &pal.cardBorder);
        if (checked) text(L"✓", box, g->fmtSmall, hex(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        text(label, D2D1::RectF(box.right + 10, row.top, row.right - 8, row.bottom), g->fmtSmall, pal.textPrimary);
    };
    checkRow(0, L"교시/과목 표시", g->settings.fullscreenShowSchedule, g->rectDisplayScheduleRow);
    checkRow(1, L"메모 표시", g->settings.fullscreenShowMemo, g->rectDisplayMemoRow);
}

// ---- Curated emoji grid for the memo (school-appropriate subset only) ----
void drawMemoEmojiPicker() {
    Palette pal = currentPalette();
    constexpr int kCols = 5;
    constexpr int kRows = (kSchoolEmojiCount + kCols - 1) / kCols;
    float cell = 40, cellGap = 4, pad = 8;
    float w = kCols * cell + (kCols - 1) * cellGap + pad * 2;
    float h = kRows * cell + (kRows - 1) * cellGap + pad * 2;
    // Opens upward, not downward: the memo toolbar sits near the bottom of the
    // screen, so a downward popup would overflow past the window's bottom edge.
    D2D1_RECT_F card = D2D1::RectF(g->rectMemoEmoji.left, g->rectMemoEmoji.top - h - 6,
        g->rectMemoEmoji.left + w, g->rectMemoEmoji.top - 6);
    roundedRect(card, 12, pal.surface, &pal.cardBorder);

    for (int i = 0; i < kSchoolEmojiCount; i++) {
        int col = i % kCols, row = i / kCols;
        D2D1_RECT_F cellRect = D2D1::RectF(
            card.left + pad + col * (cell + cellGap), card.top + pad + row * (cell + cellGap),
            card.left + pad + col * (cell + cellGap) + cell, card.top + pad + row * (cell + cellGap) + cell);
        roundedRect(cellRect, 8, hex(kHyoBlue, 0.08f));
        text(kSchoolEmojis[i], cellRect, g->fmtBody, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        g->memoEmojiRects[i] = cellRect;
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
    float rowH = 60; // roomier now that the delete icon box is 1.5x bigger
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
            text(g->profiles[i].name, D2D1::RectF(row.left + 14, row.top, row.right - 58, row.bottom),
                g->fmtBody, pal.textPrimary);

            // Icon box is 1.5x the original 26px, matching the other enlarged icon boxes.
            D2D1_RECT_F delBtn = D2D1::RectF(row.right - 47, row.top + (row.bottom - row.top - 39) / 2,
                row.right - 8, row.top + (row.bottom - row.top - 39) / 2 + 39);
            roundedRect(delBtn, 12, withAlpha(pal.error, 0.14f));
            text(L"", delBtn, g->fmtIconBox, pal.error, DWRITE_TEXT_ALIGNMENT_CENTER);

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
    if (periodShow != g->periodFieldsShownApplied) {
        ShowWindow(g->hEditLabel, periodShow);
        ShowWindow(g->hEditSubject, periodShow);
        ShowWindow(g->hComboStartHour, periodShow);
        ShowWindow(g->hComboStartMinute, periodShow);
        ShowWindow(g->hComboEndHour, periodShow);
        ShowWindow(g->hComboEndMinute, periodShow);
        g->periodFieldsShownApplied = periodShow;
    }
    if (showPeriodFields) {
        auto place = [](HWND h, D2D1_RECT_F r, D2D1_RECT_F& applied) {
            if (r.left == applied.left && r.top == applied.top && r.right == applied.right && r.bottom == applied.bottom) return;
            SetWindowPos(h, nullptr, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top),
                SWP_NOZORDER | SWP_NOACTIVATE);
            applied = r;
        };
        // Dropdown-list combos need extra window height for the popup list itself —
        // only the top sliver (matching the field rect) shows while closed. Also
        // guarded by the applied-rect check above: reapplying this (even with
        // identical values) while the drop-down list is open closes it instantly,
        // which is why an unrelated redraw (e.g. mouse hover elsewhere on the
        // still-tracked, now-hidden schedule list underneath) used to yank the
        // combo's drop-down away mid-selection.
        auto placeCombo = [](HWND h, D2D1_RECT_F r, D2D1_RECT_F& applied) {
            if (r.left == applied.left && r.top == applied.top && r.right == applied.right && r.bottom == applied.bottom) return;
            SetWindowPos(h, nullptr, (int)r.left, (int)r.top, (int)(r.right - r.left), 300, SWP_NOZORDER | SWP_NOACTIVATE);
            applied = r;
        };
        place(g->hEditLabel, g->rectFieldLabel, g->rectFieldLabelApplied);
        place(g->hEditSubject, g->rectFieldSubject, g->rectFieldSubjectApplied);
        placeCombo(g->hComboStartHour, g->rectFieldStartHour, g->rectFieldStartHourApplied);
        placeCombo(g->hComboStartMinute, g->rectFieldStartMinute, g->rectFieldStartMinuteApplied);
        placeCombo(g->hComboEndHour, g->rectFieldEndHour, g->rectFieldEndHourApplied);
        placeCombo(g->hComboEndMinute, g->rectFieldEndMinute, g->rectFieldEndMinuteApplied);
    }

    bool showProfileName = g->savingProfile;
    ShowWindow(g->hEditProfileName, showProfileName ? SW_SHOWNA : SW_HIDE);
    if (showProfileName) {
        SetWindowPos(g->hEditProfileName, nullptr, (int)g->rectProfileNameField.left, (int)g->rectProfileNameField.top,
            (int)(g->rectProfileNameField.right - g->rectProfileNameField.left),
            (int)(g->rectProfileNameField.bottom - g->rectProfileNameField.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The memo is a permanent part of the layout (not a popup-triggered field),
    // so it's normally always visible -- repositioned only when the target rect
    // actually changes (resize/fullscreen toggle). Calling SetWindowPos
    // unconditionally on every WM_PAINT caused a self-perpetuating repaint storm
    // that starved WM_TIMER entirely (the child reposition was itself
    // invalidating the WS_CLIPCHILDREN parent, queuing another WM_PAINT before
    // the message loop ever got back around to the timer message).
    // Exception: any of the other modal popups (period editor, save/load
    // profile) darken the whole screen with a translucent Direct2D overlay,
    // but the memo is a real child HWND -- it isn't part of that D2D surface,
    // so it would otherwise keep rendering (and taking clicks) right through
    // the overlay. Hide it while a modal is up.
    bool anyModalOpen = g->editingPeriodIndex != -1 || g->savingProfile || g->loadingProfile;
    if (g->hRichMemo) {
        int wantHidden = anyModalOpen ? 1 : 0;
        if (wantHidden != g->memoHiddenApplied) {
            ShowWindow(g->hRichMemo, anyModalOpen ? SW_HIDE : SW_SHOWNA);
            g->memoHiddenApplied = wantHidden;
        }
        if (anyModalOpen) return;
        const D2D1_RECT_F& r = g->rectMemoEdit;
        D2D1_RECT_F& applied = g->rectMemoEditApplied;
        if (r.left != applied.left || r.top != applied.top || r.right != applied.right || r.bottom != applied.bottom) {
            SetWindowPos(g->hRichMemo, nullptr, (int)r.left, (int)r.top,
                (int)(r.right - r.left), (int)(r.bottom - r.top), SWP_NOZORDER | SWP_NOACTIVATE);
            applied = r;
        }
        int wantReadOnly = g->fullscreen ? 1 : 0;
        if (wantReadOnly != g->memoReadOnlyApplied) {
            SendMessageW(g->hRichMemo, EM_SETREADONLY, wantReadOnly, 0);
            g->memoReadOnlyApplied = wantReadOnly;
        }
        // Sunken 3D edge reads as "windowed app chrome" -- drop it in fullscreen
        // so the memo box blends into the clean signage display.
        int wantBorder = g->fullscreen ? 0 : 1;
        if (wantBorder != g->memoBorderApplied) {
            LONG_PTR ex = GetWindowLongPtrW(g->hRichMemo, GWL_EXSTYLE);
            ex = wantBorder ? (ex | WS_EX_CLIENTEDGE) : (ex & ~WS_EX_CLIENTEDGE);
            SetWindowLongPtrW(g->hRichMemo, GWL_EXSTYLE, ex);
            SetWindowPos(g->hRichMemo, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            g->memoBorderApplied = wantBorder;
        }
    }
}

// ---- Free-write memo persistence (RTF, so bold/underline/size/colors survive) ----

std::wstring memoRtfPath() {
    PWSTR appData = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = appData;
        CoTaskMemFree(appData);
    }
    dir += L"\\HyoExam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\memo.rtf";
}

// EM_STREAMOUT callback: appends each chunk RichEdit hands us into a std::string.
DWORD CALLBACK memoStreamOutCallback(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    auto* out = reinterpret_cast<std::string*>(cookie);
    out->append(reinterpret_cast<char*>(buf), cb);
    *pcb = cb;
    return 0;
}

// EM_STREAMIN callback: feeds RichEdit the next chunk from a std::string + read offset.
DWORD CALLBACK memoStreamInCallback(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    auto* state = reinterpret_cast<std::pair<const std::string*, size_t>*>(cookie);
    const std::string& src = *state->first;
    size_t remaining = src.size() - state->second;
    LONG n = (LONG)std::min<size_t>(remaining, (size_t)cb);
    if (n > 0) memcpy(buf, src.data() + state->second, n);
    state->second += n;
    *pcb = n;
    return 0;
}

void saveMemoRtf() {
    if (!g->hRichMemo) return;
    std::string rtf;
    EDITSTREAM es{ (DWORD_PTR)&rtf, 0, memoStreamOutCallback };
    SendMessageW(g->hRichMemo, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    std::ofstream f(memoRtfPath(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(rtf.data(), (std::streamsize)rtf.size());
    SendMessageW(g->hRichMemo, EM_SETMODIFY, FALSE, 0);
}

void loadMemoRtf() {
    if (!g->hRichMemo) return;
    std::ifstream f(memoRtfPath(), std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string rtf = ss.str();
    if (rtf.empty()) return;
    std::pair<const std::string*, size_t> state{ &rtf, 0 };
    EDITSTREAM es{ (DWORD_PTR)&state, 0, memoStreamInCallback };
    SendMessageW(g->hRichMemo, EM_STREAMIN, SF_RTF, (LPARAM)&es);
}

// ---- Memo formatting toolbar actions (bold/underline/size/colors on selection) ----

CHARFORMAT2W getMemoSelectionFormat() {
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    SendMessageW(g->hRichMemo, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    return cf;
}

// Called each frame (windowed only) so the B/U/alignment buttons reflect the
// format at the current selection/caret instead of a stale toggle guess.
void refreshMemoToolbarState() {
    CHARFORMAT2W cf = getMemoSelectionFormat();
    g->memoBoldActive = (cf.dwMask & CFM_BOLD) && (cf.dwEffects & CFE_BOLD);
    g->memoUnderlineActive = (cf.dwMask & CFM_UNDERLINE) && (cf.dwEffects & CFE_UNDERLINE);

    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    SendMessageW(g->hRichMemo, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    g->memoAlign = (pf.dwMask & PFM_ALIGNMENT) ? pf.wAlignment : PFA_LEFT;
}

void setMemoAlign(WORD align) {
    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_ALIGNMENT;
    pf.wAlignment = align;
    SendMessageW(g->hRichMemo, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SetFocus(g->hRichMemo);
}

void toggleMemoBold() {
    bool isBold = g->memoBoldActive;
    CHARFORMAT2W set{};
    set.cbSize = sizeof(set);
    set.dwMask = CFM_BOLD;
    set.dwEffects = isBold ? 0 : CFE_BOLD;
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&set);
    SetFocus(g->hRichMemo);
}

void toggleMemoUnderline() {
    bool isUnderline = g->memoUnderlineActive;
    CHARFORMAT2W set{};
    set.cbSize = sizeof(set);
    set.dwMask = CFM_UNDERLINE;
    set.dwEffects = isUnderline ? 0 : CFE_UNDERLINE;
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&set);
    SetFocus(g->hRichMemo);
}

// +/-2pt per click, applied to the selection (or the caret's forward-typing
// format when nothing is selected — standard EM_SETCHARFORMAT/SCF_SELECTION
// behavior for an empty selection).
void adjustMemoFontSize(int deltaPt) {
    CHARFORMAT2W cf = getMemoSelectionFormat();
    LONG currentTwips = (cf.dwMask & CFM_SIZE) ? cf.yHeight : 200; // 10pt fallback if mixed/unset
    LONG newTwips = std::clamp<LONG>(currentTwips + deltaPt * 20, 8 * 20, 96 * 20);
    CHARFORMAT2W set{};
    set.cbSize = sizeof(set);
    set.dwMask = CFM_SIZE;
    set.yHeight = newTwips;
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&set);
    SetFocus(g->hRichMemo);
}

// Native color picker for both text and highlight color — avoids hand-rolling a
// swatch palette, and CHOOSECOLORW's custom-colors row persists across calls.
void pickMemoColor(bool background) {
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g->hRichMemo;
    cc.lpCustColors = g->memoCustomColors;
    cc.rgbResult = background ? RGB(255, 255, 0) : RGB(0, 0, 0);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    CHARFORMAT2W set{};
    set.cbSize = sizeof(set);
    if (background) {
        set.dwMask = CFM_BACKCOLOR;
        set.crBackColor = cc.rgbResult;
    } else {
        set.dwMask = CFM_COLOR;
        set.crTextColor = cc.rgbResult;
    }
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&set);
    SetFocus(g->hRichMemo);
}

// Inserts one curated emoji at the current selection/caret. Same-process
// EM_REPLACESEL call, no marshaling concerns.
void insertMemoEmoji(const wchar_t* emoji) {
    SendMessageW(g->hRichMemo, EM_REPLACESEL, TRUE, (LPARAM)emoji);
    SetFocus(g->hRichMemo);
}

// Canvas background follows the theme. Text color is forced across the whole
// document (SCF_ALL), not just SCF_DEFAULT: RichEdit only honors SCF_DEFAULT
// for typing into a genuinely empty document -- once any character exists,
// new text typed next to it inherits THAT run's (possibly stale, wrong-theme)
// color instead. Since the memo reloads saved content on every launch, it's
// effectively never empty, so SCF_DEFAULT alone left both old AND newly
// typed text stuck in whatever color it was saved in -- invisible against an
// opposite-theme background. Trade-off: this also overwrites any per-run
// color the user picked via pickMemoColor(false), since there's no cheap way
// to tell "still-default" runs apart from "deliberately colored" ones without
// walking the document run by run.
void applyMemoTheme() {
    if (!g->hRichMemo) return;
    Palette pal = currentPalette();
    COLORREF bg = RGB((BYTE)(pal.surface.r * 255), (BYTE)(pal.surface.g * 255), (BYTE)(pal.surface.b * 255));
    SendMessageW(g->hRichMemo, EM_SETBKGNDCOLOR, 0, (LPARAM)bg);

    COLORREF fg = RGB((BYTE)(pal.textPrimary.r * 255), (BYTE)(pal.textPrimary.g * 255), (BYTE)(pal.textPrimary.b * 255));
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = fg;
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf); // forward-typing baseline for a still-empty doc
    SendMessageW(g->hRichMemo, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);     // and everything already on the page
}

// Plain Edit/ComboBox controls (period-editor fields, save-profile name field)
// don't pick up the app's theme on their own -- they paint themselves via
// system colors unless told otherwise. WM_CTLCOLOREDIT/WM_CTLCOLORLISTBOX
// (handled in WndProc) cover the Edit fields' background/text; SetWindowTheme's
// "DarkMode_CFD" class (needs the comctl32 v6 manifest dependency to take
// effect) re-skins the ComboBoxes themselves, including their closed-state
// display, which no CTLCOLOR message reaches for CBS_DROPDOWNLIST.
void applyNativeControlsTheme() {
    Palette pal = currentPalette();
    if (g->hNativeCtlBgBrush) { DeleteObject(g->hNativeCtlBgBrush); g->hNativeCtlBgBrush = nullptr; }
    COLORREF bg = RGB((BYTE)(pal.surface.r * 255), (BYTE)(pal.surface.g * 255), (BYTE)(pal.surface.b * 255));
    g->hNativeCtlBgBrush = CreateSolidBrush(bg);

    const wchar_t* comboTheme = isEffectivelyLight() ? L"Explorer" : L"DarkMode_CFD";
    HWND combos[] = { g->hComboStartHour, g->hComboStartMinute, g->hComboEndHour, g->hComboEndMinute };
    for (HWND h : combos) if (h) SetWindowTheme(h, comboTheme, nullptr);
}

// The memo is read-only in fullscreen, but a plain RichEdit still happily
// takes focus and blinks a caret wherever you click -- distracting on a
// clean student-facing display that should show only the written text and
// emoji. Subclassing lets us swallow the clicks/focus that would otherwise
// place that caret, without touching the control's normal windowed behavior.
LRESULT CALLBACK RichMemoSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g->fullscreen) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
            return 0; // no click-to-place-caret while fullscreen
        case WM_SETFOCUS:
            HideCaret(hwnd);
            SetFocus(GetParent(hwnd)); // bounce focus straight back to the main window
            return 0;
        }
    }
    return CallWindowProcW(g->richMemoDefaultProc, hwnd, msg, wParam, lParam);
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

    float pad = 48;    // horizontal margin (left/right) — unchanged
    float padV = 24;   // vertical margin (top/bottom) — halved from the old 48 to cut the dead space above/below the cards
    float gap = 24;
    // Fullscreen is the clean student-facing display: no admin toolbar, no footer.
    float footerHeight = g->fullscreen ? 0.0f : 40.0f;
    // 54px icon boxes need at least that much room in the toolbar band.
    float topBarHeight = g->fullscreen ? 0.0f : 70.0f;

    // Content area = everything below the admin toolbar and above the footer.
    D2D1_RECT_F content = D2D1::RectF(pad, padV + topBarHeight, size.width - pad, size.height - padV - footerHeight);

    // Admin toolbar sits above both cards -- global app controls, not tied to
    // either panel. Hidden entirely in fullscreen (student-facing display).
    if (!g->fullscreen) {
        // Unified 12px spacing grid for the whole toolbar row (icon-to-icon,
        // tab-to-tab, tab-to-dropdown), and icons/tabs share one vertically
        // centered baseline within the topBarHeight band instead of both
        // being pinned to its top edge. Icon boxes are 1.5x the original 36px.
        float iconSize = 54, iconGap = 12;
        float iconsRight = size.width - pad;
        float iconTop = padV + (topBarHeight - iconSize) / 2.0f;

        // Right side: theme toggle immediately left of fullscreen.
        g->rectFullscreenBtn = D2D1::RectF(iconsRight - iconSize, iconTop, iconsRight, iconTop + iconSize);
        roundedRect(g->rectFullscreenBtn, 10, hex(kHyoBlue, 0.12f));
        text(L"", g->rectFullscreenBtn, g->fmtIconBox, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        g->rectThemeToggle = D2D1::RectF(iconsRight - iconSize * 2 - iconGap, iconTop, iconsRight - iconSize - iconGap, iconTop + iconSize);
        roundedRect(g->rectThemeToggle, 10, hex(kHyoBlue, 0.12f));
        text(isEffectivelyLight() ? L"" : L"", g->rectThemeToggle, g->fmtIconBox, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Save / load named profiles, left of the theme toggle.
        g->rectLoadIcon = D2D1::RectF(iconsRight - iconSize * 3 - iconGap * 2, iconTop, iconsRight - iconSize * 2 - iconGap * 2, iconTop + iconSize);
        roundedRect(g->rectLoadIcon, 10, hex(kHyoBlue, 0.12f));
        text(L"", g->rectLoadIcon, g->fmtIconBox, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        g->rectSaveIcon = D2D1::RectF(iconsRight - iconSize * 4 - iconGap * 3, iconTop, iconsRight - iconSize * 3 - iconGap * 3, iconTop + iconSize);
        roundedRect(g->rectSaveIcon, 10, hex(kHyoBlue, 0.12f));
        text(L"", g->rectSaveIcon, g->fmtIconBox, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Left side toolbar boxes, all one uniform width/gap: exam-type tabs
        // (모의고사 / 지필평가) → 화면구성 (구 "표시") → 시간 기준 선택, in that order.
        constexpr float kToolboxW = 130;
        g->scheduleButtons.clear();
        float bx = pad;
        for (auto& sc : g->scheduleStore.all()) {
            D2D1_RECT_F r = D2D1::RectF(bx, iconTop, bx + kToolboxW, iconTop + iconSize);
            bool isActive = sc.id == g->scheduleStore.activeId();
            roundedRect(r, 10, isActive ? hex(kHyoBlue, 0.22f) : hex(kHyoBlue, 0.10f), isActive ? nullptr : &pal.cardBorder);
            text(sc.name, r, g->fmtSmall, isActive ? hex(kHyoBlue) : pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
            g->scheduleButtons.push_back({ r, sc.id });
            bx += kToolboxW + iconGap;
        }

        // 화면구성 (fullscreen display-toggle) button, right after the exam-type tabs.
        g->rectDisplaySettingsButton = D2D1::RectF(bx, iconTop, bx + kToolboxW, iconTop + iconSize);
        roundedRect(g->rectDisplaySettingsButton, 10,
            g->displaySettingsOpen ? hex(kHyoBlue, 0.22f) : hex(kHyoBlue, 0.10f),
            g->displaySettingsOpen ? nullptr : &pal.cardBorder);
        text(L"화면구성", g->rectDisplaySettingsButton, g->fmtSmall,
            g->displaySettingsOpen ? hex(kHyoBlue) : pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        bx += kToolboxW + iconGap;

        // Time-source dropdown button, right after 화면구성.
        g->rectTimeSourceButton = D2D1::RectF(bx, iconTop, bx + kToolboxW, iconTop + iconSize);
        roundedRect(g->rectTimeSourceButton, 10, hex(kHyoBlue, 0.10f), &pal.cardBorder);
        text(kTimeSources[g->settings.timeSourceIndex].label,
            D2D1::RectF(g->rectTimeSourceButton.left + 14, g->rectTimeSourceButton.top,
                g->rectTimeSourceButton.right - 24, g->rectTimeSourceButton.bottom),
            g->fmtSmall, pal.textSecondary);
        text(L"", D2D1::RectF(g->rectTimeSourceButton.right - 26, g->rectTimeSourceButton.top,
                g->rectTimeSourceButton.right - 4, g->rectTimeSourceButton.bottom),
            g->fmtIcon, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        // Fullscreen still exposes one unobtrusive "return to window" icon,
        // top-right corner, for mouse-only setups (no keyboard for Esc/F11).
        // Hidden until the mouse moves, then fades back out after 5s idle (see
        // the fade-out block near the end of drawFrame) so it doesn't clutter
        // the clean signage display.
        // Only the hit-rect is computed here -- the actual drawing happens near
        // the end of drawFrame, after the clock card's rounded-corner fill
        // would otherwise paint right over this same corner and hide it.
        float iconSize = 54;
        g->rectFullscreenBtn = D2D1::RectF(size.width - pad - iconSize, padV, size.width - pad, padV + iconSize);
        g->rectThemeToggle = D2D1::RectF(0, 0, 0, 0);
        g->rectSaveIcon = D2D1::RectF(0, 0, 0, 0);
        g->rectLoadIcon = D2D1::RectF(0, 0, 0, 0);
        g->scheduleButtons.clear();
        g->rectTimeSourceButton = D2D1::RectF(0, 0, 0, 0);
        g->timeSourceDropdownOpen = false;
        g->rectDisplaySettingsButton = D2D1::RectF(0, 0, 0, 0);
        g->displaySettingsOpen = false;
    }

    // Left column: clock (+ memo below it). Right column: today's period
    // timetable. Fixed split (60/40) tuned for TV/projector signage -- no
    // longer drags/persists per-user. In fullscreen the clock is always shown;
    // the schedule panel and memo card are independently opt-in (표시 dropdown)
    // -- when one is off, the other side/dimension simply expands to fill it.
    bool showSchedule = !g->fullscreen || g->settings.fullscreenShowSchedule;
    bool showMemo = !g->fullscreen || g->settings.fullscreenShowMemo;

    constexpr float kClockPanelRatio = 0.60f;
    float totalWidth = content.right - content.left;
    D2D1_RECT_F leftCard, rightCard;
    if (showSchedule) {
        float leftWidth = totalWidth * kClockPanelRatio - gap / 2;
        leftCard = D2D1::RectF(content.left, content.top, content.left + leftWidth, content.bottom);
        rightCard = D2D1::RectF(leftCard.right + gap, content.top, content.right, content.bottom);
    } else {
        leftCard = content;
        rightCard = D2D1::RectF(0, 0, 0, 0);
    }

    // Clock card on top, free-write memo card below it -- fixed 70/30 vertical split.
    constexpr float kClockVerticalRatio = 0.70f;
    D2D1_RECT_F clockCard, memoCard;
    if (showMemo) {
        float leftHeight = leftCard.bottom - leftCard.top;
        float clockCardH = leftHeight * kClockVerticalRatio - gap / 2;
        clockCard = D2D1::RectF(leftCard.left, leftCard.top, leftCard.right, leftCard.top + clockCardH);
        memoCard = D2D1::RectF(leftCard.left, clockCard.bottom + gap, leftCard.right, leftCard.bottom);
    } else {
        clockCard = leftCard;
        memoCard = D2D1::RectF(0, 0, 0, 0);
    }

    // The height-only pass in buildFullscreenFonts() can size the clock too wide
    // for narrower card ratios / aspect ratios (it doesn't know the card width
    // yet). Re-check against the real left-card width here and shrink if needed
    // — this is what actually prevents the "HH:MM:SS" line from wrapping/clipping.
    // Re-derived from fsClockBaseSize (the height-only baseline), not from
    // fClock's current size — otherwise toggling the 표시 schedule panel off
    // (freeing up width) could never grow the clock back, only ever shrink it
    // further on each round trip.
    if (g->fullscreen) {
        float cardWidth = leftCard.right - leftCard.left;
        if (g->fsClockCorrectedForWidth != cardWidth) {
            const float kClockCharCount = 8.0f;      // "HH:MM:SS"
            const float kClockAdvanceEm = 0.58f;     // approx. digit advance as a fraction of em, Malgun Gothic Bold
            float widthFitSize = (cardWidth * 0.92f) / (kClockCharCount * kClockAdvanceEm);
            float appliedSize = std::min(g->fsClockBaseSize, std::clamp(widthFitSize, 60.0f, 520.0f));
            if (appliedSize != fClock->GetFontSize()) {
                fClock->Release();
                fClock = makeFormat(kUiFontFamily, appliedSize, DWRITE_FONT_WEIGHT_BOLD);
                fClock->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                g->fmtClockFS = fClock;
            }
            g->fsClockCorrectedForWidth = cardWidth;
        }
    }

    const ExamSchedule* active = g->scheduleStore.active();

    // ---- Left: clock ----
    roundedRect(clockCard, 16, pal.surface, &pal.cardBorder);
    float cardPad = g->fullscreen ? 32.0f : 24.0f;
    // The sync-status line is admin-facing chrome (which source, synced/not) —
    // fullscreen is the clean student-facing display, so it's windowed-only.
    if (!g->fullscreen) {
        float syncLabelH = fSmall->GetFontSize() * 1.6f;
        std::wstring syncLabel = std::wstring(kTimeSources[g->settings.timeSourceIndex].label) +
            (g->timeSync.isSynced() ? L" 시간 동기화됨" : L" 동기화 대기중 (로컬 시간)");
        text(L"● " + syncLabel,
            D2D1::RectF(clockCard.left + cardPad, clockCard.top + cardPad, clockCard.right - cardPad, clockCard.top + cardPad + syncLabelH),
            fSmall, g->timeSync.isSynced() ? hex(kTeal) : hex(kOrange), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Date/weekday on top, big clock below it — the whole block is centered as
    // a unit within the card (both vertically and horizontally) using the
    // actual chosen font sizes, so fullscreen's much larger clock digits stay
    // visually balanced instead of sitting at windowed-mode offsets.
    float clockCenterY = clockCard.top + (clockCard.bottom - clockCard.top) / 2.0f;
    float dateLineH = fDate->GetFontSize() * 1.3f;
    float clockLineH = fClock->GetFontSize() * 1.15f;
    // Coefficient tuned so the gap ends up half of what it was before the date
    // font itself was made 2.5x bigger (0.6 * 0.5 / 2.5 = 0.12).
    float blockGap = fDate->GetFontSize() * 0.12f;
    float blockTotalH = dateLineH + blockGap + clockLineH;
    float blockTop = clockCenterY - blockTotalH / 2.0f;
    text(formatDate(st), D2D1::RectF(clockCard.left, blockTop, clockCard.right, blockTop + dateLineH),
        fDate, pal.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(formatClock(st), D2D1::RectF(clockCard.left, blockTop + dateLineH + blockGap, clockCard.right, blockTop + blockTotalH),
        fClock, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- Memo card (free-write area under the clock) ----
    if (showMemo) {
        roundedRect(memoCard, 16, pal.surface, &pal.cardBorder);
        float mx = memoCard.left + cardPad * 0.6f;
        float toolbarH = g->fullscreen ? 0.0f : 44.0f;
        if (!g->fullscreen) {
            refreshMemoToolbarState();
            float btn = 32, btnGap = 8;
            float by = memoCard.top + (toolbarH - btn) / 2.0f;
            float bx = mx;
            // Icon language modeled on 한글(HWP)'s "글자 모양" toolbar: a "가"
            // glyph carries the weight/underline/color cue directly (bold
            // weight, underline stroke, color bar) instead of English-letter
            // buttons, and alignment is 3 bars in the actual left/center/right
            // position rather than "L"/"C"/"R" text.
            auto memoBtnBox = [&](D2D1_RECT_F& rect, bool active) -> D2D1_RECT_F {
                rect = D2D1::RectF(bx, by, bx + btn, by + btn);
                roundedRect(rect, 8, active ? hex(kHyoBlue, 0.30f) : hex(kHyoBlue, 0.10f));
                bx += btn + btnGap;
                return rect;
            };
            auto drawAlignBars = [&](D2D1_RECT_F r, WORD mode) {
                float barH = 3, rowGap = 4;
                float fullW = (r.right - r.left) - 12;
                float shortW = fullW * 0.6f;
                float y = r.top + ((r.bottom - r.top) - (barH * 3 + rowGap * 2)) / 2.0f;
                for (int line = 0; line < 3; line++) {
                    float w = (line == 1) ? shortW : fullW;
                    float x = mode == PFA_LEFT ? r.left + 6
                        : mode == PFA_RIGHT ? r.right - 6 - w
                        : r.left + 6 + (fullW - w) / 2.0f;
                    roundedRect(D2D1::RectF(x, y, x + w, y + barH), 1, hex(kHyoBlue));
                    y += barH + rowGap;
                }
            };

            { D2D1_RECT_F r = memoBtnBox(g->rectMemoBold, g->memoBoldActive);
              text(L"가", r, g->fmtSmallBold, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoUnderline, g->memoUnderlineActive);
              text(L"가", r, g->fmtSmall, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
              float lineY = r.bottom - 8;
              g->renderTarget->DrawLine(D2D1::Point2F(r.left + 8, lineY), D2D1::Point2F(r.right - 8, lineY), brush(hex(kHyoBlue)), 1.5f); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoAlignLeft, g->memoAlign == PFA_LEFT); drawAlignBars(r, PFA_LEFT); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoAlignCenter, g->memoAlign == PFA_CENTER); drawAlignBars(r, PFA_CENTER); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoAlignRight, g->memoAlign == PFA_RIGHT); drawAlignBars(r, PFA_RIGHT); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoSizeDown, false);
              text(L"A-", r, g->fmtSmall, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoSizeUp, false);
              text(L"A+", r, g->fmtSmall, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoFontColor, false);
              text(L"가", r, g->fmtSmall, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
              roundedRect(D2D1::RectF(r.left + 6, r.bottom - 7, r.right - 6, r.bottom - 4), 1, hex(0xE03A3A)); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoBgColor, false);
              text(L"가", r, g->fmtSmall, pal.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
              roundedRect(D2D1::RectF(r.left + 6, r.bottom - 7, r.right - 6, r.bottom - 4), 1, hex(0xFFD54A)); }
            { D2D1_RECT_F r = memoBtnBox(g->rectMemoEmoji, false);
              text(L"🙂", r, g->fmtBody, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER); }
        } else {
            g->rectMemoBold = g->rectMemoUnderline = D2D1::RectF(0, 0, 0, 0);
            g->rectMemoAlignLeft = g->rectMemoAlignCenter = g->rectMemoAlignRight = D2D1::RectF(0, 0, 0, 0);
            g->rectMemoSizeDown = g->rectMemoSizeUp = D2D1::RectF(0, 0, 0, 0);
            g->rectMemoFontColor = g->rectMemoBgColor = D2D1::RectF(0, 0, 0, 0);
            g->rectMemoEmoji = D2D1::RectF(0, 0, 0, 0);
        }
        g->rectMemoEdit = D2D1::RectF(mx, memoCard.top + toolbarH + (g->fullscreen ? cardPad * 0.4f : 4.0f),
            memoCard.right - cardPad * 0.6f, memoCard.bottom - cardPad * 0.6f);
    } else {
        g->rectMemoBold = g->rectMemoUnderline = D2D1::RectF(0, 0, 0, 0);
        g->rectMemoAlignLeft = g->rectMemoAlignCenter = g->rectMemoAlignRight = D2D1::RectF(0, 0, 0, 0);
        g->rectMemoSizeDown = g->rectMemoSizeUp = D2D1::RectF(0, 0, 0, 0);
        g->rectMemoFontColor = g->rectMemoBgColor = D2D1::RectF(0, 0, 0, 0);
        g->rectMemoEmoji = D2D1::RectF(0, 0, 0, 0);
        g->rectMemoEdit = D2D1::RectF(0, 0, 0, 0); // shrinks the native RichEdit to nothing via syncNativeControls
    }

    // ---- Right: timetable ----
    if (showSchedule) {
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

        // Fullscreen sizes the period title/time text off the actual row height
        // (rebuilt lazily when rowH changes) so a handful of periods — large
        // rows — read as big, filled cards instead of small fixed-size text
        // floating in a mostly-empty box; windowed mode keeps its fixed sizes.
        if (g->fullscreen && g->fsPeriodBuiltForRowH != rowH) buildPeriodRowFonts(rowH);
        IDWriteTextFormat* rowTitleFmt = g->fullscreen ? g->fmtPeriodTitleFS : fBody;
        IDWriteTextFormat* rowTimeFmt = g->fullscreen ? g->fmtPeriodTimeFS : g->fmtPeriodTimeWin;
        // Small fixed gap between period rows — a little breathing room without
        // eating into the now-uncapped fullscreen row height.
        float rowGapPx = g->fullscreen ? 20.0f : 8.0f;

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

            D2D1_RECT_F row = D2D1::RectF(rx, ry, rightCard.right - cardPad, ry + rowH - rowGapPx);

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
            } else if (g->fullscreen) {
                // Fullscreen has no hover/current-only affordance to fall back on, and
                // rows are now large (uncapped rowH) — an outlined card per period makes
                // that filled space read as one deliberate block instead of stray text
                // floating in empty background.
                roundedRect(row, 10, hex(kHyoBlue, 0.05f), &pal.cardBorder);
            }

            // Fullscreen rows are much wider/taller than windowed ones, so a flat
            // 14px left inset (tuned for the compact windowed list) reads as
            // cramped against the card edge -- scale it up for fullscreen.
            float textLeft = row.left + (g->fullscreen ? 40.0f : 14.0f);
            float textRight;
            if (g->fullscreen) {
                textRight = row.right - 32;
            } else {
                // Windowed 교시/시간 text is deliberately narrower than the row
                // itself -- half of the old full-row width, left-aligned.
                float fullTextRight = row.right - 46;
                textRight = textLeft + (fullTextRight - textLeft) / 2.0f;
            }

            // The height-only guess in buildPeriodRowFonts() can size the time
            // line too wide for narrower cards (it doesn't know the row width
            // yet); re-check against the real available width here and shrink
            // if needed so "HH:MM ~ HH:MM(NN분)" never wraps to a second line,
            // while leaving the little right margin textRight already reserves.
            // Re-derived from fsPeriodTimeBaseSize (the height-only baseline),
            // same grow-back reasoning as the clock's fsClockBaseSize above.
            if (g->fullscreen) {
                float availWidth = textRight - textLeft;
                if (g->fsPeriodTimeCorrectedForWidth != availWidth) {
                    const float kTimeCharCount = 19.0f;  // "00:00 ~ 00:00(999분)" worst case
                    const float kTimeAdvanceEm = 0.56f;  // approx. advance width as a fraction of em
                    float widthFitSize = availWidth / (kTimeCharCount * kTimeAdvanceEm);
                    float appliedSize = std::min(g->fsPeriodTimeBaseSize, std::clamp(widthFitSize, 16.0f, 110.0f));
                    if (appliedSize != rowTimeFmt->GetFontSize()) {
                        rowTimeFmt->Release();
                        rowTimeFmt = makeFormat(kUiFontFamily, appliedSize, DWRITE_FONT_WEIGHT_MEDIUM);
                        rowTimeFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        g->fmtPeriodTimeFS = rowTimeFmt;
                    }
                    g->fsPeriodTimeCorrectedForWidth = availWidth;
                }
            }

            // Title/time line positions scale off the actual chosen font sizes
            // (rather than fixed 6/34/36/14px offsets) so fullscreen's larger
            // rowTitleFmt/rowTimeFmt don't clip or crowd inside the also-larger rowH.
            // In fullscreen the text block is centered within the (large,
            // uncapped) row rather than pinned near the top, since the row IS
            // the period's whole visual block now.
            float titleH = rowTitleFmt->GetFontSize() * 1.3f;
            float timeH = rowTimeFmt->GetFontSize() * 1.3f;
            float lineGap = rowTitleFmt->GetFontSize() * (g->fullscreen ? 0.11f : 0.22f);
            float titleTop = g->fullscreen
                ? row.top + (rowH - rowGapPx - (titleH + lineGap + timeH)) / 2.0f
                : row.top + rowH * 0.10f;
            float timeTop = titleTop + titleH + lineGap;
            // Time/subject colors swapped from the original design: the period
            // time now carries the stronger (primary) color and the label/subject
            // the softer (secondary) one -- matches how this schedule is actually
            // scanned (time first).
            text(p.label + L" " + p.subject, D2D1::RectF(textLeft, titleTop, textRight, titleTop + titleH),
                rowTitleFmt, isCurrent ? hex(kHyoBlue) : pal.textSecondary);
            text(p.start.format() + L" ~ " + p.end.format() + L"(" + std::to_wstring(p.durationMinutes) + L"분)",
                D2D1::RectF(textLeft, timeTop, textRight, timeTop + timeH),
                rowTimeFmt, pal.textPrimary);

            if (!g->fullscreen) {
                // Delete-X hit box is 1.5x the original 26px, matching the other enlarged icon boxes.
                float rowCenterY = (row.top + row.bottom) / 2.0f;
                D2D1_RECT_F delBtn = D2D1::RectF(row.right - 45, rowCenterY - 19.5f, row.right - 6, rowCenterY + 19.5f);
                roundedRect(delBtn, 12, withAlpha(pal.error, 0.14f));
                text(L"", delBtn, g->fmtIconBox, pal.error, DWRITE_TEXT_ALIGNMENT_CENTER);
                g->periodDeleteRects[idx] = delBtn;
            }
            g->periodRowRects[idx] = row;

            ry += rowH;
        }

        if (!g->fullscreen) {
            g->rectAddPeriod = D2D1::RectF(rx, ry, rightCard.right - cardPad, ry + rowH - rowGapPx);
            roundedRect(g->rectAddPeriod, 10, hex(kHyoBlue, 0.10f), &pal.cardBorder);
            text(L"+  교시 추가", g->rectAddPeriod, g->fmtBody, hex(kHyoBlue), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            g->rectAddPeriod = D2D1::RectF(0, 0, 0, 0);
        }
    } else {
        text(L"시험 일정 없음", D2D1::RectF(rx, ry, rightCard.right - cardPad, ry + fBody->GetFontSize() * 1.5f), fBody, pal.textTertiary);
    }
    } else {
        g->rectAddPeriod = D2D1::RectF(0, 0, 0, 0);
        g->periodRowRects.clear();
        g->periodDeleteRects.clear();
    }

    // Footer / about (verbatim brand format) — windowed/admin view only; the
    // fullscreen student display stays clean with no small print. The site
    // link is a separate clickable segment so its hit-rect is exact.
    if (!g->fullscreen) {
        float footerY = size.height - padV - 24;
        float footerBottom = size.height - padV;

        // House icon sits in front of the hyot.dev text; both share one
        // clickable hit-rect.
        g->rectSiteLink = D2D1::RectF(size.width - pad - 96, footerY, size.width - pad, footerBottom);
        text(L"", D2D1::RectF(g->rectSiteLink.left, footerY, g->rectSiteLink.left + 20, footerBottom),
            g->fmtIcon, g->hoveredSiteLink ? hex(kHyoBlue) : pal.textTertiary);
        text(L"hyot.dev", D2D1::RectF(g->rectSiteLink.left + 22, footerY, g->rectSiteLink.right, footerBottom),
            g->fmtVersion, g->hoveredSiteLink ? hex(kHyoBlue) : pal.textTertiary);

        std::wstring footer = L"HyoExam v" + std::wstring(kAppVersion) + L" · © 2026 HyoT. All rights reserved.  ·";
        text(footer, D2D1::RectF(size.width - pad - 480, footerY, g->rectSiteLink.left - 4, footerBottom),
            g->fmtVersion, pal.textTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Drawn last so the fullscreen return-to-window icon (rect computed earlier,
    // in the toolbar block) actually sits on top of the schedule card's fill
    // instead of being painted over by it. Hidden until the mouse moves, then
    // fades out after 5s of no further movement -- the mouse pointer itself
    // hides/shows in lockstep (see cursorHiddenInFullscreen).
    if (g->fullscreen) {
        ULONGLONG elapsed = g->fullscreenIconLastMoveMs == 0 ? MAXULONGLONG : GetTickCount64() - g->fullscreenIconLastMoveMs;
        const ULONGLONG visibleMs = 5000, fadeMs = 800;
        float alpha = elapsed < visibleMs ? 1.0f
            : elapsed < visibleMs + fadeMs ? 1.0f - (float)(elapsed - visibleMs) / (float)fadeMs
            : 0.0f;
        if (alpha > 0.001f) {
            roundedRect(g->rectFullscreenBtn, 10, hex(kHyoBlue, 0.16f * alpha));
            text(L"", g->rectFullscreenBtn, g->fmtIconBox, hex(kHyoBlue, 0.9f * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (!g->cursorHiddenInFullscreen) {
            ShowCursor(FALSE);
            g->cursorHiddenInFullscreen = true;
        }
    }

    if (g->editingPeriodIndex != -1) drawPeriodEditor(size);
    if (g->savingProfile) drawSaveProfilePopup(size);
    if (g->loadingProfile) drawLoadProfilePopup(size);

    // Small fade-out confirmation toast (e.g. after profile save). Time-based
    // rather than a fixed frame count so it fades the same regardless of the
    // 250ms tick rate.
    if (g->toastShownAtMs != 0) {
        ULONGLONG elapsed = GetTickCount64() - g->toastShownAtMs;
        const ULONGLONG visibleMs = 1400, fadeMs = 600, totalMs = visibleMs + fadeMs;
        if (elapsed < totalMs) {
            float alpha = elapsed < visibleMs ? 1.0f : 1.0f - (float)(elapsed - visibleMs) / (float)fadeMs;
            float toastW = 200, toastH = 56;
            float toastBottom = size.height - padV - footerHeight - 16;
            D2D1_RECT_F toastRect = D2D1::RectF((size.width - toastW) / 2, toastBottom - toastH,
                (size.width + toastW) / 2, toastBottom);
            roundedRect(toastRect, 14, hex(0x1F2937, 0.92f * alpha));
            text(g->toastText, toastRect, g->fmtSmall, hex(0xFFFFFF, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            g->toastShownAtMs = 0;
        }
    }
    if (g->timeSourceDropdownOpen) drawTimeSourceDropdown();
    if (g->displaySettingsOpen) drawDisplaySettingsDropdown();
    if (g->memoEmojiPickerOpen) drawMemoEmojiPicker();

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
        g->timeSourceDropdownOpen = false;
        g->displaySettingsOpen = false;
        g->memoEmojiPickerOpen = false;
        g->savingProfile = false;
        g->loadingProfile = false;
        g->fullscreenIconLastMoveMs = 0; // return icon starts hidden until the mouse actually moves
        // If the memo had focus (mid-edit right before hitting fullscreen), its
        // caret would otherwise keep blinking underneath the now-read-only control.
        if (g->hRichMemo && GetFocus() == g->hRichMemo) {
            HideCaret(g->hRichMemo);
            SetFocus(hwnd);
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g->fullscreen = false;
        // The cursor-hide is fullscreen-only chrome; always restore it on the way out.
        if (g->cursorHiddenInFullscreen) {
            ShowCursor(TRUE);
            g->cursorHiddenInFullscreen = false;
        }
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
    v.set("timeSourceIndex", Value::makeNumber(g->settings.timeSourceIndex));
    return v;
}

void applySettingsSnapshotJson(const hyo::json::Value& v) {
    std::string themeStr = v["theme"].asString("auto");
    g->settings.theme = themeStr == "light" ? Theme::Light : themeStr == "dark" ? Theme::Dark : Theme::Auto;
    g->settings.timeSourceIndex = (int)v["timeSourceIndex"].asNumber(0);
    if (g->settings.timeSourceIndex < 0 || g->settings.timeSourceIndex >= kTimeSourceCount) g->settings.timeSourceIndex = 0;
}

bool saveCurrentAsProfile(const std::wstring& name) {
    if (name.empty()) return false;
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
    return true;
}

void loadProfileByIndex(int idx) {
    if (idx < 0 || idx >= (int)g->profiles.size()) return;
    SavedProfile& sp = g->profiles[idx];
    applySettingsSnapshotJson(sp.settingsJson);
    g->scheduleStore.fromJson(sp.schedulesJson);
    // fromJson resets the active schedule from the profile's own JSON -- mirror
    // that back into settings so the persisted activeScheduleId doesn't drift
    // from what's actually shown (it's read back on next launch).
    g->settings.activeScheduleId = g->scheduleStore.activeId();
    g->settings.save();
    g->scheduleStore.saveToFile(g->dataPath);
    g->timeSync.setHost(kTimeSources[g->settings.timeSourceIndex].host);
    // A loaded profile can carry a different theme -- the memo box and the
    // native Edit/ComboBox controls only recolor when explicitly told to (the
    // theme toggle does this; loading a profile must too, or they keep the old
    // theme's colors until the next manual toggle).
    applyMemoTheme();
    applyNativeControlsTheme();
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
            DEFAULT_PITCH, kUiFontFamily);

        auto makeEdit = [&](int id, DWORD extraStyle) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | extraStyle, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)g->hUiFont, TRUE);
            return h;
        };
        g->hEditLabel = makeEdit(kIdEditLabel, ES_AUTOHSCROLL);
        g->hEditSubject = makeEdit(kIdEditSubject, ES_AUTOHSCROLL);
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
        applyNativeControlsTheme();

        // Free-write memo under the clock. Msftedit.dll registers RICHEDIT50W;
        // must be loaded before this CreateWindowExW call.
        LoadLibraryW(L"Msftedit.dll");
        g->hRichMemo = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdRichMemo, hInst, nullptr);
        g->richMemoDefaultProc = (WNDPROC)SetWindowLongPtrW(g->hRichMemo, GWLP_WNDPROC, (LONG_PTR)RichMemoSubclassProc);
        // Theme applied AFTER load: EM_STREAMIN (inside loadMemoRtf) replaces the
        // whole document, which would otherwise wipe out the SCF_ALL recolor below.
        loadMemoRtf();
        applyMemoTheme();

        return 0;
    }
    case WM_TIMER:
        // Skip the tick redraw while a native text field is up for editing: Direct2D's
        // full-surface present on every repaint fights the child Edit control's own
        // paint and blanks out whatever the user just typed. The explicit
        // InvalidateRect calls around each click already cover state changes.
        if (g->editingPeriodIndex == -1 && !g->savingProfile) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // Flush memo edits to disk at most once per tick instead of on every
        // keystroke. EM_GETMODIFY is RichEdit's own built-in dirty flag --
        // more reliable than trying to catch every EN_CHANGE via WM_COMMAND.
        if (g->hRichMemo && SendMessageW(g->hRichMemo, EM_GETMODIFY, 0, 0)) {
            saveMemoRtf();
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
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        // Edit fields (교시명/과목/저장 이름) and ComboBox drop-down lists --
        // give them the current theme's colors instead of the system default
        // white background / black text, which stands out badly in dark mode.
        Palette pal = currentPalette();
        HDC hdc = (HDC)wParam;
        COLORREF bg = RGB((BYTE)(pal.surface.r * 255), (BYTE)(pal.surface.g * 255), (BYTE)(pal.surface.b * 255));
        COLORREF fg = RGB((BYTE)(pal.textPrimary.r * 255), (BYTE)(pal.textPrimary.g * 255), (BYTE)(pal.textPrimary.b * 255));
        SetBkColor(hdc, bg);
        SetTextColor(hdc, fg);
        return (LRESULT)g->hNativeCtlBgBrush;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        if (g->editingPeriodIndex != -1) {
            if (ptInRect(pt, g->rectPeriodSave)) savePeriodEditor();
            else if (ptInRect(pt, g->rectPeriodCancel)) closePeriodEditor();
            else if (ptInRect(pt, g->rectPeriodDelete)) deletePeriodEditor();
        } else if (g->savingProfile) {
            if (ptInRect(pt, g->rectProfileSaveBtn)) {
                if (saveCurrentAsProfile(getEditText(g->hEditProfileName))) {
                    g->toastText = L"저장되었습니다";
                    g->toastShownAtMs = GetTickCount64();
                }
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
        } else if (g->displaySettingsOpen) {
            if (ptInRect(pt, g->rectDisplayScheduleRow)) {
                g->settings.fullscreenShowSchedule = !g->settings.fullscreenShowSchedule;
                g->settings.save();
            } else if (ptInRect(pt, g->rectDisplayMemoRow)) {
                g->settings.fullscreenShowMemo = !g->settings.fullscreenShowMemo;
                g->settings.save();
            }
            g->displaySettingsOpen = false;
        } else if (ptInRect(pt, g->rectDisplaySettingsButton)) {
            g->displaySettingsOpen = true;
        } else if (!g->fullscreen && ptInRect(pt, g->rectSiteLink)) {
            ShellExecuteW(nullptr, L"open", L"https://hyot.dev", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (ptInRect(pt, g->rectFullscreenBtn)) {
            toggleFullscreen(hwnd);
        } else if (ptInRect(pt, g->rectThemeToggle)) {
            g->settings.theme = isEffectivelyLight() ? Theme::Dark : Theme::Light;
            g->settings.save();
            applyMemoTheme();
            applyNativeControlsTheme();
        } else if (!g->fullscreen && ptInRect(pt, g->rectAddPeriod)) {
            openPeriodEditor(-2);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoBold)) {
            toggleMemoBold();
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoUnderline)) {
            toggleMemoUnderline();
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoAlignLeft)) {
            setMemoAlign(PFA_LEFT);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoAlignCenter)) {
            setMemoAlign(PFA_CENTER);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoAlignRight)) {
            setMemoAlign(PFA_RIGHT);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoSizeDown)) {
            adjustMemoFontSize(-2);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoSizeUp)) {
            adjustMemoFontSize(2);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoFontColor)) {
            pickMemoColor(false);
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoBgColor)) {
            pickMemoColor(true);
        } else if (g->memoEmojiPickerOpen) {
            for (int i = 0; i < kSchoolEmojiCount; i++) {
                if (ptInRect(pt, g->memoEmojiRects[i])) { insertMemoEmoji(kSchoolEmojis[i]); break; }
            }
            g->memoEmojiPickerOpen = false;
        } else if (!g->fullscreen && ptInRect(pt, g->rectMemoEmoji)) {
            g->memoEmojiPickerOpen = true;
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
        if (g->fullscreen) {
            g->fullscreenIconLastMoveMs = GetTickCount64();
            if (g->cursorHiddenInFullscreen) {
                ShowCursor(TRUE);
                g->cursorHiddenInFullscreen = false;
            }
        }
        // The period list/site-link/etc. sit underneath whatever modal (period
        // editor, save/load profile) is currently up -- their rects are stale
        // (from the last draw before the modal opened) and irrelevant while it's
        // open, so skip hit-testing them entirely. This isn't just correctness:
        // an unrelated hover toggle here would InvalidateRect -> repaint ->
        // syncNativeControls, which used to yank focus/close whatever native
        // control (e.g. an open combo drop-down) the modal itself owns.
        bool anyModalOpen = g->editingPeriodIndex != -1 || g->savingProfile || g->loadingProfile;

        if (!anyModalOpen && g->pendingIsPeriodClick) {
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

        if (!anyModalOpen && !g->fullscreen && !g->isDraggingPeriod) {
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
            bool nowHovered = !anyModalOpen && !g->fullscreen && ptInRect(pt, g->rectSiteLink);
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
        if (g->isDraggingPeriod) { commitPeriodDragReorder(); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (g->pendingIsPeriodClick) { openPeriodEditor(g->pendingClickPeriodIndex); InvalidateRect(hwnd, nullptr, FALSE); }
        g->pendingIsPeriodClick = false;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F11) toggleFullscreen(hwnd);
        else if (wParam == VK_ESCAPE && g->editingPeriodIndex != -1) { closePeriodEditor(); InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->savingProfile) { g->savingProfile = false; InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->loadingProfile) { g->loadingProfile = false; InvalidateRect(hwnd, nullptr, FALSE); }
        else if (wParam == VK_ESCAPE && g->fullscreen) { toggleFullscreen(hwnd); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTickTimerId);
        if (g->hRichMemo && SendMessageW(g->hRichMemo, EM_GETMODIFY, 0, 0)) saveMemoRtf();
        if (g->cursorHiddenInFullscreen) ShowCursor(TRUE); // never leave the system cursor hidden on exit
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
    UpdateWindow(hwnd); // Starts windowed on the main (admin) screen; F11 for the TV/projector fullscreen display.

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g->timeSync.stop();
    discardDeviceResources();
    return 0;
}
