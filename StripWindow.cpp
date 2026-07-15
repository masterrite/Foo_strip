// StripWindow.cpp
// The floating, draggable, always-on-top window. Pure Win32 + GDI+.
// Owns its own message handling; created on foobar's main (UI) thread during
// initquit::on_init, so calls back into playback_control are main-thread-safe.

#include "stdafx.h"
#include "strip_shared.h"
#include <windowsx.h>
#include <shellapi.h>   // SHQueryUserNotificationState (fullscreen detection)
#include <gdiplus.h>
#include <string>
#include <mutex>
#include <cmath>

using namespace Gdiplus;

// ----------------------------------------------------------------------------
// Layout constants (logical px @ 96dpi; DPI scaling left as a follow-up).
// ----------------------------------------------------------------------------
namespace {

// Base (96-DPI / 100%) design dimensions. Actual pixel sizes are these scaled
// by the monitor DPI at window creation (see g_scale and the k* accessors).
// Proportions: taller album art, shorter seek row, larger time font.
// kBaseWidth/kBaseHeight are now read from config (user-customizable). Width
// drives horizontal stretch (title + bars get more room - the layout math is
// already width-relative). Height drives the INTERIOR content scale: a taller
// strip grows the art, buttons, fonts and rows proportionally so it fills the
// space instead of leaving a band of emptiness.
const int kDefHeight  = 46;   // the design baseline height (content scale = 1.0)
const int kBasePad    = 6;
const int kBaseArt    = 34;   // album art square
const int kBaseBtn    = 34;   // transport button hit size
const int kBaseSeekH  = 10;   // seek row height

// DPI scale factor (1.0 at 96 DPI / 100%). Set in strip_create_window and
// updated on WM_DPICHANGED.
double g_scale = 1.0;

// --- Proper DPI detection -------------------------------------------------
// We need the REAL DPI of the monitor the strip is on. The naive approach,
// GetDeviceCaps(GetDC(nullptr), LOGPIXELSX), is unreliable: depending on the
// host process's DPI-awareness it can report 96 regardless of the actual
// monitor scaling, leaving the strip un-scaled (tiny). These helpers query the
// true per-window / per-monitor DPI via the modern APIs (GetDpiForWindow /
// GetDpiForMonitor), loaded dynamically so the component still loads on older
// Windows that lack them (falling back to the desktop DC there).
static UINT dpi_for_window(HWND hwnd) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
        auto fn = (GetDpiForWindow_t)GetProcAddress(u, "GetDpiForWindow");
        if (fn && hwnd) { UINT d = fn(hwnd); if (d) return d; }
    }
    return 0;   // unavailable
}
static UINT dpi_for_point(POINT pt) {
    // Used before the window exists: get the DPI of the monitor at a point.
    HMODULE s = LoadLibraryW(L"shcore.dll");
    UINT dpi = 0;
    if (s) {
        typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR, int, UINT*, UINT*);
        auto fn = (GetDpiForMonitor_t)GetProcAddress(s, "GetDpiForMonitor");
        if (fn) {
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx) dpi = dx;
        }
        FreeLibrary(s);
    }
    return dpi;
}
// Set g_scale from a DPI value; returns true if it actually changed.
// Our strip is a layered top-level popup. The size diagnostic proved Windows
// does NOT bitmap-stretch it under any awareness mode - the window is created
// at exactly the pixels we request (actual rect == kWidth/kHeight even at 175%).
// Therefore we ALWAYS scale the layout by the real DPI ratio; there is no
// Windows stretch to compensate for. (Earlier "double-scaling" was a separate
// title-clip bug, not a sizing bug.)
static bool set_scale_from_dpi(UINT dpi) {
    if (!dpi) dpi = 96;
    double s = (double)dpi / 96.0;
    if (s < 1.0) s = 1.0;   // never shrink below the base design size
    if (s == g_scale) return false;
    g_scale = s;
    return true;
}

// Content scale derived from the configured height vs. the baseline (46). At the
// default height this is 1.0; at double height it's ~2.0. Multiplies the
// interior element sizes (buttons, fonts, rows) so they grow with the strip.
// Album art already follows height directly (H - kPad), so it needs no factor.
inline double contentScale() {
    double s = (double)strip_load_height() / (double)kDefHeight;
    if (s < 1.0) s = 1.0;          // never shrink interior below the baseline
    return s;
}

// kWidth/kHeight read the CONFIGURED dimensions (x DPI scale). Interior
// accessors fold in contentScale() so they grow with height.
inline int kWidth()  { return (int)(strip_load_width()  * g_scale + 0.5); }
inline int kHeight() { return (int)(strip_load_height() * g_scale + 0.5); }
inline int kPad()    { return (int)(kBasePad   * g_scale + 0.5); }
inline int kArt()    { return (int)(kBaseArt   * g_scale + 0.5); }
inline int kBtn()    { return (int)(kBaseBtn   * g_scale * contentScale() + 0.5); }
inline int kSeekH()  { return (int)(kBaseSeekH * g_scale + 0.5); }
inline float kFont(float base) { return (float)(base * g_scale * contentScale()); }
inline int S(int n) { return (int)(n * g_scale + 0.5); }
inline float Sf(float n) { return (float)(n * g_scale); }
// Content-scaled variants: scale with BOTH DPI and the height content scale, so
// vertical metrics (text Y positions, row/clip heights) grow in lockstep with
// the fonts when the strip is made taller. Using plain S()/Sf() for these would
// keep the boxes the original size while the fonts grew, clipping the text.
inline int SC(int n) { return (int)(n * g_scale * contentScale() + 0.5); }
inline float SCf(float n) { return (float)(n * g_scale * contentScale()); }

const wchar_t* kClassName = L"FoobarStripWindow";

ULONG_PTR g_gdiplusToken = 0;
HWND g_hwnd = nullptr;
UINT_PTR g_timer = 0;

// While ANY window is in a modal move/size loop, we suspend the strip's repaints
// so they don't compete with that window's drag on the shared UI thread (which
// makes dragging foobar choppy). Driven by a system-wide WinEvent hook.
HWINEVENTHOOK g_moveHook = nullptr;
bool g_moveLoopActive = false;

// Hide the strip while a fullscreen app (game, video, presentation) is in front,
// so it doesn't draw on top of it. Re-shown when back on the desktop.
bool g_hiddenForFullscreen = false;
bool g_hiddenByUser = false;   // master "Show strip" toggle (persists)

// Interaction state
bool g_scrubbing = false;
double g_scrub_preview = 0.0; // 0..1 while dragging the seek bar

// (1) which button is currently pressed, for click feedback. 0=none,1=prev,2=play,3=next
int g_pressed_btn = 0;
// hover feedback: which button the mouse is currently over (same numbering).
int g_hover_btn = 0;
bool g_tracking_leave = false; // TrackMouseEvent armed for WM_MOUSELEAVE

// Album-art enlarge popup: a separate borderless top-level window that shows a
// 300x300 version of the cover while the mouse is over the art thumbnail.
HWND g_artPopup = nullptr;
bool g_artHover = false;             // mouse currently over the art region
RECT g_rcArt{};                      // art thumbnail rect (set each paint)
inline int kPopupSize() { return (int)(strip_load_popup_size() * g_scale + 0.5); } // enlarged art dim
const wchar_t* kPopupClass = L"FoobarStripArtPopup";

// Cached, pre-scaled strip thumbnail. Scaling the FULL-resolution cover to the
// tiny strip size on every 60fps repaint is very expensive for large covers
// (a 4000x4000 image resampled 60x/sec = the reported 16-19% CPU). We instead
// scale ONCE into this cache and blit it each frame, re-scaling only when the
// source art or the target size changes.
Gdiplus::Bitmap* g_artThumb = nullptr;   // owned pre-scaled thumbnail
unsigned         g_artThumbGen = 0;       // art generation the thumb was made from
int              g_artThumbSize = 0;      // target size the thumb was scaled to

static void invalidate_art_thumb() {      // called when the source art changes
    delete g_artThumb; g_artThumb = nullptr;
    g_artThumbGen = 0; g_artThumbSize = 0;
}

// Forward declarations (defined after StripProc).
void show_art_popup();
void hide_art_popup();

// ----------------------------------------------------------------------------
// Theming. All paint colors route through the active Theme so right-click can
// flip dark/light. ARGB values.
// ----------------------------------------------------------------------------
struct Theme {
    Gdiplus::Color bg;          // strip background
    Gdiplus::Color artPh;       // album-art placeholder
    Gdiplus::Color title;       // title text
    Gdiplus::Color artist;      // artist subtitle text
    Gdiplus::Color time;        // seek time text (brighter than artist for contrast)
    Gdiplus::Color icon;        // transport glyphs
    Gdiplus::Color groove;      // seek track (unfilled)
    Gdiplus::Color accent;      // seek fill
    Gdiplus::Color thumb;       // seek thumb
    Gdiplus::Color btnHl;       // button hover/press tint (alpha applied per-use)
    Gdiplus::Color popupBg;     // enlarge popup backdrop
    Gdiplus::Color popupBorder; // enlarge popup border
};

const Theme kDarkTheme = {
    Gdiplus::Color(230, 24, 26, 31),   // bg (slightly translucent dark)
    Gdiplus::Color(255, 40, 44, 52),   // artPh
    Gdiplus::Color(255, 242, 244, 248),// title
    Gdiplus::Color(255, 138, 147, 162),// artist
    Gdiplus::Color(255, 196, 202, 212),// time (brighter than artist for contrast)
    Gdiplus::Color(255, 212, 218, 227),// icon
    Gdiplus::Color(255, 60, 64, 72),   // groove
    Gdiplus::Color(255, 91, 167, 245), // accent
    Gdiplus::Color(255, 255, 255, 255),// thumb
    Gdiplus::Color(255, 255, 255, 255),// btnHl (white tint)
    Gdiplus::Color(255, 18, 20, 24),   // popupBg
    Gdiplus::Color(255, 70, 76, 86),   // popupBorder
};

const Theme kLightTheme = {
    Gdiplus::Color(255, 238, 238, 238),// bg (matches Win11 taskbar)
    Gdiplus::Color(255, 210, 214, 220),// artPh
    Gdiplus::Color(255, 28, 32, 38),   // title (dark text)
    Gdiplus::Color(255, 96, 104, 116), // artist
    Gdiplus::Color(255, 60, 66, 78),   // time (darker than artist for contrast)
    Gdiplus::Color(255, 64, 70, 80),   // icon (dark glyphs)
    Gdiplus::Color(255, 198, 202, 208),// groove
    Gdiplus::Color(255, 38, 130, 222), // accent (slightly deeper blue for contrast)
    Gdiplus::Color(255, 60, 66, 76),   // thumb (dark on light)
    Gdiplus::Color(255, 0, 0, 0),      // btnHl (black tint on light bg)
    Gdiplus::Color(255, 248, 249, 251),// popupBg
    Gdiplus::Color(255, 200, 205, 212),// popupBorder
};

bool g_darkMode = true;                 // legacy; superseded by theme mode

// Read the Windows app theme from the registry. Returns true if the system is
// in DARK mode. Used for theme mode 2 (Follow system).
static bool system_is_dark() {
    DWORD val = 1, sz = sizeof(val);
    // AppsUseLightTheme = 0 means dark apps. Default to light (val=1) if missing.
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val, &sz) != ERROR_SUCCESS)
        return false;
    return val == 0;
}

// The live theme. Rebuilt per call from the configured theme mode:
//   0 Light, 1 Dark, 2 Follow-system, 3 Custom (preset base + custom overrides).
const Theme& theme() {
    static Theme s_custom;   // rebuilt when in custom mode
    int mode = strip_load_theme_mode();
    if (mode == 0) return kLightTheme;
    if (mode == 1) return kDarkTheme;
    if (mode == 2) return system_is_dark() ? kDarkTheme : kLightTheme;
    // mode 3 = Custom: start from the dark preset as a base, then apply the
    // user's custom colors. Index contract: 0 bg, 1 text, 2 buttons, 3 fill,
    // 4 track. Text drives title/artist/time; fill drives accent; track drives
    // groove. Other fields (thumb, popup, etc.) keep the dark-preset base.
    s_custom = kDarkTheme;
    auto C = [](int rgb) {
        return Gdiplus::Color(255, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    };
    s_custom.bg     = C(strip_load_color(0));
    s_custom.title  = C(strip_load_color(1));
    s_custom.artist = C(strip_load_color(1));
    s_custom.time   = C(strip_load_color(1));
    s_custom.icon   = C(strip_load_color(2));
    s_custom.accent = C(strip_load_color(3));
    s_custom.groove = C(strip_load_color(4));
    s_custom.popupBg = C(strip_load_color(5));
    // Hover/press highlight tint: derive from the button color so it always
    // contrasts with the user's chosen background. (Previously this kept the
    // dark-preset's white, which was invisible on light or white custom
    // backgrounds - the hover/press feedback "disappeared" in Custom mode.)
    s_custom.btnHl  = C(strip_load_color(2));
    return s_custom;
}

// (2) after releasing a scrub, hold the target position until foobar's reported
// position catches up, so the thumb doesn't flash back to the old spot.
bool g_pending_seek = false;
double g_pending_seek_pos = 0.0; // seconds

// (4) marquee scroll offset for an over-long title.
double g_marquee_offset = 0.0;   // px scrolled
ULONGLONG g_marquee_last_ms = 0;
double g_marquee_phase_ms = 0.0; // seconds into the current marquee cycle
double g_title_width = 0.0;      // measured px of current title
double g_title_avail = 0.0;      // available px in the title area

// Geometry of interactive regions, recomputed each paint.
RECT g_rcPrev{}, g_rcPlay{}, g_rcNext{}, g_rcSeek{};
RECT g_rcPrevHit{}, g_rcPlayHit{}, g_rcNextHit{};  // wider pressable areas
RECT g_rcVol{};                      // volume slider track rect (visual)
RECT g_rcVolHit{};                   // volume slider hit-test rect (generous)
RECT g_rcMute{};                     // speaker / mute-toggle icon rect
bool g_volScrubbing = false;         // dragging the volume slider

// Smooth-position interpolation: between 1Hz on_playback_time callbacks we
// advance position locally so the scrubber glides instead of stepping.
ULONGLONG g_last_tick_ms = 0;
double g_interp_position = 0.0;
double g_last_reported_pos = -1.0; // last st.position we saw, to re-baseline dt

inline bool pt_in(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring fmt_time(double secs) {
    if (secs < 0) secs = 0;
    int s = (int)secs;          // truncate (floor), matching foobar's seekbar
    int h = s / 3600;
    int m = (s % 3600) / 60;
    s %= 60;
    wchar_t buf[32];
    if (h > 0) swprintf_s(buf, L"%d:%02d:%02d", h, m, s);  // 1:30:00 for >= 1h
    else       swprintf_s(buf, L"%d:%02d", m, s);          // 3:10 otherwise
    return buf;
}

// True when a fullscreen app (game / video / presentation) is foreground. Uses
// the same Shell signal the OS uses to suppress notifications during fullscreen.
bool is_fullscreen_app_active() {
    // Hide only for a real fullscreen app/presentation, reported by the Shell's
    // notification state (does NOT fire for Start menu, taskbar, or flyouts),
    // and only when that fullscreen context is on the strip's own monitor.
    QUERY_USER_NOTIFICATION_STATE state;
    if (SHQueryUserNotificationState(&state) != S_OK) return false;
    bool shellFullscreen = (state == QUNS_BUSY ||
                            state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                            state == QUNS_PRESENTATION_MODE);
    if (!shellFullscreen) return false;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    HMONITOR stripMon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST)
                               : fgMon;
    return fgMon == stripMon;
}

// ----------------------------------------------------------------------------
// Painting
// ----------------------------------------------------------------------------
void paint(HWND hwnd) {
    RECT rc; GetWindowRect(hwnd, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    // Layered-window pipeline: render into a 32-bit premultiplied-ARGB DIB and
    // push it via UpdateLayeredWindow (see paint_art_popup_layered). Step 1 keeps
    // the background fully opaque so this looks identical to the old BitBlt path;
    // the background alpha setting comes next. Content (art/text/buttons/bars) is
    // always opaque.
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP old = (HBITMAP)SelectObject(mem, dib);

    // Draw through a GDI+ Bitmap that wraps the DIB memory as PREMULTIPLIED ARGB.
    // Going through the bitmap (not the HDC) is what makes GDI+ manage the alpha
    // channel correctly: antialiased glyph edges and the translucent background
    // get proper premultiplied alpha, so UpdateLayeredWindow composites them
    // cleanly. Drawing via the HDC instead leaves edge pixels non-premultiplied,
    // which shows as black fringing around text on the transparent surface.
    Bitmap layerBmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
    Graphics g(&layerBmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    // Background fill at the configured alpha (255 = opaque, lower = see-through
    // over whatever is behind the strip). Content drawn afterward stays opaque,
    // so only the backing is translucent. The DIB starts zeroed (transparent),
    // so a sub-255 alpha composites correctly over the desktop.
    g.SetSmoothingMode(SmoothingModeNone);
    int bgA = strip_load_bg_alpha();
    Color base = theme().bg;
    SolidBrush bg(Color((BYTE)bgA, base.GetR(), base.GetG(), base.GetB()));
    g.FillRectangle(&bg, -1, -1, W + 2, H + 2);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    // Snapshot state under lock.
    std::wstring title, artist, timeStr;
    double frac = 0.0;
    bool playing = false, canSeek = false;
    Bitmap* art = nullptr;
    unsigned artGen = 0;
    {
        auto& st = strip_get_state();
        std::lock_guard<std::mutex> guard(st.lock);
        title = st.title;
        artist = st.artist;
        playing = st.playing;
        canSeek = st.canSeek;
        art = st.art;
        artGen = st.artGen;
        double pos = g_scrubbing ? g_scrub_preview * st.length : g_interp_position;
        if (st.length > 0) frac = min(1.0, max(0.0, pos / st.length));
        timeStr = fmt_time(pos) + L" / " + fmt_time(st.length);

        // ====================================================================
        // PROPORTIONAL LAYOUT (scales coherently with height at any DPI).
        // The strip is two stacked bands inside a small inset:
        //   * TOP band  (~58% of content height): album art (left), title+artist
        //     (center-left), transport buttons + volume (right) - all vertically
        //     centered on topMid.
        //   * BOT band  (~42%): seek bar + time, vertically centered on botMid.
        // Every element derives its size/position from these bands and the band
        // height, so taller strips grow everything together instead of leaving
        // gaps or letting elements drift independently. 'u' is a unit scale =
        // band-driven size relative to the original 46px design.
        // ====================================================================
        int inset      = kPad() / 2;
        int contentTop = inset;
        int contentBot = H - inset;
        int contentH   = contentBot - contentTop;
        int topRowH    = (int)(contentH * 0.58 + 0.5);
        int botRowH    = contentH - topRowH;
        int topMid     = contentTop + topRowH / 2;
        int botMid     = contentTop + topRowH + botRowH / 2;
        // Unit scale: how big the interior is vs. the baseline. Drives button
        // size, font size, bar thickness - all from ONE factor so nothing drifts.
        // Baseline contentH at 46px height (DPI 1.0) is ~40px. The baseline MUST
        // be scaled by g_scale: contentH already includes the DPI factor (H =
        // configuredHeight x g_scale), so dividing by a bare 40 would re-apply
        // DPI a second time and the interior would scale by g_scale2 (~4x at
        // 200%) - buttons/fonts ballooned and clipped. Dividing by 40xg_scale
        // makes u a pure HEIGHT factor (1.0 at default height, any DPI), and the
        // single g_scale in US()/USf() supplies the DPI scaling. (Mirrors the
        // wScale baseline below, which already divides by 300xg_scale.)
        double u = (double)contentH / (40.0 * g_scale);
        if (u < 1.0) u = 1.0;
        auto US = [&](double n) { return (int)(n * g_scale * u + 0.5); };
        auto USf = [&](double n) { return (float)(n * g_scale * u); };

        // Width-scale: control spacing scales with strip WIDTH (relative to the
        // 300px default), not height. WS() maps a configured px gap through that
        // ratio and DPI, so wider strips get proportionally larger gaps.
        double wScale = (double)W / (300.0 * g_scale);
        if (wScale < 1.0) wScale = 1.0;
        auto WS = [&](double n) { return (int)(n * g_scale * wScale + 0.5); };

        // Album art: full-content-height square at the left.
        int artSize = contentH;
        int ax = inset;
        int ay = contentTop;
        g_rcArt = { ax, ay, ax + artSize, ay + artSize }; // hover hit-testing
        if (art) {
            // Rebuild the cached thumbnail only when the art GENERATION or the
            // target size changes - NOT every frame, and NOT keyed on the bitmap
            // pointer (the heap can hand the next track's bitmap the same address
            // as the freed previous one, which made a pointer key serve stale art).
            if (g_artThumb == nullptr || g_artThumbGen != artGen || g_artThumbSize != artSize) {
                invalidate_art_thumb();
                if (artSize > 0) {
                    g_artThumb = new Gdiplus::Bitmap(artSize, artSize, PixelFormat32bppPARGB);
                    Gdiplus::Graphics tg(g_artThumb);
                    tg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    tg.SetPixelOffsetMode(PixelOffsetModeHalf);
                    tg.DrawImage(art, 0, 0, artSize, artSize);
                    g_artThumbGen = artGen;
                    g_artThumbSize = artSize;
                }
            }
            if (g_artThumb)
                g.DrawImage(g_artThumb, ax, ay, artSize, artSize);
        } else {
            SolidBrush ph(theme().artPh);
            g.FillRectangle(&ph, ax, ay, artSize, artSize);
        }

        // ---- Title + artist (stacked, centered on topMid) ----
        // Font family + per-element sizes come from config (Text settings).
        wchar_t faceW[128];
        {
            pfc::string8 face; strip_load_font_face(face);
            if (face.is_empty()) face = "Segoe UI";
            MultiByteToWideChar(CP_UTF8, 0, face.get_ptr(), -1, faceW, 128);
        }
        FontFamily ff(faceW);
        // Fall back to a generic sans family if the configured face is invalid.
        FontFamily ffFallback(L"Segoe UI");
        const FontFamily* pff = ff.IsAvailable() ? &ff : &ffFallback;
        float szTitle  = (float)strip_load_font_size(0);
        float szArtist = (float)strip_load_font_size(1);
        Font fTitle(pff, USf(szTitle), FontStyleBold, UnitPixel);
        Font fArtist(pff, USf(szArtist), FontStyleRegular, UnitPixel);
        SolidBrush cTitle(theme().title);
        SolidBrush cArtist(theme().title);

        int tx = ax + artSize + kPad();
        // Transport buttons + speaker + volume occupy the right of the top band.
        // Icon base sizes come from config (Text/Icons settings) and still scale
        // with height via US(). gTran/gSpk are glyph scale factors relative to the
        // original 20px / 14px designs, applied to the glyph stroke offsets.
        int iTran = strip_load_icon_size(0);   // transport base px
        int iSpk  = strip_load_icon_size(1);   // speaker base px
        double gTran = iTran / 20.0;
        double gSpk  = iSpk / 14.0;
        int btnSize = US(iTran);               // transport button hit box
        int spaceBtn = WS(strip_load_spacing(0));   // gap between transport buttons (width-scaled)
        int spaceVol = WS(strip_load_spacing(1));   // gap buttons -> volume (width-scaled)
        bool showVol = strip_load_show_volume();
        int volWidth = showVol ? US(40) : 0;   // volume bar width (0 if hidden)
        int muteWidth = showVol ? US(iSpk) : 0;// speaker icon box (0 if hidden)
        int volGap = showVol ? US(10) : 0;
        int rightControls = (btnSize * 3) + (spaceBtn * 2) + muteWidth + volWidth + volGap
                          + (showVol ? spaceVol : 0);
        int textW = W - tx - rightControls - kPad();
        if (textW < 10) textW = 10;

        // Title sits just above center, artist just below - both centered on
        // topMid as a pair. Box heights ~1.3x the font size for line height, so
        // taller fonts get proportional room.
        int titleH = (int)(USf(szTitle * 1.3f) + 0.5);
        int artistH = (int)(USf(szArtist * 1.3f) + 0.5);
        int pairH = titleH + artistH;
        int titleY = topMid - pairH / 2;
        int artistY = titleY + titleH;
        RectF artistRect((REAL)tx, (REAL)artistY, (REAL)textW, (REAL)artistH);
        StringFormat sf;
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);

        // (4) Title with marquee. The timer thread owns the marquee state and
        // resets it on song change (see the advance block); here we only measure
        // the current title so the timer knows whether it overflows.
        RectF measure;
        g.MeasureString(title.c_str(), -1, &fTitle, PointF(0, 0), &measure);
        g_title_width = measure.Width;
        g_title_avail = textW;

        Region oldClip;
        g.GetClip(&oldClip);
        g.SetClip(RectF((REAL)tx, (REAL)titleY, (REAL)textW, (REAL)titleH));
        if (g_title_width > g_title_avail) {
            // Continuous marquee: draw the title twice, separated by a gap. As
            // the first copy scrolls off the left, the second is already filling
            // in from the right, so it reads as one endless stream. The offset
            // wraps at (title width + gap), keeping motion seamless.
            const REAL gap = 40.0f;
            REAL span = (REAL)g_title_width + gap;
            REAL off = (REAL)fmod(g_marquee_offset, span);

            StringFormat sfNo;
            sfNo.SetFormatFlags(StringFormatFlagsNoWrap);

            RectF r1((REAL)tx - off, (REAL)titleY, (REAL)g_title_width + Sf(20), (REAL)titleH);
            g.DrawString(title.c_str(), -1, &fTitle, r1, &sfNo, &cTitle);

            RectF r2((REAL)tx - off + span, (REAL)titleY, (REAL)g_title_width + Sf(20), (REAL)titleH);
            g.DrawString(title.c_str(), -1, &fTitle, r2, &sfNo, &cTitle);
        } else {
            RectF titleRect((REAL)tx, (REAL)titleY, (REAL)textW, (REAL)titleH);
            g.DrawString(title.c_str(), -1, &fTitle, titleRect, &sf, &cTitle);
        }
        g.SetClip(&oldClip);

        g.DrawString(artist.c_str(), -1, &fArtist, artistRect, &sf, &cArtist);

        // ---- Transport buttons + volume (right side of the TOP band) ----
        // Everything here centers on topMid and uses the unit-scaled sizes, so
        // it stays put and grows proportionally as the strip gets taller.
        int bandMid = topMid;
        int btnTop = bandMid - btnSize / 2;
        int btnBot = bandMid + btnSize / 2;

        // Right edge, laid out right-to-left:
        //   [ prev ][ play ][ next ]  [speaker] [volume bar]
        // When volume is hidden, the speaker + bar are skipped and the buttons
        // anchor directly to the right edge (reflow right, no empty gap).
        int volBarH = US(8);
        int btnRight;
        if (showVol) {
            int volRight = W - kPad();
            int volLeft  = volRight - volWidth;
            g_rcVol = { volLeft, bandMid - volBarH / 2, volRight, bandMid + volBarH / 2 };
            int volHitPad = US(7);
            g_rcVolHit = { volLeft - volHitPad, btnTop, volRight + volHitPad, btnBot };
            g_rcMute = { volLeft - muteWidth - US(2), bandMid - muteWidth / 2,
                         volLeft - US(2), bandMid + muteWidth / 2 };
            btnRight = g_rcMute.left - spaceVol;         // buttons end before speaker
        } else {
            // Volume hidden: no bar/speaker; buttons go to the right edge.
            g_rcVol = { 0, 0, 0, 0 };
            g_rcVolHit = { 0, 0, 0, 0 };
            g_rcMute = { 0, 0, 0, 0 };
            btnRight = W - kPad();
        }
        g_rcNext = { btnRight - btnSize, btnTop, btnRight, btnBot };
        g_rcPlay = { g_rcNext.left - spaceBtn - btnSize, btnTop, g_rcNext.left - spaceBtn, btnBot };
        g_rcPrev = { g_rcPlay.left - spaceBtn - btnSize, btnTop, g_rcPlay.left - spaceBtn, btnBot };

        // Wider PRESSABLE areas: inflate each button's hit box horizontally by a
        // fraction of the button size (so it scales with button size). The visual
        // glyphs stay put; only the clickable region grows. Inner edges are
        // clamped to the midpoint between adjacent buttons, so the hit areas meet
        // edge-to-edge but never overlap (no ambiguous clicks) - even when the
        // buttons are flush (spaceBtn == 0).
        int hitPad = btnSize / 4;               // horizontal pad, scales with size
        int vPad   = btnSize / 4 + 1;           // vertical pad, scales with size (+1px)
        // Keep the taller hit box within the top band so it doesn't spill into the
        // seek bar below or off the top edge.
        int hitTop = btnTop - vPad; if (hitTop < contentTop) hitTop = contentTop;
        int hitBot = btnBot + vPad; if (hitBot > contentTop + topRowH) hitBot = contentTop + topRowH;
        LONG midPN = (g_rcPrev.right + g_rcPlay.left) / 2;   // prev|play boundary
        LONG midNN = (g_rcPlay.right + g_rcNext.left) / 2;   // play|next boundary
        LONG prevR = g_rcPrev.right + hitPad;  if (prevR > midPN) prevR = midPN;
        LONG playL = g_rcPlay.left  - hitPad;  if (playL < midPN) playL = midPN;
        LONG playR = g_rcPlay.right + hitPad;  if (playR > midNN) playR = midNN;
        LONG nextL = g_rcNext.left  - hitPad;  if (nextL < midNN) nextL = midNN;
        g_rcPrevHit = { g_rcPrev.left - hitPad, hitTop, prevR, hitBot };
        g_rcPlayHit = { playL, hitTop, playR, hitBot };
        g_rcNextHit = { nextL, hitTop, g_rcNext.right + hitPad, hitBot };

        // (1) Button feedback: hover = subtle, pressed = stronger. The highlight
        // fills the full CLICKABLE (hit) area so the feedback matches exactly what
        // the user is pressing. A 1px inset keeps adjacent highlights from
        // touching when buttons are flush.
        {
            auto drawBtnBg = [&](const RECT& r, int id) {
                int alpha = 0;
                if (g_pressed_btn == id) alpha = 70;
                else if (g_hover_btn == id) alpha = 32;
                if (alpha == 0) return;
                Color t = theme().btnHl;
                SolidBrush hl(Color(alpha, t.GetR(), t.GetG(), t.GetB()));
                g.FillRectangle(&hl, r.left + S(1), r.top + S(1),
                                (r.right - r.left) - S(2), (r.bottom - r.top) - S(2));
            };
            drawBtnBg(g_rcPrevHit, 1);
            drawBtnBg(g_rcPlayHit, 2);
            drawBtnBg(g_rcNextHit, 3);
        }

        // Modern outline glyphs: shapes are stroked, not filled. Stroke width
        // and glyph size scale with the strip height (US) so glyphs grow with
        // their button boxes instead of floating tiny in a large box.
        // Glyph-offset helpers: scale the hand-tuned offsets by the configured
        // icon size (relative to the 20px/14px originals). GT = transport, GS =
        // speaker. Pen width scales with the transport icon.
        auto GT = [&](double n) { return (REAL)USf(n * gTran); };
        auto GS = [&](double n) { return (REAL)USf(n * gSpk); };
        Pen pen(theme().icon, USf(1.6f * gTran));
        pen.SetLineJoin(LineJoinRound);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);

        // prev |<
        {
            int cx = (g_rcPrev.left + g_rcPrev.right) / 2;
            int cy = (g_rcPrev.top + g_rcPrev.bottom) / 2;
            g.DrawLine(&pen, (REAL)cx - GT(7), (REAL)cy - GT(7), (REAL)cx - GT(7), (REAL)cy + GT(7));
            PointF t2[3] = { {(REAL)cx + GT(6), (REAL)cy - GT(7)}, {(REAL)cx - GT(5), (REAL)cy}, {(REAL)cx + GT(6), (REAL)cy + GT(7)} };
            g.DrawPolygon(&pen, t2, 3);
        }
        // play / pause
        {
            int cx = (g_rcPlay.left + g_rcPlay.right) / 2;
            int cy = (g_rcPlay.top + g_rcPlay.bottom) / 2;
            if (playing) {
                g.DrawLine(&pen, (REAL)cx - GT(5), (REAL)cy - GT(8), (REAL)cx - GT(5), (REAL)cy + GT(8));
                g.DrawLine(&pen, (REAL)cx + GT(5), (REAL)cy - GT(8), (REAL)cx + GT(5), (REAL)cy + GT(8));
            } else {
                PointF tri[3] = { {(REAL)cx - GT(6), (REAL)cy - GT(9)}, {(REAL)cx + GT(9), (REAL)cy}, {(REAL)cx - GT(6), (REAL)cy + GT(9)} };
                g.DrawPolygon(&pen, tri, 3);
            }
        }
        // next >|
        {
            int cx = (g_rcNext.left + g_rcNext.right) / 2;
            int cy = (g_rcNext.top + g_rcNext.bottom) / 2;
            PointF tri[3] = { {(REAL)cx - GT(6), (REAL)cy - GT(7)}, {(REAL)cx + GT(5), (REAL)cy}, {(REAL)cx - GT(6), (REAL)cy + GT(7)} };
            g.DrawPolygon(&pen, tri, 3);
            g.DrawLine(&pen, (REAL)cx + GT(7), (REAL)cy - GT(7), (REAL)cx + GT(7), (REAL)cy + GT(7));
        }

        // ---- Volume: speaker icon (mute toggle) + slider ----
        if (showVol) {
            // Still inside the paint lock scope, so read directly from st.
            double volLinear = st.volume_linear;
            bool muted = st.muted;

            int mcx = (g_rcMute.left + g_rcMute.right) / 2;
            int mcy = (g_rcMute.top + g_rcMute.bottom) / 2;
            // Speaker body (small trapezoid-ish): a rectangle + triangle cone.
            g.DrawLine(&pen, (REAL)mcx - GS(5), (REAL)mcy - GS(2), (REAL)mcx - GS(5), (REAL)mcy + GS(2)); // back
            PointF spk[4] = {
                {(REAL)mcx - GS(5), (REAL)mcy - GS(2)},
                {(REAL)mcx - GS(1), (REAL)mcy - GS(5)},
                {(REAL)mcx - GS(1), (REAL)mcy + GS(5)},
                {(REAL)mcx - GS(5), (REAL)mcy + GS(2)} };
            g.DrawPolygon(&pen, spk, 4);
            if (muted) {
                // Muted: an X to the right of the speaker.
                g.DrawLine(&pen, (REAL)mcx + GS(1), (REAL)mcy - GS(3), (REAL)mcx + GS(5), (REAL)mcy + GS(3));
                g.DrawLine(&pen, (REAL)mcx + GS(5), (REAL)mcy - GS(3), (REAL)mcx + GS(1), (REAL)mcy + GS(3));
            } else {
                // Sound waves: two small arcs.
                g.DrawArc(&pen, (REAL)mcx - GS(1), (REAL)mcy - GS(4), GS(6), GS(8), -60.0f, 120.0f);
            }

            // Volume as a fillable bar (no thumb): a background bar with an
            // accent fill from the left proportional to volume. The whole bar is
            // the control, so there's no small thumb to grab - dragging anywhere
            // on it sets the level. Matches the original Deskband plugin's look.
            int vBarH = US(8);                         // bar height (scales w/ height)
            int vTrackY = (g_rcVol.top + g_rcVol.bottom) / 2;
            int vLeft = g_rcVol.left, vRight = g_rcVol.right;
            int vTop = vTrackY - vBarH / 2;
            SolidBrush vbg(theme().groove);
            SolidBrush vaccent(theme().accent);
            g.FillRectangle(&vbg, vLeft, vTop, vRight - vLeft, vBarH);
            double vfrac = muted ? 0.0 : volLinear;
            if (vfrac < 0) vfrac = 0; if (vfrac > 1) vfrac = 1;
            int vfill = (int)((vRight - vLeft) * vfrac);
            g.FillRectangle(&vaccent, vLeft, vTop, vfill, vBarH);
        }

        // ---- Bottom band: seek bar + time, centered on botMid ----
        std::wstring t = timeStr;
        float szTime = (float)strip_load_font_size(2);
        Font fTime(pff, USf(szTime), FontStyleRegular, UnitPixel);
        RectF timeMeasure;
        g.MeasureString(t.c_str(), -1, &fTime, PointF(0, 0), &timeMeasure);
        int timeW = (int)timeMeasure.Width + US(6);

        int seekLeft = ax + artSize + kPad();
        int seekRight = W - kPad() - timeW - kPad();
        if (seekRight < seekLeft + US(20)) seekRight = seekLeft + US(20);

        int tr = US(5);                                 // thumb radius
        int trackY = botMid;                            // centered in bottom band
        g_rcSeek = { seekLeft, trackY - tr, seekRight, trackY + tr };

        int gh = US(3);                                 // groove thickness
        SolidBrush groove(theme().groove);
        SolidBrush accent(theme().accent);
        g.FillRectangle(&groove, seekLeft, trackY - gh / 2, seekRight - seekLeft, gh);
        int fillW = (int)((seekRight - seekLeft) * frac);
        g.FillRectangle(&accent, seekLeft, trackY - gh / 2, fillW, gh);

        // thumb
        SolidBrush thumb(theme().thumb);
        int thumbX = seekLeft + fillW;
        g.FillEllipse(&thumb, thumbX - tr, trackY - tr, tr * 2, tr * 2);

        // time text - right-aligned to the volume bar's right edge, centered on
        // the seek track vertically.
        SolidBrush cTime(theme().title);
        StringFormat sfTime;
        sfTime.SetAlignment(StringAlignmentFar);
        sfTime.SetTrimming(StringTrimmingNone);
        sfTime.SetFormatFlags(StringFormatFlagsNoWrap);
        int timeH = (int)(USf(szTime * 1.3f) + 0.5);
        RectF timeRect((REAL)(seekRight + kPad()), (REAL)(trackY - timeH / 2),
                       (REAL)(W - kPad() - (seekRight + kPad())), (REAL)timeH);
        g.DrawString(t.c_str(), -1, &fTime, timeRect, &sfTime, &cTime);
    }

    // GDI+ text/vector antialiasing writes color but leaves some edge pixels
    // with a too-low alpha. On a layered window those show as a dark halo around
    // the text once the background is translucent. Fix it up: ensure no pixel is
    // MORE transparent than the background. We raise alpha (and the premultiplied
    // RGB proportionally so the visible color is preserved) up to bgA. Content
    // areas (already alpha 255) are untouched.
    g.Flush(FlushIntentionSync);
    if (bits && bgA < 255) {
        BYTE* px = (BYTE*)bits;            // BGRA, premultiplied, top-down
        int count = W * H;
        for (int i = 0; i < count; i++, px += 4) {
            BYTE a = px[3];
            if (a < bgA) {
                // Raise alpha to bgA. Rescale premultiplied BGR by bgA/a so the
                // un-premultiplied color stays the same (avoids darkening edges).
                if (a == 0) {
                    // Fully transparent: this is true background gap - set to the
                    // background color at bgA (premultiplied).
                    px[0] = (BYTE)(base.GetB() * bgA / 255);
                    px[1] = (BYTE)(base.GetG() * bgA / 255);
                    px[2] = (BYTE)(base.GetR() * bgA / 255);
                } else {
                    px[0] = (BYTE)min(255, px[0] * bgA / a);
                    px[1] = (BYTE)min(255, px[1] * bgA / a);
                    px[2] = (BYTE)min(255, px[2] * bgA / a);
                }
                px[3] = (BYTE)bgA;
            }
        }
    }

    // Push the ARGB bitmap to the layered strip window.
    POINT ptSrc{ 0, 0 };
    SIZE  sz{ W, H };
    POINT ptDst{ rc.left, rc.top };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ----------------------------------------------------------------------------
// Window procedure
// ----------------------------------------------------------------------------
LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        auto& st = strip_get_state();
        bool needsRepaintOut = false;
        {
        std::lock_guard<std::mutex> guard(st.lock);
        ULONGLONG now = GetTickCount64();

        // Fullscreen suppression: hide the strip while a game/video is fronted.
        // Checked ~once per second (state changes slowly; the call isn't free).
        static ULONGLONG s_lastFsCheck = 0;
        if (now - s_lastFsCheck >= 1000) {
            s_lastFsCheck = now;
            bool fs = is_fullscreen_app_active();
            if (fs && !g_hiddenForFullscreen) {
                g_hiddenForFullscreen = true;
                ShowWindow(hwnd, SW_HIDE);
            } else if (!fs && g_hiddenForFullscreen) {
                g_hiddenForFullscreen = false;
                if (!g_hiddenByUser) ShowWindow(hwnd, SW_SHOWNA); // unless user-hidden
            }
        }

        // Re-assert topmost z-order frequently (every ~200ms, separate from the
        // 1s fullscreen check). A one-time WS_EX_TOPMOST isn't enough to stay
        // above the taskbar - when the strip sits ON the taskbar and you open
        // Start, click a taskbar item, or switch monitors, the taskbar raises
        // above the strip. Frequent re-assertion pulls it back promptly. This is
        // the ceiling for a normal window; we can't permanently beat the taskbar,
        // but ~5x/sec keeps it visible in practice.
        static ULONGLONG s_lastTop = 0;
        if (!g_hiddenForFullscreen && !g_hiddenByUser && now - s_lastTop >= 200) {
            s_lastTop = now;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (g_hiddenForFullscreen || g_hiddenByUser) return 0; // nothing to draw while hidden

        // (2) While a seek is pending, keep showing the target until foobar's
        // reported position reaches it (within ~0.6s), then resume normal flow.
        if (g_pending_seek) {
            if (fabs(st.position - g_pending_seek_pos) < 0.6) {
                g_pending_seek = false;          // caught up
                g_last_tick_ms = now;
            } else {
                g_interp_position = g_pending_seek_pos; // hold (repaint handled below)
            }
        }

        if (!g_pending_seek) {
            // Interpolate position between foobar's ~1Hz updates for a smooth
            // bar. CRITICAL: dt must measure time since foobar's LAST reported
            // position, not since playback start. So whenever st.position
            // changes (foobar pushed an update), re-baseline the clock. Without
            // this, dt grows unbounded and stacks on top of st.position, making
            // the bar run ~2x fast.
            if (st.playing && !g_scrubbing) {
                if (st.position != g_last_reported_pos) {
                    g_last_reported_pos = st.position;
                    g_last_tick_ms = now;          // re-anchor on fresh data
                }
                if (g_last_tick_ms == 0) g_last_tick_ms = now;
                double dt = (now - g_last_tick_ms) / 1000.0;
                // Don't predict more than ~1s past the last reported value; if a
                // foobar update is late, we hold rather than drift further ahead.
                if (dt > 1.05) dt = 1.05;
                g_interp_position = st.position + dt;
                if (st.length > 0 && g_interp_position > st.length)
                    g_interp_position = st.length;
            } else {
                g_interp_position = st.position;
                g_last_tick_ms = now;
                g_last_reported_pos = st.position;
            }
        }

        // (4) Continuous marquee: advance the offset at a constant rate after a
        // short initial pause. paint() wraps it with fmod over (title+gap), so
        // motion is endless and seamless - no slide-back.
        //
        // Song-switch handling: g_title_width is measured in paint(), so on the
        // first tick after a new title it may still hold the OLD title's width.
        // Advancing against that stale width causes a visible jitter right at the
        // switch (worst on ~60Hz, where the 16ms timer beats with the refresh).
        // So we detect the title change HERE, reset the marquee, and skip the
        // advance for this frame - paint() will re-measure the new title before
        // the next tick advances it.
        static std::wstring s_marqueeTitle;
        bool titleJustChanged = (st.title != s_marqueeTitle);
        if (titleJustChanged) {
            s_marqueeTitle = st.title;
            g_marquee_offset = 0.0;
            g_marquee_last_ms = 0;
            g_marquee_phase_ms = 0.0;
        }

        bool marqueeActive = (g_title_width > g_title_avail && g_title_avail > 0);
        if (marqueeActive && !titleJustChanged) {
            if (g_marquee_last_ms == 0) {
                g_marquee_last_ms = now;
                g_marquee_phase_ms = 0.0;
            }
            double dt = (now - g_marquee_last_ms) / 1000.0;
            g_marquee_last_ms = now;
            g_marquee_phase_ms += dt;

            const double pauseStart = 1.2;  // s held before scrolling begins
            const double speed = 30.0;      // px/sec
            if (g_marquee_phase_ms > pauseStart) {
                g_marquee_offset += dt * speed;
            }
        } else if (!marqueeActive) {
            g_marquee_offset = 0.0;
            g_marquee_last_ms = 0;
            g_marquee_phase_ms = 0.0;
        }

        // Only repaint when something is actually animating - AND not while a
        // window move/size loop is active anywhere, so the strip doesn't steal
        // UI-thread time and make dragging (foobar or the strip) choppy.
        bool needsRepaint = !g_moveLoopActive && (
            (st.playing && !g_scrubbing) ||  // seek bar advancing
            g_scrubbing ||                   // user dragging the scrubber
            g_volScrubbing ||                // user dragging the volume slider
            g_pending_seek ||                // holding post-seek position
            marqueeActive ||                 // title scrolling
            g_pressed_btn != 0 ||            // button pressed
            g_hover_btn != 0);               // button hovered

        // Layered windows don't reliably receive WM_PAINT, so repaint by calling
        // paint() directly rather than InvalidateRect (which only schedules a
        // WM_PAINT that may never arrive). paint() locks st.lock itself, so we
        // must release THIS lock first (scope ends here) to avoid self-deadlock.
        needsRepaintOut = needsRepaint;
        } // release st.lock before painting

        if (needsRepaintOut)
            paint(hwnd);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // Double-click the album art -> bring foobar's main window to the front
        // (a common, intuitive shortcut). Elsewhere on the strip, double-click
        // does nothing special (the first click already did its action).
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (pt_in(g_rcArt, x, y)) {
            HWND fb = core_api::get_main_window();
            if (fb) {
                // Restore-then-raise foobar's main window, reliably even from
                // minimized. The order matters: we AttachThreadInput to foobar's
                // OWN thread FIRST, then do the restore + raise INSIDE the attach.
                // While attached, our show/activate calls behave as if issued by
                // foobar's own thread, so the foreground lock doesn't block them
                // and the restore's activation isn't lost to a race. Detaching
                // last keeps the whole sequence as one atomic operation.
                DWORD fbT = GetWindowThreadProcessId(fb, nullptr);
                DWORD myT = GetCurrentThreadId();
                bool attached = (fbT && fbT != myT && AttachThreadInput(myT, fbT, TRUE));

                if (IsIconic(fb)) ShowWindow(fb, SW_RESTORE);   // 1. un-minimize
                SetForegroundWindow(fb);                        // 2. raise + focus
                BringWindowToTop(fb);
                SetActiveWindow(fb);

                if (attached) AttachThreadInput(myT, fbT, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSEACTIVATE:
        // The strip is a no-activate tool window. By default, when it's the
        // inactive window, the OS can swallow the first click just to activate
        // it - the user sees the hover overlay but the press doesn't register
        // ("missing clicks" when working in other windows). Returning
        // MA_NOACTIVATE tells Windows: don't activate the strip (keep focus where
        // it is) AND deliver the click as a normal mouse message instead of
        // eating it. So every click registers on the first try.
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);

        if (pt_in(g_rcSeek, x, y)) {
            g_scrubbing = true;
            double f = (double)(x - g_rcSeek.left) / (g_rcSeek.right - g_rcSeek.left);
            g_scrub_preview = min(1.0, max(0.0, f));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcMute, x, y)) {
            strip_toggle_mute();   // fires on_volume_change -> refresh + repaint
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcVolHit, x, y)) {
            g_volScrubbing = true;
            double f = (double)(x - g_rcVol.left) / (g_rcVol.right - g_rcVol.left);
            strip_set_volume(min(1.0, max(0.0, f))); // fires on_volume_change
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcPrevHit, x, y) || pt_in(g_rcPlayHit, x, y) || pt_in(g_rcNextHit, x, y)) {
            // Record which button is pressed for visual feedback; fired on up.
            if (pt_in(g_rcPrevHit, x, y)) g_pressed_btn = 1;
            else if (pt_in(g_rcPlayHit, x, y)) g_pressed_btn = 2;
            else g_pressed_btn = 3;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcArt, x, y)) {
            // Album art: reserved for hover-popup and double-click (open foobar).
            // Don't start a window drag here, or the modal drag loop would
            // swallow the double-click. The rest of the strip body still drags.
        } else {
            // Drag the window using the OS's native move loop (smooth, compositor
            // -backed) instead of hand-rolling it with SetWindowPos. We release
            // capture and hand Windows a caption-drag; it runs its own modal move.
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // Arm WM_MOUSELEAVE so we can clear hover when the cursor exits.
        if (!g_tracking_leave) {
            TRACKMOUSEEVENT tme{ sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_tracking_leave = true;
        }

        // Update hover state (skip while actively scrubbing).
        if (!g_scrubbing) {
            int h = 0;
            if (pt_in(g_rcPrevHit, x, y)) h = 1;
            else if (pt_in(g_rcPlayHit, x, y)) h = 2;
            else if (pt_in(g_rcNextHit, x, y)) h = 3;
            if (h != g_hover_btn) {
                g_hover_btn = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // Album-art hover -> show/hide the enlarged popup.
            bool overArt = pt_in(g_rcArt, x, y);
            // Only pop the enlarged view if the popup is enabled AND there's
            // actually album art to show - otherwise hovering an empty
            // placeholder showed a blank square.
            bool haveArt;
            { auto& st = strip_get_state();
              std::lock_guard<std::mutex> g(st.lock); haveArt = (st.art != nullptr); }
            bool popupEnabled = strip_load_show_popup();
            if (overArt && haveArt && popupEnabled && !g_artHover) {
                g_artHover = true;
                show_art_popup();
            } else if ((!overArt || !haveArt || !popupEnabled) && g_artHover) {
                g_artHover = false;
                hide_art_popup();
            }
        }

        if (g_scrubbing) {
            double f = (double)(x - g_rcSeek.left) / (g_rcSeek.right - g_rcSeek.left);
            g_scrub_preview = min(1.0, max(0.0, f));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (g_volScrubbing) {
            double f = (double)(x - g_rcVol.left) / (g_rcVol.right - g_rcVol.left);
            strip_set_volume(min(1.0, max(0.0, f))); // on_volume_change refreshes
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ReleaseCapture();

        if (g_volScrubbing) {
            g_volScrubbing = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (g_scrubbing) {
            g_scrubbing = false;
            auto& st = strip_get_state();
            double length;
            { std::lock_guard<std::mutex> guard(st.lock); length = st.length; }
            if (length > 0) {
                double target = g_scrub_preview * length;
                strip_seek_to(target);
                // (2) Hold the thumb at the target until foobar's reported
                // position catches up, so it doesn't flash back for one frame.
                g_pending_seek = true;
                g_pending_seek_pos = target;
                g_interp_position = target;
                g_last_tick_ms = GetTickCount64();
            }
        } else if (g_pressed_btn != 0) {
            // Transport clicks (only if mouse still over the same button).
            auto pc = playback_control::get();
            if (g_pressed_btn == 1 && pt_in(g_rcPrevHit, x, y)) {
                pc->previous();
            } else if (g_pressed_btn == 2 && pt_in(g_rcPlayHit, x, y)) {
                // (1) If stopped, start playback; otherwise toggle pause. This is
                // why the play button did nothing before a song was started.
                if (pc->is_playing()) pc->toggle_pause();
                else pc->start(playback_control::track_command_play, false);
            } else if (g_pressed_btn == 3 && pt_in(g_rcNextHit, x, y)) {
                pc->next();
            }
            g_pressed_btn = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking_leave = false;
        if (g_hover_btn != 0) {
            g_hover_btn = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (g_artHover) {
            g_artHover = false;
            hide_art_popup();
        }
        return 0;

    case WM_RBUTTONUP:
        // Open foobar's Preferences dialog at our page. (Theme is now a setting
        // inside that page rather than a right-click toggle.)
        strip_open_preferences();
        return 0;

    case WM_WINDOWPOSCHANGING: {
        // Edge snapping - only while SHIFT is held. By default the strip moves
        // freely; hold Shift during a drag to snap flush to a monitor edge.
        WINDOWPOS* wp2 = (WINDOWPOS*)lp;
        if (!(wp2->flags & SWP_NOMOVE) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            const int kSnap = S(12);
            int w = (wp2->cx != 0) ? wp2->cx : kWidth();
            int h = (wp2->cy != 0) ? wp2->cy : kHeight();
            RECT pr{ wp2->x, wp2->y, wp2->x + w, wp2->y + h };
            HMONITOR mon = MonitorFromRect(&pr, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfo(mon, &mi)) {
                const RECT& m = mi.rcMonitor; // full monitor (incl. taskbar)
                if (abs(wp2->x - m.left) <= kSnap) wp2->x = m.left;
                if (abs((wp2->x + w) - m.right) <= kSnap) wp2->x = m.right - w;
                if (abs(wp2->y - m.top) <= kSnap) wp2->y = m.top;
                if (abs((wp2->y + h) - m.bottom) <= kSnap) wp2->y = m.bottom - h;
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // we fully paint in WM_PAINT (double-buffered)

    case WM_DPICHANGED: {
        // Fired when the strip is dragged to a monitor with a different scale.
        // wParam high word = new DPI. Update g_scale and resize to match so the
        // strip stays the right physical size (text/buttons scale correctly).
        UINT newDpi = HIWORD(wp);
        if (set_scale_from_dpi(newDpi)) {
            SetWindowPos(hwnd, nullptr, 0, 0, kWidth(), kHeight(),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            paint(hwnd);
        }
        return 0;
    }

    case WM_PAINT: {
        // paint() renders via UpdateLayeredWindow (no BeginPaint/EndPaint), so we
        // must validate the update region here or WM_PAINT would loop forever.
        paint(hwnd);
        ValidateRect(hwnd, nullptr);
        return 0;
    }

    case WM_DESTROY:
        if (g_timer) { KillTimer(hwnd, g_timer); g_timer = 0; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// Album-art enlarge popup
// Render the popup into a 32-bit premultiplied-ARGB DIB and push it to the
// layered window via UpdateLayeredWindow. The padding/backdrop is drawn at the
// configured alpha (see-through over the desktop); the album art is drawn fully
// opaque inset inside it. This is the per-pixel-alpha pipeline (distinct from
// the opaque BitBlt path the strip still uses).
void paint_art_popup_layered() {
    if (!g_artPopup) return;
    RECT rc; GetWindowRect(g_artPopup, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    // 32-bit top-down DIB section we can both GDI+ draw into and hand to
    // UpdateLayeredWindow. Negative height = top-down so orientation matches.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, dib);

    {
        // Draw through a GDI+ Bitmap wrapping the DIB as premultiplied ARGB so
        // alpha compositing is correct (see the strip paint for the rationale).
        Bitmap layerBmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&layerBmp);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.Clear(Color(0, 0, 0, 0));   // fully transparent base

        int inset = S(6);
        int alpha = strip_load_popup_alpha();
        Color pc = theme().popupBg;
        // Backdrop at configured alpha (GDI+ premultiplies into PARGB for us).
        SolidBrush bg(Color((BYTE)alpha, pc.GetR(), pc.GetG(), pc.GetB()));
        g.FillRectangle(&bg, 0, 0, W, H);

        auto& st = strip_get_state();
        std::lock_guard<std::mutex> guard(st.lock);
        if (st.art) {
            g.DrawImage(st.art, inset, inset, W - inset * 2, H - inset * 2);
        } else {
            Color ph = theme().artPh;  // placeholder is opaque
            SolidBrush phb(Color(255, ph.GetR(), ph.GetG(), ph.GetB()));
            g.FillRectangle(&phb, inset, inset, W - inset * 2, H - inset * 2);
        }
    }

    // Push the ARGB bitmap to the layered window.
    POINT ptSrc{ 0, 0 };
    SIZE  sz{ W, H };
    POINT ptDst{ rc.left, rc.top };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_artPopup, screen, &ptDst, &sz, mem, &ptSrc,
                        0, &bf, ULW_ALPHA);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ----------------------------------------------------------------------------
LRESULT CALLBACK ArtPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Layered window: all rendering goes through paint_art_popup_layered via
    // UpdateLayeredWindow, so we just validate on WM_PAINT.
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProc(hwnd, msg, wp, lp);
}

void ensure_art_popup_created() {
    if (g_artPopup) return;
    HINSTANCE hInst = core_api::get_my_instance();
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = ArtPopupProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassEx(&wc);

    g_artPopup = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kPopupClass, L"", WS_POPUP,
        0, 0, kPopupSize(), kPopupSize(),
        nullptr, nullptr, hInst, nullptr);
}

void show_art_popup() {
    ensure_art_popup_created();
    if (!g_artPopup || !g_hwnd) return;

    // Center the enlarged cover horizontally over the strip.
    RECT sr; GetWindowRect(g_hwnd, &sr);
    int stripW = sr.right - sr.left;
    int psize = kPopupSize();
    int x = sr.left + (stripW - psize) / 2;
    int yAbove = sr.top - psize - 6;
    int yBelow = sr.bottom + 6;

    // Clamp horizontally to the work area.
    RECT wa{}; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + psize > wa.right) x = wa.right - psize;
    if (x < wa.left) x = wa.left;

    // Prefer above the strip; fall back to below. If neither fits (very large
    // popup on a short screen), clamp to the work area top.
    int y = (yAbove >= wa.top) ? yAbove : yBelow;
    if (y + psize > wa.bottom) y = wa.bottom - psize;
    if (y < wa.top) y = wa.top;

    SetWindowPos(g_artPopup, HWND_TOPMOST, x, y, psize, psize,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    paint_art_popup_layered();   // render ARGB content to the layered window
}

void hide_art_popup() {
    if (g_artPopup) ShowWindow(g_artPopup, SW_HIDE);
}

} // namespace

// ----------------------------------------------------------------------------
// System-wide move/size loop detection. EVENT_SYSTEM_MOVESIZESTART/END fire for
// any window the user drags or resizes. While active, we pause strip repaints.
// ----------------------------------------------------------------------------
static void CALLBACK MoveSizeEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                       LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_MOVESIZESTART) g_moveLoopActive = true;
    else if (event == EVENT_SYSTEM_MOVESIZEEND) {
        g_moveLoopActive = false;
        if (g_hwnd) {
            InvalidateRect(g_hwnd, nullptr, FALSE); // refresh once after
            RECT wr;
            if (GetWindowRect(g_hwnd, &wr))
                strip_save_position(wr.left, wr.top);
        }
    }
    else if (event == EVENT_SYSTEM_FOREGROUND) {
        // Another window came to the foreground - clicked Start, a taskbar item,
        // closed a window, switched apps. Re-evaluate hide state IMMEDIATELY so
        // the strip shows/hides without waiting up to a second for the timer
        // (this is why closing the Start menu used to leave it hidden until the
        // next click). Then re-assert topmost if visible.
        if (g_hwnd) {
            bool fs = is_fullscreen_app_active();
            if (fs && !g_hiddenForFullscreen) {
                g_hiddenForFullscreen = true;
                ShowWindow(g_hwnd, SW_HIDE);
            } else if (!fs && g_hiddenForFullscreen) {
                g_hiddenForFullscreen = false;
                if (!g_hiddenByUser) ShowWindow(g_hwnd, SW_SHOWNA);
            }
            if (!g_hiddenForFullscreen && !g_hiddenByUser) {
                SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Public entry points called from foo_strip.cpp
// ----------------------------------------------------------------------------
void strip_create_window() {
    // Theme is resolved live via theme()/strip_load_theme_mode(); no per-window
    // theme state to restore here anymore.

    // Determine DPI scale so the strip renders at the right physical size on
    // high-DPI monitors. We query the ACTUAL monitor DPI (not GetDeviceCaps on
    // the desktop DC, which can report 96 depending on process DPI-awareness).
    // Before the window exists we use the DPI of the monitor at the strip's
    // saved/most-likely position; after creation we re-confirm with
    // GetDpiForWindow and resize if needed.
    {
        POINT probe;
        int sx, sy;
        if (strip_load_position(sx, sy)) { probe.x = sx; probe.y = sy; }
        else {
            RECT wa{}; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
            probe.x = wa.right - 100; probe.y = wa.bottom - 30;
        }
        UINT dpi = dpi_for_point(probe);
        if (!dpi) {   // fallback for old Windows: desktop DC
            HDC screen = GetDC(nullptr);
            if (screen) { int d = GetDeviceCaps(screen, LOGPIXELSX); if (d > 0) dpi = (UINT)d; ReleaseDC(nullptr, screen); }
        }
        set_scale_from_dpi(dpi);
    }

    GdiplusStartupInput gdiIn;
    GdiplusStartup(&g_gdiplusToken, &gdiIn, nullptr);

    HINSTANCE hInst = core_api::get_my_instance();

    WNDCLASSEX wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;          // enable WM_LBUTTONDBLCLK (art double-click)
    wc.lpfnWndProc = StripProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassEx(&wc);

    // Tool window (off taskbar/alt-tab) + topmost + no activate (don't steal focus).
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    DWORD style = WS_POPUP | WS_VISIBLE;

    // Position: restore the last-dragged spot if we have one, else default to
    // the bottom-right of the work area.
    RECT wa{}; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    int x, y;
    if (strip_load_position(x, y)) {
        // Clamp to the FULL virtual screen (all monitors), not the work area, so
        // a position over the taskbar is preserved. The work area excludes the
        // taskbar, so clamping to it would yank an on-taskbar strip upward. We
        // still guard against fully off-screen (e.g. a detached monitor) by
        // keeping the window within the overall desktop bounds.
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (x < vx) x = vx;
        if (y < vy) y = vy;
        if (x > vx + vw - kWidth()) x = vx + vw - kWidth();
        if (y > vy + vh - kHeight()) y = vy + vh - kHeight();
    } else {
        x = wa.right - kWidth() - 16;
        y = wa.bottom - kHeight() - 16;
    }

    g_hwnd = CreateWindowEx(exStyle, kClassName, L"Foobar Strip", style,
                            x, y, kWidth(), kHeight(),
                            nullptr, nullptr, hInst, nullptr);

    // Now that the window exists, get its TRUE DPI (per-monitor) and, if it
    // differs from the pre-creation estimate, rescale the window to match.
    if (g_hwnd) {
        UINT realDpi = dpi_for_window(g_hwnd);
        if (realDpi && set_scale_from_dpi(realDpi)) {
            SetWindowPos(g_hwnd, nullptr, 0, 0, kWidth(), kHeight(),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // A layered window is invisible until its first UpdateLayeredWindow. Render
    // once right now so the strip appears immediately and deterministically,
    // rather than waiting (and racing) on the first timer tick / WM_PAINT - which
    // layered windows don't reliably receive.
    if (g_hwnd) paint(g_hwnd);

    // ~60fps repaint (16ms) - matches common monitor refresh, smooth for the
    // marquee + scrubber, and reliably delivered by the standard timer. Combined
    // with the conditional repaint below, idle cost stays near zero.
    g_timer = SetTimer(g_hwnd, 1, 16, nullptr);

    // Hook system-wide move/size loops so we can pause repaints during any drag.
    // We must NOT skip our own process: foobar's main window lives in the same
    // process as the strip, and dragging IT is the case we're smoothing.
    g_moveHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZEEND,
        nullptr, MoveSizeEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT);

    // Honor the persisted master visibility: if the user left it hidden, start
    // hidden (the create style includes WS_VISIBLE, so we explicitly hide here).
    g_hiddenByUser = !strip_load_show_strip();
    if (g_hiddenByUser && g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
}

// Apply the master "Show strip" setting: hide or show the whole window. Called
// from the prefs page when the checkbox changes. Re-shows need a fresh paint
// (layered windows show nothing until UpdateLayeredWindow) and topmost re-assert.
void strip_apply_visibility() {
    if (!g_hwnd) return;
    bool show = strip_load_show_strip();
    g_hiddenByUser = !show;
    if (show) {
        ShowWindow(g_hwnd, SW_SHOWNA);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        paint(g_hwnd);   // render immediately (layered)
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void strip_destroy_window() {
    if (g_moveHook) {
        UnhookWinEvent(g_moveHook);
        g_moveHook = nullptr;
    }
    if (g_artPopup) {
        DestroyWindow(g_artPopup);
        g_artPopup = nullptr;
        UnregisterClass(kPopupClass, core_api::get_my_instance());
    }
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClass(kClassName, core_api::get_my_instance());
    invalidate_art_thumb();   // free cached thumbnail while GDI+ is still up
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

void strip_notify_repaint() {
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_artPopup && IsWindowVisible(g_artPopup))
        paint_art_popup_layered(); // re-render enlarged cover (layered)
}

// Re-read settings from config and apply them to the live window. Called by the
// preferences page when the user clicks Apply/OK. For now this resizes the strip
// to the configured width (height unchanged) and repaints. Later milestones will
// extend it to re-create fonts, recolor, etc.
void strip_apply_settings() {
    if (!g_hwnd) return;
    // kWidth()/kHeight() reflect the new configured dimensions automatically.
    // Resize the window to both, keeping its current top-left position.
    RECT wr;
    if (!GetWindowRect(g_hwnd, &wr)) return;
    SetWindowPos(g_hwnd, nullptr, 0, 0, kWidth(), kHeight(),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_artPopup && IsWindowVisible(g_artPopup))
        paint_art_popup_layered();  // live popup color/alpha update (layered)
}

void strip_reset_interp() {
    // Clear the interpolation baseline so the next timer tick re-anchors to the
    // current (new track / seeked) position instead of extrapolating from the
    // old one. Also drop any pending-seek hold, which belongs to the old track.
    g_last_tick_ms = 0;
    g_interp_position = 0.0;
    g_pending_seek = false;
}

void strip_sync_interp(double position) {
    // Called on each per-second position update from foobar. Re-anchor the
    // baseline timestamp to "now" so the timer's dt measures only the gap since
    // THIS reading - otherwise position (jumping each second) and dt (growing)
    // stack and the clock runs at ~2x.
    // While a seek is pending we're deliberately holding the target position, so
    // only update the timestamp, not the displayed value.
    g_last_tick_ms = GetTickCount64();
    if (!g_pending_seek)
        g_interp_position = position;
}
