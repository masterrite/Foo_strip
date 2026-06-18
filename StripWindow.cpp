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

const int kWidth      = 300;
const int kHeight     = 46;   // strip height
const int kPad        = 6;
const int kArt        = 26;   // album art square (top row)
const int kBtn        = 26;   // transport button hit size
const int kSeekH      = 12;   // seek row height
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
const int kPopupSize = 300;          // enlarged art dimension
const wchar_t* kPopupClass = L"FoobarStripArtPopup";

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
    Gdiplus::Color artist;      // artist + time text
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
    Gdiplus::Color(255, 64, 70, 80),   // icon (dark glyphs)
    Gdiplus::Color(255, 198, 202, 208),// groove
    Gdiplus::Color(255, 38, 130, 222), // accent (slightly deeper blue for contrast)
    Gdiplus::Color(255, 60, 66, 76),   // thumb (dark on light)
    Gdiplus::Color(255, 0, 0, 0),      // btnHl (black tint on light bg)
    Gdiplus::Color(255, 248, 249, 251),// popupBg
    Gdiplus::Color(255, 200, 205, 212),// popupBorder
};

bool g_darkMode = true;                 // toggled by right-click; persisted
const Theme& theme() { return g_darkMode ? kDarkTheme : kLightTheme; }

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
    int m = s / 60;
    s %= 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", m, s);
    return buf;
}

// True when a fullscreen app (game / video / presentation) is foreground. Uses
// the same Shell signal the OS uses to suppress notifications during fullscreen.
bool is_fullscreen_app_active() {
    QUERY_USER_NOTIFICATION_STATE state;
    if (SHQueryUserNotificationState(&state) == S_OK) {
        if (state == QUNS_BUSY ||
            state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE)
            return true;
    }

    // Fallback for borderless-windowed games, which Windows may report as a
    // normal maximized window: if the foreground window (not ours, not the
    // desktop/shell) covers its entire monitor, treat it as fullscreen.
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;
    if (fg == GetDesktopWindow() || fg == GetShellWindow()) return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(mon, &mi)) return false;

    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// ----------------------------------------------------------------------------
// Painting
// ----------------------------------------------------------------------------
void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Double-buffer to avoid flicker.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    Graphics g(mem);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background: fill with smoothing OFF and as a fully opaque base so the
    // black double-buffer doesn't bleed through at the edges (which showed as a
    // dark line on the top/left). Antialiasing is enabled afterward for glyphs.
    g.SetSmoothingMode(SmoothingModeNone);
    Color base = theme().bg;
    SolidBrush bg(Color(255, base.GetR(), base.GetG(), base.GetB()));
    g.FillRectangle(&bg, -1, -1, W + 2, H + 2);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    // Snapshot state under lock.
    std::wstring title, artist, timeStr;
    double frac = 0.0;
    bool playing = false, canSeek = false;
    Bitmap* art = nullptr;
    {
        auto& st = strip_get_state();
        std::lock_guard<std::mutex> guard(st.lock);
        title = st.title;
        artist = st.artist;
        playing = st.playing;
        canSeek = st.canSeek;
        art = st.art;
        double pos = g_scrubbing ? g_scrub_preview * st.length : g_interp_position;
        if (st.length > 0) frac = min(1.0, max(0.0, pos / st.length));
        timeStr = fmt_time(pos) + L" / " + fmt_time(st.length);

        // Vertical layout: the seek row occupies the bottom; the top row (art,
        // title, buttons) is centered in the remaining area above it. This keeps
        // everything balanced at 46px without overlap or dead space.
        int seekTop = H - kPad - kSeekH;          // top of the seek row
        int topArea = seekTop;                    // top content area height (0..seekTop)

        // ---- Top row: album art (vertically centered in the top area) ----
        int ax = kPad;
        int ay = (topArea - kArt) / 2;
        if (ay < 2) ay = 2;
        g_rcArt = { ax, ay, ax + kArt, ay + kArt }; // for hover hit-testing
        if (art) {
            g.DrawImage(art, ax, ay, kArt, kArt);
        } else {
            SolidBrush ph(theme().artPh);
            g.FillRectangle(&ph, ax, ay, kArt, kArt);
        }

        // ---- Title + artist ----
        FontFamily ff(L"Segoe UI");
        Font fTitle(&ff, 11, FontStyleBold, UnitPixel);
        Font fArtist(&ff, 9, FontStyleRegular, UnitPixel);
        SolidBrush cTitle(theme().title);
        SolidBrush cArtist(theme().artist);

        int tx = ax + kArt + kPad;
        int textW = W - tx - (kBtn * 3) - kPad * 2;
        if (textW < 10) textW = 10;
        RectF artistRect((REAL)tx, (REAL)ay + 14, (REAL)textW, 14);
        StringFormat sf;
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);

        // (4) Title with marquee. Reset scroll when the title changes.
        static std::wstring s_lastTitle;
        if (title != s_lastTitle) {
            s_lastTitle = title;
            g_marquee_offset = 0.0;
            g_marquee_last_ms = 0;
            g_marquee_phase_ms = 0.0;
        }
        RectF measure;
        g.MeasureString(title.c_str(), -1, &fTitle, PointF(0, 0), &measure);
        g_title_width = measure.Width;
        g_title_avail = textW;

        Region oldClip;
        g.GetClip(&oldClip);
        g.SetClip(RectF((REAL)tx, (REAL)ay - 1, (REAL)textW, 16));
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

            RectF r1((REAL)tx - off, (REAL)ay - 1, (REAL)g_title_width + 20, 16);
            g.DrawString(title.c_str(), -1, &fTitle, r1, &sfNo, &cTitle);

            RectF r2((REAL)tx - off + span, (REAL)ay - 1, (REAL)g_title_width + 20, 16);
            g.DrawString(title.c_str(), -1, &fTitle, r2, &sfNo, &cTitle);
        } else {
            RectF titleRect((REAL)tx, (REAL)ay - 1, (REAL)textW, 16);
            g.DrawString(title.c_str(), -1, &fTitle, titleRect, &sf, &cTitle);
        }
        g.SetClip(&oldClip);

        g.DrawString(artist.c_str(), -1, &fArtist, artistRect, &sf, &cArtist);

        // ---- Transport buttons (right of top row, centered to the art) ----
        int by = ay + (kArt - kBtn) / 2;
        g_rcNext = { W - kPad - kBtn, by, W - kPad, by + kBtn };
        g_rcPlay = { g_rcNext.left - kBtn, by, g_rcNext.left, by + kBtn };
        g_rcPrev = { g_rcPlay.left - kBtn, by, g_rcPlay.left, by + kBtn };

        SolidBrush ico(theme().icon);

        // (1) Button feedback: hover = subtle, pressed = stronger.
        {
            auto drawBtnBg = [&](const RECT& r, int id) {
                int alpha = 0;
                if (g_pressed_btn == id) alpha = 70;
                else if (g_hover_btn == id) alpha = 32;
                if (alpha == 0) return;
                Color t = theme().btnHl;
                SolidBrush hl(Color(alpha, t.GetR(), t.GetG(), t.GetB()));
                g.FillRectangle(&hl, r.left + 2, r.top + 2,
                                (r.right - r.left) - 4, (r.bottom - r.top) - 4);
            };
            drawBtnBg(g_rcPrev, 1);
            drawBtnBg(g_rcPlay, 2);
            drawBtnBg(g_rcNext, 3);
        }

        // prev button
        {
            int cx = (g_rcPrev.left + g_rcPrev.right) / 2;
            int cy = (g_rcPrev.top + g_rcPrev.bottom) / 2;
            g.FillRectangle(&ico, cx - 6, cy - 5, 2, 10);
            PointF tri[3] = { {(REAL)cx + 5, (REAL)cy - 5}, {(REAL)cx + 5, (REAL)cy + 5}, {(REAL)cx - 3, (REAL)cy} };
            g.FillPolygon(&ico, tri, 3);
        }
        // play / pause
        {
            int cx = (g_rcPlay.left + g_rcPlay.right) / 2;
            int cy = (g_rcPlay.top + g_rcPlay.bottom) / 2;
            if (playing) {
                g.FillRectangle(&ico, cx - 5, cy - 6, 3, 12);
                g.FillRectangle(&ico, cx + 2, cy - 6, 3, 12);
            } else {
                PointF tri[3] = { {(REAL)cx - 4, (REAL)cy - 6}, {(REAL)cx - 4, (REAL)cy + 6}, {(REAL)cx + 6, (REAL)cy} };
                g.FillPolygon(&ico, tri, 3);
            }
        }
        // next >|
        {
            int cx = (g_rcNext.left + g_rcNext.right) / 2;
            int cy = (g_rcNext.top + g_rcNext.bottom) / 2;
            PointF tri[3] = { {(REAL)cx - 5, (REAL)cy - 5}, {(REAL)cx - 5, (REAL)cy + 5}, {(REAL)cx + 3, (REAL)cy} };
            g.FillPolygon(&ico, tri, 3);
            g.FillRectangle(&ico, cx + 4, cy - 5, 2, 10);
        }

        // ---- Bottom row: seek bar + time ----
        int sy = H - kPad - kSeekH / 2;
        std::wstring t = timeStr;
        Font fTime(&ff, 9, FontStyleRegular, UnitPixel);
        RectF timeMeasure;
        g.MeasureString(t.c_str(), -1, &fTime, PointF(0, 0), &timeMeasure);
        int timeW = (int)timeMeasure.Width + 4;

        int seekLeft = kPad;
        int seekRight = W - kPad - timeW - kPad;
        g_rcSeek = { seekLeft, H - kPad - kSeekH, seekRight, H - kPad };

        int trackY = (g_rcSeek.top + g_rcSeek.bottom) / 2;
        SolidBrush groove(theme().groove);
        SolidBrush accent(theme().accent);
        g.FillRectangle(&groove, seekLeft, trackY - 1, seekRight - seekLeft, 3);
        int fillW = (int)((seekRight - seekLeft) * frac);
        g.FillRectangle(&accent, seekLeft, trackY - 1, fillW, 3);

        // thumb
        SolidBrush thumb(theme().thumb);
        int thumbX = seekLeft + fillW;
        g.FillEllipse(&thumb, thumbX - 5, trackY - 5, 10, 10);

        // time text
        SolidBrush cTime(theme().artist);
        RectF timeRect((REAL)(seekRight + kPad), (REAL)(g_rcSeek.top - 1), (REAL)(timeW + kPad), (REAL)kSeekH);
        g.DrawString(t.c_str(), -1, &fTime, timeRect, &sf, &cTime);
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ----------------------------------------------------------------------------
// Window procedure
// ----------------------------------------------------------------------------
LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        auto& st = strip_get_state();
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
                ShowWindow(hwnd, SW_SHOWNA); // show without activating
            }

            // Re-assert topmost z-order. A one-time WS_EX_TOPMOST at creation
            // isn't enough to stay above the taskbar (itself topmost) or other
            // topmost windows that leapfrog us. Nudging back to HWND_TOPMOST
            // ~1/sec keeps the strip visible without moving/activating it.
            if (!g_hiddenForFullscreen) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        if (g_hiddenForFullscreen) return 0; // nothing to draw while hidden

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
        // motion is endless and seamless — no slide-back.
        bool marqueeActive = (g_title_width > g_title_avail && g_title_avail > 0);
        if (marqueeActive) {
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
        } else {
            g_marquee_offset = 0.0;
            g_marquee_last_ms = 0;
            g_marquee_phase_ms = 0.0;
        }

        // Only repaint when something is actually animating — AND not while a
        // window move/size loop is active anywhere, so the strip doesn't steal
        // UI-thread time and make dragging (foobar or the strip) choppy.
        bool needsRepaint = !g_moveLoopActive && (
            (st.playing && !g_scrubbing) ||  // seek bar advancing
            g_scrubbing ||                   // user dragging the scrubber
            g_pending_seek ||                // holding post-seek position
            marqueeActive ||                 // title scrolling
            g_pressed_btn != 0 ||            // button pressed
            g_hover_btn != 0);               // button hovered

        if (needsRepaint)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);

        if (pt_in(g_rcSeek, x, y)) {
            g_scrubbing = true;
            double f = (double)(x - g_rcSeek.left) / (g_rcSeek.right - g_rcSeek.left);
            g_scrub_preview = min(1.0, max(0.0, f));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcPrev, x, y) || pt_in(g_rcPlay, x, y) || pt_in(g_rcNext, x, y)) {
            // Record which button is pressed for visual feedback; fired on up.
            if (pt_in(g_rcPrev, x, y)) g_pressed_btn = 1;
            else if (pt_in(g_rcPlay, x, y)) g_pressed_btn = 2;
            else g_pressed_btn = 3;
            InvalidateRect(hwnd, nullptr, FALSE);
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
            if (pt_in(g_rcPrev, x, y)) h = 1;
            else if (pt_in(g_rcPlay, x, y)) h = 2;
            else if (pt_in(g_rcNext, x, y)) h = 3;
            if (h != g_hover_btn) {
                g_hover_btn = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // Album-art hover → show/hide the enlarged popup.
            bool overArt = pt_in(g_rcArt, x, y);
            if (overArt && !g_artHover) {
                g_artHover = true;
                show_art_popup();
            } else if (!overArt && g_artHover) {
                g_artHover = false;
                hide_art_popup();
            }
        }

        if (g_scrubbing) {
            double f = (double)(x - g_rcSeek.left) / (g_rcSeek.right - g_rcSeek.left);
            g_scrub_preview = min(1.0, max(0.0, f));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ReleaseCapture();

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
            if (g_pressed_btn == 1 && pt_in(g_rcPrev, x, y)) {
                pc->previous();
            } else if (g_pressed_btn == 2 && pt_in(g_rcPlay, x, y)) {
                // (1) If stopped, start playback; otherwise toggle pause. This is
                // why the play button did nothing before a song was started.
                if (pc->is_playing()) pc->toggle_pause();
                else pc->start(playback_control::track_command_play, false);
            } else if (g_pressed_btn == 3 && pt_in(g_rcNext, x, y)) {
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
        // Toggle light/dark. Repaint strip + popup, and persist the choice.
        g_darkMode = !g_darkMode;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (g_artPopup && IsWindowVisible(g_artPopup))
            InvalidateRect(g_artPopup, nullptr, FALSE);
        strip_save_dark_mode(g_darkMode); // persist via foobar cfg
        return 0;

    case WM_ERASEBKGND:
        return 1; // we fully paint in WM_PAINT (double-buffered)

    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_timer) { KillTimer(hwnd, g_timer); g_timer = 0; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// Album-art enlarge popup
// ----------------------------------------------------------------------------
LRESULT CALLBACK ArtPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Backdrop + thin border so it reads as a panel over any wallpaper.
        SolidBrush bg(theme().popupBg);
        g.FillRectangle(&bg, 0, 0, W, H);

        {
            auto& st = strip_get_state();
            std::lock_guard<std::mutex> guard(st.lock);
            if (st.art) {
                g.DrawImage(st.art, 4, 4, W - 8, H - 8); // scaled up, small inset
            } else {
                SolidBrush ph(theme().artPh);
                g.FillRectangle(&ph, 4, 4, W - 8, H - 8);
            }
        }
        Pen border(theme().popupBorder, 1.0f);
        g.DrawRectangle(&border, 0, 0, W - 1, H - 1);

        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
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
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kPopupClass, L"", WS_POPUP,
        0, 0, kPopupSize, kPopupSize,
        nullptr, nullptr, hInst, nullptr);
}

void show_art_popup() {
    ensure_art_popup_created();
    if (!g_artPopup || !g_hwnd) return;

    // Center the enlarged cover horizontally over the strip so their edges
    // line up cleanly (strip and popup are the same width).
    RECT sr; GetWindowRect(g_hwnd, &sr);
    int stripW = sr.right - sr.left;
    int x = sr.left + (stripW - kPopupSize) / 2;
    int yAbove = sr.top - kPopupSize - 6;
    int yBelow = sr.bottom + 6;

    // Clamp only if it would actually leave the screen work area.
    RECT wa{}; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + kPopupSize > wa.right) x = wa.right - kPopupSize;
    if (x < wa.left) x = wa.left;

    int y = (yAbove >= wa.top) ? yAbove : yBelow; // prefer above, else below

    SetWindowPos(g_artPopup, HWND_TOPMOST, x, y, kPopupSize, kPopupSize,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_artPopup, nullptr, FALSE);
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
            // Persist the new position so the strip reopens where you left it.
            RECT wr;
            if (GetWindowRect(g_hwnd, &wr))
                strip_save_position(wr.left, wr.top);
        }
    }
}

// ----------------------------------------------------------------------------
// Public entry points called from foo_strip.cpp
// ----------------------------------------------------------------------------
void strip_create_window() {
    g_darkMode = strip_load_dark_mode(); // restore persisted theme

    GdiplusStartupInput gdiIn;
    GdiplusStartup(&g_gdiplusToken, &gdiIn, nullptr);

    HINSTANCE hInst = core_api::get_my_instance();

    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = StripProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassEx(&wc);

    // Tool window (off taskbar/alt-tab) + topmost + no activate (don't steal focus).
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
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
        if (x > vx + vw - kWidth) x = vx + vw - kWidth;
        if (y > vy + vh - kHeight) y = vy + vh - kHeight;
    } else {
        x = wa.right - kWidth - 16;
        y = wa.bottom - kHeight - 16;
    }

    g_hwnd = CreateWindowEx(exStyle, kClassName, L"Foobar Strip", style,
                            x, y, kWidth, kHeight,
                            nullptr, nullptr, hInst, nullptr);

    // ~60fps repaint (16ms) — matches common monitor refresh, smooth for the
    // marquee + scrubber, and reliably delivered by the standard timer. Combined
    // with the conditional repaint below, idle cost stays near zero.
    g_timer = SetTimer(g_hwnd, 1, 16, nullptr);

    // Hook system-wide move/size loops so we can pause repaints during any drag.
    // We must NOT skip our own process: foobar's main window lives in the same
    // process as the strip, and dragging IT is the case we're smoothing.
    g_moveHook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
        nullptr, MoveSizeEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT);
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
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

void strip_notify_repaint() {
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_artPopup && IsWindowVisible(g_artPopup))
        InvalidateRect(g_artPopup, nullptr, FALSE); // refresh enlarged cover too
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
    // THIS reading — otherwise position (jumping each second) and dt (growing)
    // stack and the clock runs at ~2x.
    // While a seek is pending we're deliberately holding the target position, so
    // only update the timestamp, not the displayed value.
    g_last_tick_ms = GetTickCount64();
    if (!g_pending_seek)
        g_interp_position = position;
}
