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
const int kBaseWidth  = 300;
const int kBaseHeight = 46;   // strip height
const int kBasePad    = 6;
const int kBaseArt    = 34;   // album art square — taller, fills more height
const int kBaseBtn    = 34;   // transport button hit size (bigger)
const int kBaseSeekH  = 10;   // seek row height — shorter

// DPI scale factor (1.0 at 96 DPI / 100%). Set in strip_create_window.
double g_scale = 1.0;

// Scaled pixel sizes — recomputed from g_scale. Used everywhere in layout/paint.
inline int kWidth()  { return (int)(kBaseWidth  * g_scale + 0.5); }
inline int kHeight() { return (int)(kBaseHeight * g_scale + 0.5); }
inline int kPad()    { return (int)(kBasePad    * g_scale + 0.5); }
inline int kArt()    { return (int)(kBaseArt    * g_scale + 0.5); }
inline int kBtn()    { return (int)(kBaseBtn    * g_scale + 0.5); }
inline int kSeekH()  { return (int)(kBaseSeekH  * g_scale + 0.5); }
inline float kFont(float base) { return (float)(base * g_scale); } // scaled font px
inline int S(int n) { return (int)(n * g_scale + 0.5); }           // scale a literal
inline float Sf(float n) { return (float)(n * g_scale); }          // scale (float)

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
inline int kPopupSize() { return (int)(300 * g_scale + 0.5); } // enlarged art dim
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
RECT g_rcVol{};                      // volume slider track rect
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
    int m = s / 60;
    s %= 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", m, s);
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

        // Album art fills (almost) the full strip height as a square — the
        // dominant element, matching the mockup. Title/buttons sit to its right,
        // seek bar spans below to the right of the art.
        int artSize = H - kPad();              // near-full height, small top/bottom inset
        int ax = kPad() / 2;
        int ay = kPad() / 2;
        g_rcArt = { ax, ay, ax + artSize, ay + artSize }; // hover hit-testing
        if (art) {
            g.DrawImage(art, ax, ay, artSize, artSize);
        } else {
            SolidBrush ph(theme().artPh);
            g.FillRectangle(&ph, ax, ay, artSize, artSize);
        }

        // ---- Title + artist ----
        FontFamily ff(L"Segoe UI");
        Font fTitle(&ff, kFont(13), FontStyleBold, UnitPixel);
        Font fArtist(&ff, kFont(10), FontStyleRegular, UnitPixel);
        SolidBrush cTitle(theme().title);
        SolidBrush cArtist(theme().title);   // artist same color as title now

        int tx = ax + artSize + kPad();
        // The top row's right side holds: 3 transport buttons + speaker + volume
        // slider. The title/artist text must stop before all of that.
        int rightControls = (kBtn() * 3) + S(18) + S(2) + S(46) + S(4);
        int textW = W - tx - rightControls - kPad();
        if (textW < 10) textW = 10;
        // Title at top, artist just below. Sized to leave room below for the
        // seek bar with padding from the bottom edge.
        int titleY = S(3);
        int artistY = titleY + S(15);
        RectF artistRect((REAL)tx, (REAL)artistY, (REAL)textW, Sf(13));
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
        g.SetClip(RectF((REAL)tx, (REAL)titleY, (REAL)textW, Sf(17)));
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

            RectF r1((REAL)tx - off, (REAL)titleY, (REAL)g_title_width + Sf(20), Sf(17));
            g.DrawString(title.c_str(), -1, &fTitle, r1, &sfNo, &cTitle);

            RectF r2((REAL)tx - off + span, (REAL)titleY, (REAL)g_title_width + Sf(20), Sf(17));
            g.DrawString(title.c_str(), -1, &fTitle, r2, &sfNo, &cTitle);
        } else {
            RectF titleRect((REAL)tx, (REAL)titleY, (REAL)textW, Sf(17));
            g.DrawString(title.c_str(), -1, &fTitle, titleRect, &sf, &cTitle);
        }
        g.SetClip(&oldClip);

        g.DrawString(artist.c_str(), -1, &fArtist, artistRect, &sf, &cArtist);

        // ---- Transport buttons (right side, in the upper content band) ----
        // Keep the buttons' vertical extent within the title/artist band so the
        // hover/press highlight never reaches the seek/time row below.
        int bandTop = titleY;
        int bandBot = artistY + S(13);                   // bottom of the artist text
        int bandMid = (bandTop + bandBot) / 2 - S(2);    // nudged up 2px
        int bh = bandBot - bandTop;                      // highlight height
        int btnTop = bandMid - bh / 2;
        int btnBot = bandMid + bh / 2;

        // Right edge of the top row, laid out right-to-left:
        //   [ prev ][ play ][ next ]  [speaker] [volume slider]
        int volW = S(46);                                // volume track width
        int muteW = S(18);                               // speaker icon box
        int volRight = W - kPad();
        int volLeft = volRight - volW;
        g_rcVol = { volLeft, bandMid - S(5), volRight, bandMid + S(5) };
        g_rcMute = { volLeft - muteW - S(2), bandMid - muteW / 2,
                     volLeft - S(2), bandMid + muteW / 2 };

        int btnRight = g_rcMute.left - S(4);             // buttons end before speaker
        g_rcNext = { btnRight - kBtn(), btnTop, btnRight, btnBot };
        g_rcPlay = { g_rcNext.left - kBtn(), btnTop, g_rcNext.left, btnBot };
        g_rcPrev = { g_rcPlay.left - kBtn(), btnTop, g_rcPlay.left, btnBot };

        // (1) Button feedback: hover = subtle, pressed = stronger. The highlight
        // is inset from the rect so adjacent buttons/time text stay clear.
        {
            auto drawBtnBg = [&](const RECT& r, int id) {
                int alpha = 0;
                if (g_pressed_btn == id) alpha = 70;
                else if (g_hover_btn == id) alpha = 32;
                if (alpha == 0) return;
                Color t = theme().btnHl;
                SolidBrush hl(Color(alpha, t.GetR(), t.GetG(), t.GetB()));
                g.FillRectangle(&hl, r.left + S(2), r.top + S(2),
                                (r.right - r.left) - S(4), (r.bottom - r.top) - S(4));
            };
            drawBtnBg(g_rcPrev, 1);
            drawBtnBg(g_rcPlay, 2);
            drawBtnBg(g_rcNext, 3);
        }

        // Modern outline glyphs: shapes are stroked, not filled. Antialiasing is
        // already on. Stroke width scales with DPI; round joins/caps look clean.
        Pen pen(theme().icon, Sf(1.6f));
        pen.SetLineJoin(LineJoinRound);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);

        // prev |<
        {
            int cx = (g_rcPrev.left + g_rcPrev.right) / 2;
            int cy = (g_rcPrev.top + g_rcPrev.bottom) / 2;
            g.DrawLine(&pen, (REAL)cx - Sf(7), (REAL)cy - Sf(7), (REAL)cx - Sf(7), (REAL)cy + Sf(7));
            PointF t2[3] = { {(REAL)cx + Sf(6), (REAL)cy - Sf(7)}, {(REAL)cx - Sf(5), (REAL)cy}, {(REAL)cx + Sf(6), (REAL)cy + Sf(7)} };
            g.DrawPolygon(&pen, t2, 3);
        }
        // play / pause
        {
            int cx = (g_rcPlay.left + g_rcPlay.right) / 2;
            int cy = (g_rcPlay.top + g_rcPlay.bottom) / 2;
            if (playing) {
                g.DrawLine(&pen, (REAL)cx - Sf(5), (REAL)cy - Sf(8), (REAL)cx - Sf(5), (REAL)cy + Sf(8));
                g.DrawLine(&pen, (REAL)cx + Sf(5), (REAL)cy - Sf(8), (REAL)cx + Sf(5), (REAL)cy + Sf(8));
            } else {
                PointF tri[3] = { {(REAL)cx - Sf(6), (REAL)cy - Sf(9)}, {(REAL)cx + Sf(9), (REAL)cy}, {(REAL)cx - Sf(6), (REAL)cy + Sf(9)} };
                g.DrawPolygon(&pen, tri, 3);
            }
        }
        // next >|
        {
            int cx = (g_rcNext.left + g_rcNext.right) / 2;
            int cy = (g_rcNext.top + g_rcNext.bottom) / 2;
            PointF tri[3] = { {(REAL)cx - Sf(6), (REAL)cy - Sf(7)}, {(REAL)cx + Sf(5), (REAL)cy}, {(REAL)cx - Sf(6), (REAL)cy + Sf(7)} };
            g.DrawPolygon(&pen, tri, 3);
            g.DrawLine(&pen, (REAL)cx + Sf(7), (REAL)cy - Sf(7), (REAL)cx + Sf(7), (REAL)cy + Sf(7));
        }

        // ---- Volume: speaker icon (mute toggle) + slider ----
        {
            // Still inside the paint lock scope, so read directly from st.
            double volLinear = st.volume_linear;
            bool muted = st.muted;

            int mcx = (g_rcMute.left + g_rcMute.right) / 2;
            int mcy = (g_rcMute.top + g_rcMute.bottom) / 2;
            // Speaker body (small trapezoid-ish): a rectangle + triangle cone.
            g.DrawLine(&pen, (REAL)mcx - Sf(5), (REAL)mcy - Sf(2), (REAL)mcx - Sf(5), (REAL)mcy + Sf(2)); // back
            PointF spk[4] = {
                {(REAL)mcx - Sf(5), (REAL)mcy - Sf(2)},
                {(REAL)mcx - Sf(1), (REAL)mcy - Sf(5)},
                {(REAL)mcx - Sf(1), (REAL)mcy + Sf(5)},
                {(REAL)mcx - Sf(5), (REAL)mcy + Sf(2)} };
            g.DrawPolygon(&pen, spk, 4);
            if (muted) {
                // Muted: an X to the right of the speaker.
                g.DrawLine(&pen, (REAL)mcx + Sf(1), (REAL)mcy - Sf(3), (REAL)mcx + Sf(5), (REAL)mcy + Sf(3));
                g.DrawLine(&pen, (REAL)mcx + Sf(5), (REAL)mcy - Sf(3), (REAL)mcx + Sf(1), (REAL)mcy + Sf(3));
            } else {
                // Sound waves: two small arcs.
                g.DrawArc(&pen, (REAL)mcx - Sf(1), (REAL)mcy - Sf(4), Sf(6), Sf(8), -60.0f, 120.0f);
            }

            // Volume slider track.
            int vTrackY = (g_rcVol.top + g_rcVol.bottom) / 2;
            int vLeft = g_rcVol.left, vRight = g_rcVol.right;
            int vgh = S(3);
            SolidBrush vgroove(theme().groove);
            SolidBrush vaccent(theme().accent);
            g.FillRectangle(&vgroove, vLeft, vTrackY - vgh / 2, vRight - vLeft, vgh);
            double vfrac = muted ? 0.0 : volLinear;
            if (vfrac < 0) vfrac = 0; if (vfrac > 1) vfrac = 1;
            int vfill = (int)((vRight - vLeft) * vfrac);
            g.FillRectangle(&vaccent, vLeft, vTrackY - vgh / 2, vfill, vgh);
            // Volume thumb.
            int vtr = S(4);
            SolidBrush vthumb(theme().thumb);
            int vthumbX = vLeft + vfill;
            g.FillEllipse(&vthumb, vthumbX - vtr, vTrackY - vtr, vtr * 2, vtr * 2);
        }

        // ---- Bottom row: seek bar + time ----
        int sy = H - kPad() - kSeekH() / 2;
        std::wstring t = timeStr;
        Font fTime(&ff, kFont(12), FontStyleRegular, UnitPixel);
        RectF timeMeasure;
        g.MeasureString(t.c_str(), -1, &fTime, PointF(0, 0), &timeMeasure);
        int timeW = (int)timeMeasure.Width + 4;

        // Seek bar starts to the RIGHT of the album art (shorter, like the
        // mockup) and runs to just before the time text.
        int seekLeft = ax + artSize + kPad();
        int seekRight = W - kPad() - timeW - kPad();
        if (seekRight < seekLeft + S(20)) seekRight = seekLeft + S(20); // min length

        // Track vertical center: leave a full pad of clearance below so it isn't
        // flush against the bottom edge, while staying clear of the artist text.
        int tr = S(5);                                  // thumb radius
        int trackY = H - kPad() - tr;                   // padded above the bottom
        g_rcSeek = { seekLeft, trackY - tr, seekRight, trackY + tr };

        int gh = S(3);                                  // groove thickness, scaled
        SolidBrush groove(theme().groove);
        SolidBrush accent(theme().accent);
        g.FillRectangle(&groove, seekLeft, trackY - gh / 2, seekRight - seekLeft, gh);
        int fillW = (int)((seekRight - seekLeft) * frac);
        g.FillRectangle(&accent, seekLeft, trackY - gh / 2, fillW, gh);

        // thumb (scaled with DPI)
        SolidBrush thumb(theme().thumb);
        int thumbX = seekLeft + fillW;
        g.FillEllipse(&thumb, thumbX - tr, trackY - tr, tr * 2, tr * 2);

        // time text — vertically centered on the track
        SolidBrush cTime(theme().title);   // same color as title/artist
        RectF timeRect((REAL)(seekRight + kPad()), (REAL)(trackY - S(8)),
                       (REAL)(timeW + kPad()), (REAL)S(16));
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
        }

        // Re-assert topmost z-order frequently (every ~200ms, separate from the
        // 1s fullscreen check). A one-time WS_EX_TOPMOST isn't enough to stay
        // above the taskbar — when the strip sits ON the taskbar and you open
        // Start, click a taskbar item, or switch monitors, the taskbar raises
        // above the strip. Frequent re-assertion pulls it back promptly. This is
        // the ceiling for a normal window; we can't permanently beat the taskbar,
        // but ~5x/sec keeps it visible in practice.
        static ULONGLONG s_lastTop = 0;
        if (!g_hiddenForFullscreen && now - s_lastTop >= 200) {
            s_lastTop = now;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
            g_volScrubbing ||                // user dragging the volume slider
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
        } else if (pt_in(g_rcMute, x, y)) {
            strip_toggle_mute();   // fires on_volume_change -> refresh + repaint
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (pt_in(g_rcVol, x, y)) {
            g_volScrubbing = true;
            double f = (double)(x - g_rcVol.left) / (g_rcVol.right - g_rcVol.left);
            strip_set_volume(min(1.0, max(0.0, f))); // fires on_volume_change
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
            // Only pop the enlarged view if there's actually album art to show —
            // otherwise hovering an empty placeholder showed a blank square.
            bool haveArt;
            { auto& st = strip_get_state();
              std::lock_guard<std::mutex> g(st.lock); haveArt = (st.art != nullptr); }
            if (overArt && haveArt && !g_artHover) {
                g_artHover = true;
                show_art_popup();
            } else if ((!overArt || !haveArt) && g_artHover) {
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

    case WM_WINDOWPOSCHANGING: {
        // Edge snapping — only while SHIFT is held. By default the strip moves
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

        // Backdrop fills the popup; the art sits inset inside it so the backdrop
        // shows as a padded frame around the cover (the original look).
        int inset = S(6);
        SolidBrush bg(theme().popupBg);
        g.FillRectangle(&bg, 0, 0, W, H);
        {
            auto& st = strip_get_state();
            std::lock_guard<std::mutex> guard(st.lock);
            if (st.art) {
                g.DrawImage(st.art, inset, inset, W - inset * 2, H - inset * 2);
            } else {
                SolidBrush ph(theme().artPh);
                g.FillRectangle(&ph, inset, inset, W - inset * 2, H - inset * 2);
            }
        }

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
        0, 0, kPopupSize(), kPopupSize(),
        nullptr, nullptr, hInst, nullptr);
}

void show_art_popup() {
    ensure_art_popup_created();
    if (!g_artPopup || !g_hwnd) return;

    // Center the enlarged cover horizontally over the strip so their edges
    // line up cleanly (strip and popup are the same width).
    RECT sr; GetWindowRect(g_hwnd, &sr);
    int stripW = sr.right - sr.left;
    int x = sr.left + (stripW - kPopupSize()) / 2;
    int yAbove = sr.top - kPopupSize() - 6;
    int yBelow = sr.bottom + 6;

    // Clamp only if it would actually leave the screen work area.
    RECT wa{}; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + kPopupSize() > wa.right) x = wa.right - kPopupSize();
    if (x < wa.left) x = wa.left;

    int y = (yAbove >= wa.top) ? yAbove : yBelow; // prefer above, else below

    SetWindowPos(g_artPopup, HWND_TOPMOST, x, y, kPopupSize(), kPopupSize(),
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
            RECT wr;
            if (GetWindowRect(g_hwnd, &wr))
                strip_save_position(wr.left, wr.top);
        }
    }
    else if (event == EVENT_SYSTEM_FOREGROUND) {
        // Another window came to the foreground — clicked Start, a taskbar item,
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
                ShowWindow(g_hwnd, SW_SHOWNA);
            }
            if (!g_hiddenForFullscreen) {
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
    g_darkMode = strip_load_dark_mode(); // restore persisted theme

    // Determine DPI scale so the strip renders at a sensible physical size on
    // high-DPI displays instead of tiny. We read the DPI for the monitor where
    // the strip will appear. GetDpiForSystem is available on Win10+; fall back
    // to the desktop DC if absent. 96 DPI == 100% == scale 1.0.
    UINT dpi = 96;
    HDC screen = GetDC(nullptr);
    if (screen) {
        int d = GetDeviceCaps(screen, LOGPIXELSX);
        if (d > 0) dpi = (UINT)d;
        ReleaseDC(nullptr, screen);
    }
    g_scale = (double)dpi / 96.0;
    if (g_scale < 1.0) g_scale = 1.0; // never shrink below the base design size

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
        if (x > vx + vw - kWidth()) x = vx + vw - kWidth();
        if (y > vy + vh - kHeight()) y = vy + vh - kHeight();
    } else {
        x = wa.right - kWidth() - 16;
        y = wa.bottom - kHeight() - 16;
    }

    g_hwnd = CreateWindowEx(exStyle, kClassName, L"Foobar Strip", style,
                            x, y, kWidth(), kHeight(),
                            nullptr, nullptr, hInst, nullptr);

    // ~60fps repaint (16ms) — matches common monitor refresh, smooth for the
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
