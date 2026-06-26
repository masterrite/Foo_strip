// ============================================================================
// StripPrefs.cpp - foobar2000 preferences page for the Floating Strip.
//
// MENTAL MODEL (so this file is readable):
//   * A "preferences page" is a SERVICE we register with foobar. foobar calls
//     our page only when the user opens Preferences - it costs nothing at rest.
//   * foobar asks our page four things: its name, its unique GUID, where it
//     lives in the settings tree (parent GUID), and - when the user clicks it -
//     to instantiate() the actual settings window.
//   * instantiate() returns a "preferences_page_instance": the live window plus
//     apply()/reset()/get_state() so foobar's OK/Cancel/Apply buttons work.
//
// MILESTONE 1 (this version): the page appears in Preferences and opens via
// right-click, but contains only a placeholder label. Real settings + tabs come
// next, once we've confirmed this scaffold loads.
// ============================================================================

#include "stdafx.h"
#include "strip_shared.h"
#include <windowsx.h>
#include <commctrl.h>   // trackbar (slider) control
#include <commdlg.h>    // ChooseColor dialog (CHOOSECOLOR, CC_* flags)
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")  // ChooseColor (background color picker)
#include "foobar2000/SDK/coreDarkMode.h"

namespace {

// Our page's unique identity. foobar uses this GUID to tell our page apart from
// every other component's page, and it's the same GUID the strip's right-click
// passes to show_preferences() to open straight to us. (Generated random GUID.)
// {7F3C1A92-5E44-4B8D-9A21-6C0F2D7E5B30}
const GUID g_prefs_page_guid =
    { 0x7f3c1a92, 0x5e44, 0x4b8d,
      { 0x9a, 0x21, 0x6c, 0x0f, 0x2d, 0x7e, 0x5b, 0x30 } };

// ----------------------------------------------------------------------------
// The live settings window. foobar creates one of these when the user opens our
// page (via instantiate()), and destroys it when they leave the page.
//
// For Milestone 1 it's just a child window with a placeholder label. We build
// the controls in code (no .rc resource file) to avoid changing the project's
// resource setup for now.
// ----------------------------------------------------------------------------
// Control IDs.
enum {
    ID_EDIT_W = 1001, ID_SLIDER_W = 1002,
    ID_EDIT_H = 1003, ID_SLIDER_H = 1004,
    ID_THEME_MODE = 1005,   // theme combobox
    ID_TABS = 1006,         // the Size/Color/Text tab control
    ID_EDIT_P = 1007, ID_SLIDER_P = 1008,   // popup size
    ID_FONT_FACE = 1009,                     // font family combobox
    ID_EDIT_PA = 1015, ID_SLIDER_PA = 1016,  // popup alpha (transparency)
    ID_EDIT_BA = 1017, ID_SLIDER_BA = 1018,  // strip background alpha
    ID_COLOR_BASE = 1010,   // color buttons: ID_COLOR_BASE + i (i = 0..5)
    ID_FEDIT_BASE = 1020,   // font size edits:   ID_FEDIT_BASE + k (k=0..2)
    ID_FSLIDER_BASE = 1023, // font size sliders:  ID_FSLIDER_BASE + k
    ID_EDIT_IT = 1030, ID_SLIDER_IT = 1031,  // transport icon size
    ID_EDIT_IS = 1032, ID_SLIDER_IS = 1033,  // speaker icon size
    ID_SHOW_VOLUME = 1034,                    // show/hide volume checkbox
    ID_SHOW_STRIP = 1039,                     // master show/hide strip checkbox
    ID_EDIT_SB = 1035, ID_SLIDER_SB = 1036,   // spacing between buttons
    ID_EDIT_SV = 1037, ID_SLIDER_SV = 1038,   // spacing buttons-to-volume
    ID_SHOW_POPUP = 1040,                     // show/hide album-art popup checkbox
};
enum { kNumColors = 6 };   // 0 bg,1 text,2 buttons,3 fill,4 track,5 popup padding
enum { kNumFonts = 3 };    // 0 title, 1 artist, 2 time
// Allowed ranges (must match the clamps in strip_load_*).
enum { W_MIN = 300, W_MAX = 800, H_MIN = 40, H_MAX = 120, P_MIN = 150, P_MAX = 600,
       F_MIN = 6, F_MAX = 48, PA_MIN = 0, PA_MAX = 255, IC_MIN = 8, IC_MAX = 60,
       SP_MIN = 0, SP_MAX = 40 };

class strip_prefs_instance : public preferences_page_instance {
public:
    strip_prefs_instance(fb2k::hwnd_t parent, preferences_page_callback::ptr callback)
        : m_callback(callback)
    {
        m_building = true;   // ignore control events fired during construction
        HINSTANCE inst = core_api::get_my_instance();

        // Make sure the trackbar (slider) control class is registered.
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&icc);

        // Host child window. foobar embeds this; it hosts the tab control, which
        // in turn frames the scrollable content panel (m_content). The host itself
        // does not scroll - the panel inside the tab's display area does.
        m_hwnd = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, (HWND)parent, nullptr, inst, nullptr);

        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
        m_prevProc = (WNDPROC)SetWindowLongPtrW(
            m_hwnd, GWLP_WNDPROC, (LONG_PTR)&host_proc);

        // Use foobar's dialog font for our controls. Child controls created via
        // CreateWindowExW otherwise default to the old system font (blocky, wrong
        // size); foobar's pages use the shell dialog font. Inherit the parent's
        // font if it has one, else fall back to the modern default GUI font.
        m_bodyFont = (HFONT)SendMessageW((HWND)parent, WM_GETFONT, 0, 0);
        if (!m_bodyFont) m_bodyFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        m_ownBodyFont = false;   // borrowed (parent's or stock) - don't delete it
        if (m_bodyFont) SendMessageW(m_hwnd, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);

        // ---- Tab control: Size / Color / Text ----
        INITCOMMONCONTROLSEX icc2{ sizeof(icc2), ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc2);
        m_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            8, 8, 360, 28, m_hwnd, (HMENU)(INT_PTR)ID_TABS, inst, nullptr);
        const wchar_t* tabNames[3] = { L"General", L"Size", L"Text" };
        for (int i = 0; i < 3; i++) {
            TCITEMW ti{}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)tabNames[i];
            SendMessageW(m_tabs, TCM_INSERTITEMW, i, (LPARAM)&ti);
        }
        if (m_bodyFont) SendMessageW(m_tabs, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);

        // Scale the layout by the real DPI. Windows does not stretch our window
        // (confirmed by measurement), so we size everything ourselves. Use
        // GetDpiForWindow (per-monitor accurate on Win10+); fall back to the
        // device-caps DPI, else 96. Coordinates are designed at 96 DPI (100%).
        {
            UINT dpi = 0;
            HMODULE u = GetModuleHandleW(L"user32.dll");
            if (u) {
                typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
                auto fn = (GetDpiForWindow_t)GetProcAddress(u, "GetDpiForWindow");
                if (fn) dpi = fn(m_hwnd);
            }
            if (!dpi) {
                HDC dc = GetDC(m_hwnd);
                if (dc) { int d = GetDeviceCaps(dc, LOGPIXELSX); if (d > 0) dpi = (UINT)d; ReleaseDC(m_hwnd, dc); }
            }
            if (!dpi) dpi = 96;
            m_uiScale = (double)dpi / 96.0;
            if (m_uiScale < 1.0) m_uiScale = 1.0;
        }
        auto SU = [s = m_uiScale](int n) { return (int)(n * s + 0.5); };

        // The tab control fills the whole host content area so its raised
        // display-area frame wraps the settings. The body controls live on a
        // separate panel (m_content) placed at the tab's DISPLAY rectangle -
        // i.e. inside that frame.
        //
        // m_content is a REGISTERED window class, not a "STATIC". A STATIC derives
        // its background from its PARENT's WM_CTLCOLORSTATIC response - and our
        // panel's parent is the tab control, whose dark hook (CTabsHook) answers no
        // WM_CTLCOLOR* at all, so a STATIC panel always painted with the default
        // light brush no matter what we did. A plain class with a NULL background
        // brush instead delivers WM_ERASEBKGND straight to our wndproc, where we
        // fill with foobar's themed brush (see onMsg). AddDialog() then stacks
        // CDialogHook on top to theme the child controls' WM_CTLCOLOR*.
        static const wchar_t* kPanelClass = L"FoobarStripPrefsPanel";
        {
            static bool s_registered = false;
            if (!s_registered) {
                WNDCLASSEXW wcx{ sizeof(wcx) };
                wcx.lpfnWndProc   = &host_proc;     // direct class proc, no STATIC base
                wcx.hInstance     = inst;
                wcx.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
                wcx.hbrBackground  = nullptr;        // we erase ourselves (themed)
                wcx.lpszClassName = kPanelClass;
                RegisterClassExW(&wcx);
                s_registered = true;
            }
        }
        m_content = CreateWindowExW(
            0, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
            0, 0, 0, 0, m_tabs, nullptr, inst, nullptr);
        SetWindowLongPtrW(m_content, GWLP_USERDATA, (LONG_PTR)this);
        // No SetWindowLongPtr(GWLP_WNDPROC) here: host_proc IS the class proc, so
        // there's no original proc to chain to (DefWindowProc handles the rest).
        m_prevContentProc = nullptr;
        if (m_bodyFont) SendMessageW(m_content, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);

        // Size the tab control to fill the host, then place m_content at the tab's
        // interior display rect. layoutTabs() repeats this on every WM_SIZE.
        layoutTabs();

        // Layout constants in design pixels; SU() scales them to the real font.
        // Columns are pulled in slightly vs. the old free-floating layout: the
        // content now sits inside the tab frame AND reserves a vertical scrollbar,
        // which together eat ~20 design px of width, so the right column (edits)
        // must start sooner or it would slip under the scrollbar / frame edge.
        const int LBL_X = SU(10), CTL_X = SU(120), EDIT_X = SU(286), CW = SU(322);
        // Body controls are laid out in m_content's client space. Its origin is
        // already inside the tab frame (below the tab row), so the first row needs
        // only a small top margin, not room to clear the tab strip.
        const int y0 = SU(12);        // first row top margin inside the panel
        const int ROW = SU(32);       // vertical step between rows
        const int HEAD = SU(26);      // extra space a heading consumes above rows
        // Label baseline vs its control. The +4 nudge is a design-pixel value and
        // MUST scale with DPI like every other offset, or labels drift out of
        // alignment with their (scaled) controls at high DPI.
        auto lblY = [&](int rowY) { return rowY + SU(4); };

        // ================= GENERAL tab (group 0) =================
        {
            int y = y0;
            // ---- Visibility ----
            makeHeading(0, L"Visibility", LBL_X, y, CW); y += HEAD;
            m_savedShowStrip = m_origShowStrip = strip_load_show_strip();
            m_showStrip = CreateWindowExW(0, L"BUTTON", L"Show strip",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                LBL_X, y, dscale(240), dscale(22), m_content, (HMENU)(INT_PTR)ID_SHOW_STRIP, inst, nullptr);
            SendMessageW(m_showStrip, BM_SETCHECK, m_savedShowStrip ? BST_CHECKED : BST_UNCHECKED, 0);
            if (m_bodyFont) SendMessageW(m_showStrip, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
            track(0, m_showStrip); y += SU(26);

            m_savedShowVol = m_origShowVol = strip_load_show_volume();
            m_showVol = CreateWindowExW(0, L"BUTTON", L"Show volume control",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                LBL_X, y, dscale(180), dscale(22), m_content, (HMENU)(INT_PTR)ID_SHOW_VOLUME, inst, nullptr);
            SendMessageW(m_showVol, BM_SETCHECK, m_savedShowVol ? BST_CHECKED : BST_UNCHECKED, 0);
            if (m_bodyFont) SendMessageW(m_showVol, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
            track(0, m_showVol);

            // Show album-art popup - shares the row with the volume toggle.
            m_savedShowPopup = m_origShowPopup = strip_load_show_popup();
            m_showPopup = CreateWindowExW(0, L"BUTTON", L"Show album art popup",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                LBL_X + dscale(190), y, dscale(190), dscale(22), m_content,
                (HMENU)(INT_PTR)ID_SHOW_POPUP, inst, nullptr);
            SendMessageW(m_showPopup, BM_SETCHECK, m_savedShowPopup ? BST_CHECKED : BST_UNCHECKED, 0);
            if (m_bodyFont) SendMessageW(m_showPopup, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
            track(0, m_showPopup); y += SU(30);

            // ---- Theme ----
            makeHeading(0, L"Theme", LBL_X, y, CW); y += HEAD;
            m_savedMode = strip_load_theme_mode(); m_origMode = m_savedMode;
            for (int i = 0; i < kNumColors; i++)
                m_curCol[i] = m_savedCol[i] = m_origCol[i] = strip_load_color(i);
            track(0, makeLabel(L"Mode:", LBL_X, lblY(y)));
            m_theme = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                CTL_X, y, dscale(170), dscale(200), m_content, (HMENU)(INT_PTR)ID_THEME_MODE, inst, nullptr);
            if (m_bodyFont) SendMessageW(m_theme, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
            track(0, m_theme);
            const wchar_t* modes[] = { L"Light", L"Dark", L"Follow system", L"Custom" };
            for (auto m : modes) SendMessageW(m_theme, CB_ADDSTRING, 0, (LPARAM)m);
            SendMessageW(m_theme, CB_SETCURSEL, m_savedMode, 0); y += SU(30);

            const wchar_t* colNames[kNumColors] = {
                L"Background:", L"Text:", L"Buttons:", L"Bar fill:", L"Bar track:",
                L"Popup padding:" };
            for (int i = 0; i < kNumColors; i++) {
                track(0, makeLabel(colNames[i], LBL_X, lblY(y)));
                m_colBtn[i] = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    CTL_X, y, dscale(60), dscale(22), m_content, (HMENU)(INT_PTR)(ID_COLOR_BASE + i), inst, nullptr);
                track(0, m_colBtn[i]); y += SU(28);
            }
            y += SU(4);

            // ---- Transparency ----
            makeHeading(0, L"Transparency", LBL_X, y, CW); y += HEAD;
            m_savedPA = strip_load_popup_alpha(); m_origPA = m_savedPA;
            track(0, makeLabel(L"Popup opacity:", LBL_X, lblY(y)));
            m_sliderPA = makeSlider(ID_SLIDER_PA, CTL_X, y, PA_MIN, PA_MAX, m_savedPA); track(0, m_sliderPA);
            m_editPA   = makeEdit(ID_EDIT_PA, EDIT_X, y, m_savedPA, PA_MIN, PA_MAX); track(0, m_editPA); track(0, m_lastSpin);
            y += ROW;
            m_savedBA = strip_load_bg_alpha(); m_origBA = m_savedBA;
            track(0, makeLabel(L"Strip opacity:", LBL_X, lblY(y)));
            m_sliderBA = makeSlider(ID_SLIDER_BA, CTL_X, y, PA_MIN, PA_MAX, m_savedBA); track(0, m_sliderBA);
            m_editBA   = makeEdit(ID_EDIT_BA, EDIT_X, y, m_savedBA, PA_MIN, PA_MAX); track(0, m_editBA); track(0, m_lastSpin);
            updateColorEnabled();
            m_tabContentH[0] = y + ROW;   // total height for scroll range
        }

        // ================= SIZE tab (group 1) =================
        {
            int y = y0;
            m_savedW = strip_load_width();   m_origW = m_savedW;
            m_savedH = strip_load_height();  m_origH = m_savedH;
            m_savedP = strip_load_popup_size(); m_origP = m_savedP;

            // ---- Dimensions ----
            makeHeading(1, L"Dimensions", LBL_X, y, CW); y += HEAD;
            track(1, makeLabel(L"Width (px):", LBL_X, lblY(y)));
            m_sliderW = makeSlider(ID_SLIDER_W, CTL_X, y, W_MIN, W_MAX, m_savedW); track(1, m_sliderW);
            m_editW   = makeEdit(ID_EDIT_W, EDIT_X, y, m_savedW, W_MIN, W_MAX); track(1, m_editW); track(1, m_lastSpin);
            y += ROW;
            track(1, makeLabel(L"Height (px):", LBL_X, lblY(y)));
            m_sliderH = makeSlider(ID_SLIDER_H, CTL_X, y, H_MIN, H_MAX, m_savedH); track(1, m_sliderH);
            m_editH   = makeEdit(ID_EDIT_H, EDIT_X, y, m_savedH, H_MIN, H_MAX); track(1, m_editH); track(1, m_lastSpin);
            y += ROW;
            track(1, makeLabel(L"Art popup (px):", LBL_X, lblY(y)));
            m_sliderP = makeSlider(ID_SLIDER_P, CTL_X, y, P_MIN, P_MAX, m_savedP); track(1, m_sliderP);
            m_editP   = makeEdit(ID_EDIT_P, EDIT_X, y, m_savedP, P_MIN, P_MAX); track(1, m_editP); track(1, m_lastSpin);
            y += ROW + SU(6);

            // ---- Icons & spacing ----
            makeHeading(1, L"Icons & spacing", LBL_X, y, CW); y += HEAD;
            m_savedIcon[0] = m_origIcon[0] = strip_load_icon_size(0);
            m_savedIcon[1] = m_origIcon[1] = strip_load_icon_size(1);
            m_savedSpace[0] = m_origSpace[0] = strip_load_spacing(0);
            m_savedSpace[1] = m_origSpace[1] = strip_load_spacing(1);
            track(1, makeLabel(L"Button icons:", LBL_X, lblY(y)));
            m_sliderIT = makeSlider(ID_SLIDER_IT, CTL_X, y, IC_MIN, IC_MAX, m_savedIcon[0]); track(1, m_sliderIT);
            m_editIT   = makeEdit(ID_EDIT_IT, EDIT_X, y, m_savedIcon[0], IC_MIN, IC_MAX); track(1, m_editIT); track(1, m_lastSpin);
            y += ROW;
            track(1, makeLabel(L"Button spacing:", LBL_X, lblY(y)));
            m_sliderSB = makeSlider(ID_SLIDER_SB, CTL_X, y, SP_MIN, SP_MAX, m_savedSpace[0]); track(1, m_sliderSB);
            m_editSB   = makeEdit(ID_EDIT_SB, EDIT_X, y, m_savedSpace[0], SP_MIN, SP_MAX); track(1, m_editSB); track(1, m_lastSpin);
            y += ROW;
            track(1, makeLabel(L"Volume icon:", LBL_X, lblY(y)));
            m_sliderIS = makeSlider(ID_SLIDER_IS, CTL_X, y, IC_MIN, IC_MAX, m_savedIcon[1]); track(1, m_sliderIS);
            m_editIS   = makeEdit(ID_EDIT_IS, EDIT_X, y, m_savedIcon[1], IC_MIN, IC_MAX); track(1, m_editIS); track(1, m_lastSpin);
            y += ROW;
            track(1, makeLabel(L"Volume gap:", LBL_X, lblY(y)));
            m_sliderSV = makeSlider(ID_SLIDER_SV, CTL_X, y, SP_MIN, SP_MAX, m_savedSpace[1]); track(1, m_sliderSV);
            m_editSV   = makeEdit(ID_EDIT_SV, EDIT_X, y, m_savedSpace[1], SP_MIN, SP_MAX); track(1, m_editSV); track(1, m_lastSpin);
            m_tabContentH[1] = y + ROW;
        }

        // ================= TEXT tab (group 2) =================
        {
            int y = y0;
            for (int k = 0; k < kNumFonts; k++)
                m_curFont[k] = m_savedFont[k] = m_origFont[k] = strip_load_font_size(k);
            strip_load_font_face(m_savedFace);
            m_curFace = m_savedFace; m_origFace = m_savedFace;

            makeHeading(2, L"Font", LBL_X, y, CW); y += HEAD;
            track(2, makeLabel(L"Family:", LBL_X, lblY(y)));
            m_face = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL,
                CTL_X, y, dscale(200), dscale(300), m_content, (HMENU)(INT_PTR)ID_FONT_FACE, inst, nullptr);
            if (m_bodyFont) SendMessageW(m_face, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
            track(2, m_face);
            populateFontList();
            y += ROW;

            const wchar_t* fontLabels[kNumFonts] = { L"Title size:", L"Artist size:", L"Time size:" };
            for (int k = 0; k < kNumFonts; k++) {
                track(2, makeLabel(fontLabels[k], LBL_X, lblY(y)));
                m_fslider[k] = makeSlider(ID_FSLIDER_BASE + k, CTL_X, y, F_MIN, F_MAX, m_savedFont[k]); track(2, m_fslider[k]);
                m_fedit[k]   = makeEdit(ID_FEDIT_BASE + k, EDIT_X, y, m_savedFont[k], F_MIN, F_MAX); track(2, m_fedit[k]); track(2, m_lastSpin);
                y += ROW;
            }
            m_tabContentH[2] = y + ROW;
        }

        showTab(0);   // start on the General tab

        // Apply foobar's dark-mode theming. The hook auto-follows foobar's config
        // (createAuto) and updates live. We theme each piece deliberately rather
        // than walking m_hwnd's children, because a blanket AddControls(m_hwnd)
        // would classify our content PANEL (a STATIC window) as a static-text
        // control and subclass it as one - fighting our use of it as a dialog-like
        // container. Instead:
        //   * the tab control: AddCtrlAuto routes by window class to the tab hook;
        //   * the panel: AddDialog installs the dialog hook (themes its WM_CTLCOLOR*
        //     so the child controls get dark backgrounds), and AddControls themes
        //     the body controls that live on it.
        m_dark.AddCtrlAuto(m_tabs);
        m_dark.AddDialog(m_content);
        m_dark.AddControls(m_content);
        m_building = false;   // construction done - handle control events now
    }

    ~strip_prefs_instance() {
        // If the user live-previewed but did NOT apply (Cancel / navigate away),
        // restore the originals so the strip reverts. If they applied, the orig*
        // values were updated to the committed ones, so this is a no-op.
        bool colorsChanged = false;
        for (int i = 0; i < kNumColors; i++)
            if (strip_load_color(i) != m_origCol[i]) colorsChanged = true;
        bool fontsChanged = false;
        for (int k = 0; k < kNumFonts; k++)
            if (strip_load_font_size(k) != m_origFont[k]) fontsChanged = true;
        pfc::string8 curFace; strip_load_font_face(curFace);
        if (strcmp(curFace.get_ptr(), m_origFace.get_ptr()) != 0) fontsChanged = true;
        if (strip_load_icon_size(0) != m_origIcon[0] || strip_load_icon_size(1) != m_origIcon[1]) fontsChanged = true;
        if (strip_load_show_volume() != m_origShowVol) fontsChanged = true;
        bool stripVisChanged = (strip_load_show_strip() != m_origShowStrip);
        if (strip_load_spacing(0) != m_origSpace[0] || strip_load_spacing(1) != m_origSpace[1]) fontsChanged = true;
        if (strip_load_width()  != m_origW || strip_load_height() != m_origH ||
            strip_load_popup_size() != m_origP || strip_load_popup_alpha() != m_origPA ||
            strip_load_bg_alpha() != m_origBA ||
            strip_load_theme_mode() != m_origMode || colorsChanged || fontsChanged) {
            strip_save_width(m_origW);
            strip_save_height(m_origH);
            strip_save_popup_size(m_origP);
            strip_save_popup_alpha(m_origPA);
            strip_save_bg_alpha(m_origBA);
            strip_save_theme_mode(m_origMode);
            for (int i = 0; i < kNumColors; i++) strip_save_color(i, m_origCol[i]);
            for (int k = 0; k < kNumFonts; k++) strip_save_font_size(k, m_origFont[k]);
            strip_save_font_face(m_origFace.get_ptr());
            strip_save_icon_size(0, m_origIcon[0]);
            strip_save_icon_size(1, m_origIcon[1]);
            strip_save_show_volume(m_origShowVol);
            strip_save_show_popup(m_origShowPopup);
            strip_save_spacing(0, m_origSpace[0]);
            strip_save_spacing(1, m_origSpace[1]);
            strip_apply_settings();
        }
        if (stripVisChanged) {
            strip_save_show_strip(m_origShowStrip);
            strip_apply_visibility();
        }
        if (m_boldFont) DeleteObject(m_boldFont);
        if (m_bodyFont && m_ownBodyFont) DeleteObject(m_bodyFont);
        if (m_hwnd) DestroyWindow(m_hwnd);
    }

    // --- preferences_page_instance interface ---
    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable
                       | preferences_state::dark_mode_supported;
        int mode = (int)SendMessageW(m_theme, CB_GETCURSEL, 0, 0);
        bool colorsChanged = false;
        for (int i = 0; i < kNumColors; i++)
            if (m_curCol[i] != m_savedCol[i]) colorsChanged = true;
        bool fontsChanged = false;
        for (int k = 0; k < kNumFonts; k++)
            if (readEdit(ID_FEDIT_BASE + k) != m_savedFont[k]) fontsChanged = true;
        if (strcmp(m_curFace.get_ptr(), m_savedFace.get_ptr()) != 0) fontsChanged = true;
        if (readEdit(ID_EDIT_IT) != m_savedIcon[0] || readEdit(ID_EDIT_IS) != m_savedIcon[1]) fontsChanged = true;
        if ((SendMessageW(m_showVol, BM_GETCHECK, 0, 0) == BST_CHECKED) != m_savedShowVol) fontsChanged = true;
        if ((SendMessageW(m_showPopup, BM_GETCHECK, 0, 0) == BST_CHECKED) != m_savedShowPopup) fontsChanged = true;
        if ((SendMessageW(m_showStrip, BM_GETCHECK, 0, 0) == BST_CHECKED) != m_savedShowStrip) fontsChanged = true;
        if (readEdit(ID_EDIT_SB) != m_savedSpace[0] || readEdit(ID_EDIT_SV) != m_savedSpace[1]) fontsChanged = true;
        if (readEdit(ID_EDIT_W) != m_savedW || readEdit(ID_EDIT_H) != m_savedH ||
            readEdit(ID_EDIT_P) != m_savedP || readEdit(ID_EDIT_PA) != m_savedPA ||
            readEdit(ID_EDIT_BA) != m_savedBA ||
            mode != m_savedMode || colorsChanged || fontsChanged)
            state |= preferences_state::changed;
        return state;
    }

    fb2k::hwnd_t get_wnd() override { return (fb2k::hwnd_t)m_hwnd; }

    void apply() override {
        strip_save_width(readEdit(ID_EDIT_W));     // clamps + persists
        strip_save_height(readEdit(ID_EDIT_H));
        strip_save_popup_size(readEdit(ID_EDIT_P));
        strip_save_popup_alpha(readEdit(ID_EDIT_PA));
        strip_save_bg_alpha(readEdit(ID_EDIT_BA));
        int mode = (int)SendMessageW(m_theme, CB_GETCURSEL, 0, 0);
        if (mode >= 0) strip_save_theme_mode(mode);
        for (int i = 0; i < kNumColors; i++) strip_save_color(i, m_curCol[i]);
        for (int k = 0; k < kNumFonts; k++) strip_save_font_size(k, readEdit(ID_FEDIT_BASE + k));
        readFace(m_curFace);
        strip_save_font_face(m_curFace.get_ptr());
        strip_save_icon_size(0, readEdit(ID_EDIT_IT));
        strip_save_icon_size(1, readEdit(ID_EDIT_IS));
        bool sv = SendMessageW(m_showVol, BM_GETCHECK, 0, 0) == BST_CHECKED;
        strip_save_show_volume(sv);
        bool sp = SendMessageW(m_showPopup, BM_GETCHECK, 0, 0) == BST_CHECKED;
        strip_save_show_popup(sp);
        bool ss = SendMessageW(m_showStrip, BM_GETCHECK, 0, 0) == BST_CHECKED;
        strip_save_show_strip(ss);
        strip_save_spacing(0, readEdit(ID_EDIT_SB));
        strip_save_spacing(1, readEdit(ID_EDIT_SV));
        strip_apply_settings();
        strip_apply_visibility();
        // Re-read clamped values back and update the committed baseline.
        m_savedW = strip_load_width();
        m_savedH = strip_load_height();
        m_savedP = strip_load_popup_size();
        m_savedPA = strip_load_popup_alpha();
        m_savedBA = strip_load_bg_alpha();
        m_savedMode = strip_load_theme_mode();
        for (int i = 0; i < kNumColors; i++)
            m_savedCol[i] = m_origCol[i] = m_curCol[i];
        for (int k = 0; k < kNumFonts; k++) {
            m_savedFont[k] = m_origFont[k] = strip_load_font_size(k);
            setBoth(ID_FSLIDER_BASE + k, ID_FEDIT_BASE + k, m_savedFont[k]);
        }
        m_savedFace = m_origFace = m_curFace;
        m_savedIcon[0] = m_origIcon[0] = strip_load_icon_size(0);
        m_savedIcon[1] = m_origIcon[1] = strip_load_icon_size(1);
        setBoth(ID_SLIDER_IT, ID_EDIT_IT, m_savedIcon[0]);
        setBoth(ID_SLIDER_IS, ID_EDIT_IS, m_savedIcon[1]);
        m_savedShowVol = m_origShowVol = strip_load_show_volume();
        m_savedShowPopup = m_origShowPopup = strip_load_show_popup();
        m_savedShowStrip = m_origShowStrip = strip_load_show_strip();
        m_savedSpace[0] = m_origSpace[0] = strip_load_spacing(0);
        m_savedSpace[1] = m_origSpace[1] = strip_load_spacing(1);
        setBoth(ID_SLIDER_SB, ID_EDIT_SB, m_savedSpace[0]);
        setBoth(ID_SLIDER_SV, ID_EDIT_SV, m_savedSpace[1]);
        m_origW = m_savedW; m_origH = m_savedH; m_origP = m_savedP; m_origPA = m_savedPA;
        m_origBA = m_savedBA;
        m_origMode = m_savedMode;
        setBoth(ID_SLIDER_W, ID_EDIT_W, m_savedW);
        setBoth(ID_SLIDER_H, ID_EDIT_H, m_savedH);
        setBoth(ID_SLIDER_P, ID_EDIT_P, m_savedP);
        setBoth(ID_SLIDER_PA, ID_EDIT_PA, m_savedPA);
        setBoth(ID_SLIDER_BA, ID_EDIT_BA, m_savedBA);
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

    void reset() override {
        setBoth(ID_SLIDER_W, ID_EDIT_W, 300);
        setBoth(ID_SLIDER_H, ID_EDIT_H, 46);
        setBoth(ID_SLIDER_P, ID_EDIT_P, 300);
        setBoth(ID_SLIDER_PA, ID_EDIT_PA, 255);
        setBoth(ID_SLIDER_BA, ID_EDIT_BA, 255);
        SendMessageW(m_theme, CB_SETCURSEL, 1, 0);   // Dark
        // Default custom colors (match the cfg_int defaults in foo_strip.cpp):
        // bg, text, buttons, fill, track.
        static const int defs[kNumColors] = {
            (24 << 16)  | (26 << 8)  | 31,
            (242 << 16) | (244 << 8) | 248,
            (212 << 16) | (218 << 8) | 227,
            (91 << 16)  | (167 << 8) | 245,
            (60 << 16)  | (64 << 8)  | 72,
            (18 << 16)  | (20 << 8)  | 24,
        };
        for (int i = 0; i < kNumColors; i++) {
            m_curCol[i] = defs[i];
            InvalidateRect(m_colBtn[i], nullptr, TRUE);
        }
        // Font defaults: title 13, artist 10, time 12; face Segoe UI.
        static const int fdefs[kNumFonts] = { 13, 10, 12 };
        for (int k = 0; k < kNumFonts; k++)
            setBoth(ID_FSLIDER_BASE + k, ID_FEDIT_BASE + k, fdefs[k]);
        setBoth(ID_SLIDER_IT, ID_EDIT_IT, 20);
        setBoth(ID_SLIDER_IS, ID_EDIT_IS, 14);
        SendMessageW(m_showVol, BM_SETCHECK, BST_CHECKED, 0);   // default: shown
        SendMessageW(m_showStrip, BM_SETCHECK, BST_CHECKED, 0); // default: strip shown
        strip_save_show_strip(true);
        strip_apply_visibility();
        setBoth(ID_SLIDER_SB, ID_EDIT_SB, 0);                   // default: flush
        setBoth(ID_SLIDER_SV, ID_EDIT_SV, 4);                   // default: 4px gap
        {
            int idx = (int)SendMessageW(m_face, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)L"Segoe UI");
            if (idx != CB_ERR) SendMessageW(m_face, CB_SETCURSEL, idx, 0);
            readFace(m_curFace);
        }
        updateColorEnabled();
        livePreview();
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

private:
    // --- tab helpers ---
    void track(int tab, HWND ctrl) { if (tab >= 0 && tab < 3 && ctrl) m_tabCtrls[tab].push_back(ctrl); }
    void showTab(int sel) {
        // First scroll back to top (so a switch never leaves controls offset),
        // then show the selected group and hide the rest, then size the bar.
        scrollTo(0);
        m_curTab = sel;
        for (int t = 0; t < 3; t++)
            for (HWND c : m_tabCtrls[t])
                ShowWindow(c, t == sel ? SW_SHOW : SW_HIDE);
        configScrollbar();
    }
    // Visible content height = the panel's client height. The tab row is OUTSIDE
    // the panel (the panel sits in the tab's display rect), so the entire client
    // area is scrollable content - no tab-strip offset to subtract.
    int viewHeight() {
        RECT rc; GetClientRect(m_content, &rc);
        int h = rc.bottom;
        return h > 0 ? h : 0;
    }
    // Set the scrollbar range/page for the current tab (or hide it if it fits).
    void configScrollbar() {
        int content = (m_curTab >= 0 && m_curTab < 3) ? m_tabContentH[m_curTab] : 0;
        int view = viewHeight();
        SCROLLINFO si{ sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = content > 0 ? content : 0;
        si.nPage = view > 0 ? view : 1;
        si.nPos = m_scrollY;
        SetScrollInfo(m_content, SB_VERT, &si, TRUE);
        // Clamp position if the content now fits.
        int maxPos = content - view; if (maxPos < 0) maxPos = 0;
        if (m_scrollY > maxPos) scrollTo(maxPos);
    }
    // Scroll so the top of the content sits at -pos; moves all child controls.
    void scrollTo(int pos) {
        if (pos < 0) pos = 0;
        int dy = m_scrollY - pos;     // positive dy scrolls content down
        if (dy == 0) { m_scrollY = pos; return; }
        m_scrollY = pos;
        // The entire panel client area scrolls (its children are the body rows).
        ScrollWindowEx(m_content, 0, dy, nullptr, nullptr, nullptr, nullptr,
                       SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        SetScrollPos(m_content, SB_VERT, m_scrollY, TRUE);
        UpdateWindow(m_content);
    }
    // Handle a WM_VSCROLL action code from the scrollbar.
    void onVScroll(int code) {
        int view = viewHeight();
        int content = (m_curTab >= 0 && m_curTab < 3) ? m_tabContentH[m_curTab] : 0;
        int maxPos = content - view; if (maxPos < 0) maxPos = 0;
        int pos = m_scrollY;
        switch (code) {
            case SB_LINEUP:   pos -= 24; break;
            case SB_LINEDOWN: pos += 24; break;
            case SB_PAGEUP:   pos -= view; break;
            case SB_PAGEDOWN: pos += view; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{ sizeof(si) }; si.fMask = SIF_TRACKPOS;
                GetScrollInfo(m_hwnd, SB_VERT, &si);
                pos = si.nTrackPos; break;
            }
            case SB_TOP:    pos = 0; break;
            case SB_BOTTOM: pos = maxPos; break;
        }
        if (pos < 0) pos = 0; if (pos > maxPos) pos = maxPos;
        scrollTo(pos);
    }

    // --- small control-creation helpers ---
    // Scale a design-pixel size by the font-derived UI scale, so control sizes
    // track the inherited (DPI-scaled) font and stay consistent with positions.
    int dscale(int n) { return (int)(n * m_uiScale + 0.5); }
    // Is the page currently themed dark? The fb2k::CCoreDarkModeHooks wrapper
    // exposes its dark state through operator bool() (-> coreDarkModeObj::isDark),
    // which createAuto() keeps in sync with foobar's config. So we just read the
    // hook directly - no libPPUI linkage, no DC probing.
    bool panel_is_dark() { return (bool)m_dark; }
    // Size the tab control to fill the host, then place the content panel at the
    // tab's interior DISPLAY rectangle (TCM_ADJUSTRECT maps the full tab rect to
    // the region inside its frame). This is what puts the body controls INSIDE
    // the tab's raised frame instead of floating beside the tab row.
    void layoutTabs() {
        if (!m_tabs || !m_content) return;
        RECT host; GetClientRect(m_hwnd, &host);
        // A small outer margin so the frame doesn't jam against the page edges.
        int m = dscale(6);
        RECT tabRc = { m, m, host.right - m, host.bottom - m };
        if (tabRc.right <= tabRc.left || tabRc.bottom <= tabRc.top) return;
        SetWindowPos(m_tabs, nullptr, tabRc.left, tabRc.top,
                     tabRc.right - tabRc.left, tabRc.bottom - tabRc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // Ask the tab control for the display area inside its frame, in tab-client
        // coords. TCM_ADJUSTRECT with FALSE converts a window rect -> display rect.
        RECT disp = { 0, 0, tabRc.right - tabRc.left, tabRc.bottom - tabRc.top };
        SendMessageW(m_tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&disp);
        SetWindowPos(m_content, nullptr, disp.left, disp.top,
                     disp.right - disp.left, disp.bottom - disp.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    HWND makeLabel(const wchar_t* text, int x, int y) {
        HWND l = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
            x, y, dscale(110), dscale(20), m_content, nullptr, core_api::get_my_instance(), nullptr);
        if (m_bodyFont) SendMessageW(l, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
        return l;
    }
    // Section heading: a bold, slightly larger caption. (No separator line - a
    // stretched etched static renders as a box at scaled DPI.)
    void makeHeading(int tab, const wchar_t* text, int x, int y, int width) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
            x, y, width, dscale(22), m_content, nullptr, core_api::get_my_instance(), nullptr);
        if (!m_boldFont) {
            // Derive a larger, bold version of the dialog font once, so headings
            // read as headings (bigger + bolder than the body rows).
            HFONT base = (HFONT)SendMessageW(m_hwnd, WM_GETFONT, 0, 0);
            LOGFONTW lf{};
            if (base) GetObjectW(base, sizeof(lf), &lf);
            else { lf.lfHeight = -12; lstrcpyW(lf.lfFaceName, L"Segoe UI"); }
            // Enlarge ~25%. lfHeight is negative (character height), so multiply
            // its magnitude.
            if (lf.lfHeight < 0) lf.lfHeight = (LONG)(lf.lfHeight * 1.25);
            else if (lf.lfHeight > 0) lf.lfHeight = (LONG)(lf.lfHeight * 1.25);
            else lf.lfHeight = -15;
            lf.lfWeight = FW_BOLD;
            m_boldFont = CreateFontIndirectW(&lf);
        }
        SendMessageW(h, WM_SETFONT, (WPARAM)m_boldFont, TRUE);
        track(tab, h);
        // Thin separator under the caption. Height is hard-locked to 2px (never
        // scaled): an SS_ETCHEDHORZ static taller than ~2px renders as a sunken
        // box instead of a line, which is what bloated at high DPI before.
        HWND line = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            x, y + dscale(20), width, 2, m_content, nullptr, core_api::get_my_instance(), nullptr);
        track(tab, line);
    }
    HWND makeSlider(int id, int x, int y, int lo, int hi, int val) {
        HWND s = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            x, y, dscale(160), dscale(28), m_content, (HMENU)(INT_PTR)id,
            core_api::get_my_instance(), nullptr);
        SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
        SendMessageW(s, TBM_SETPOS, TRUE, val);
        return s;
    }
    HWND makeEdit(int id, int x, int y, int val, int lo, int hi) {
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_NUMBER,
            x, y, dscale(60), dscale(24), m_content, (HMENU)(INT_PTR)id,
            core_api::get_my_instance(), nullptr);
        SetDlgItemInt(m_content, id, (UINT)val, FALSE);
        if (m_bodyFont) SendMessageW(e, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
        // Buddied up-down spinner: UDS_AUTOBUDDY attaches to the edit just
        // created, UDS_SETBUDDYINT makes the arrows write the integer into it,
        // UDS_ALIGNRIGHT docks the arrows on the edit's right edge. Editing via
        // the arrows fires EN_CHANGE on the edit, which our handler already
        // catches (sync slider + live-preview), so no extra wiring is needed.
        HWND ud = CreateWindowExW(0, UPDOWN_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT
                     | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
            0, 0, 0, 0, m_content, (HMENU)(INT_PTR)(id + 100),
            core_api::get_my_instance(), nullptr);
        SendMessageW(ud, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
        SendMessageW(ud, UDM_SETPOS32, 0, (LPARAM)val);
        m_lastSpin = ud;   // so the caller can track() it into the right tab
        return e;
    }

    // Enumerate installed font families into the face combobox, then select the
    // saved face. Uses EnumFontFamiliesExW; dedupes (it can report a family once
    // per charset) by skipping if already present.
    static int CALLBACK enumFontProc(const LOGFONTW* lf, const TEXTMETRICW*,
                                     DWORD, LPARAM lp) {
        HWND combo = (HWND)lp;
        const wchar_t* name = lf->lfFaceName;
        if (name[0] == L'@') return 1;   // skip vertical @-fonts
        if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name) == CB_ERR)
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
        return 1;
    }
    void populateFontList() {
        HDC hdc = GetDC(m_hwnd);
        LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(hdc, &lf, enumFontProc, (LPARAM)m_face, 0);
        ReleaseDC(m_hwnd, hdc);
        // Select the saved face (convert UTF-8 -> wide).
        wchar_t faceW[128];
        MultiByteToWideChar(CP_UTF8, 0, m_savedFace.get_ptr(), -1, faceW, 128);
        int idx = (int)SendMessageW(m_face, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)faceW);
        if (idx != CB_ERR) SendMessageW(m_face, CB_SETCURSEL, idx, 0);
    }
    // Read the selected font face into a UTF-8 string.
    void readFace(pfc::string_base& out) {
        int idx = (int)SendMessageW(m_face, CB_GETCURSEL, 0, 0);
        if (idx == CB_ERR) { out = "Segoe UI"; return; }
        wchar_t buf[128] = {};
        SendMessageW(m_face, CB_GETLBTEXT, idx, (LPARAM)buf);
        char utf8[256]; WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 256, nullptr, nullptr);
        out = utf8;
    }

    int readEdit(int id) {
        BOOL ok = FALSE;
        int v = (int)GetDlgItemInt(m_content, id, &ok, FALSE);
        return ok ? v : 0;
    }
    // Set both the slider and edit for a value without re-triggering each other.
    void setBoth(int sliderId, int editId, int val) {
        m_syncing = true;
        SendMessageW(GetDlgItem(m_content, sliderId), TBM_SETPOS, TRUE, val);
        SetDlgItemInt(m_content, editId, (UINT)val, FALSE);
        m_syncing = false;
    }

    // Shared proc for BOTH the outer host (m_hwnd) and the inner content panel
    // (m_content). They carry the same 'this' in USERDATA. We dispatch on which
    // window the message is for, then chain to that window's original wndproc.
    // NOTE on subclass order for m_content: we install host_proc first (saving the
    // STATIC default as m_prevContentProc); AddDialog() then stacks foobar's
    // CDialogHook ON TOP. So the chain is CDialogHook -> host_proc -> STATIC. The
    // hook answers WM_CTLCOLOR* for the child controls; host_proc only needs to
    // fill the panel's OWN background on WM_ERASEBKGND (the hook has no erase
    // handler), which it does using the hook's brush - see onMsg.
    static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto self = (strip_prefs_instance*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (self) {
            bool handled = false;
            LRESULT r = self->onMsg(hwnd, msg, wp, lp, handled);
            if (handled) return r;
        }
        WNDPROC prev = nullptr;
        if (self) prev = (hwnd == self->m_content) ? self->m_prevContentProc
                                                   : self->m_prevProc;
        if (prev) return CallWindowProcW(prev, hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    // Returns a result only when 'handled' is set true; otherwise the caller
    // chains to the original wndproc.
    LRESULT onMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        // --- Messages for the OUTER host window (m_hwnd) ---
        if (hwnd == m_hwnd) {
            // Foobar resized the page: refit the tab control and the panel inside
            // it, then refresh the scrollbar for the new viewport height.
            if (msg == WM_SIZE) { layoutTabs(); configScrollbar(); return 0; }
            // Paint the thin margin around the tab control in the themed color so
            // the host STATIC's default light fill doesn't peek out at the edges.
            // (Same brush source as the panel: the panel's own dark hook, with a
            // system-face fallback for light mode.)
            if (msg == WM_ERASEBKGND) {
                HDC hdc = (HDC)wp;
                RECT rc; GetClientRect(m_hwnd, &rc);
                HBRUSH br = nullptr;
                if (panel_is_dark()) {
                    br = (HBRUSH)SendMessageW(m_content, WM_CTLCOLORSTATIC,
                                             (WPARAM)hdc, (LPARAM)m_content);
                }
                if (!br) br = GetSysColorBrush(COLOR_WINDOW);
                FillRect(hdc, &rc, br);
                handled = true;
                return 1;
            }
            // The tab control is a child of m_hwnd, so its selection-change
            // notification arrives here (not at the panel).
            if (msg == WM_NOTIFY && !m_building) {
                NMHDR* nm = (NMHDR*)lp;
                if (nm->idFrom == ID_TABS && nm->code == TCN_SELCHANGE) {
                    int sel = (int)SendMessageW(m_tabs, TCM_GETCURSEL, 0, 0);
                    showTab(sel);
                }
            }
            return 0;
        }
        // --- Messages for the inner content panel (m_content) ---
        // Child control colors in LIGHT mode. In dark mode the stacked CDialogHook
        // (top subclass) intercepts WM_CTLCOLOR* before this proc and themes the
        // controls; we never see them here. But in light mode the hook is inactive
        // and passes them through to us - and if we let them fall to DefWindowProc,
        // it returns the GREY COLOR_3DFACE brush, which paints grey boxes behind
        // every label/heading/checkbox against our now-white panel. So in light
        // mode we answer them ourselves: transparent text background + the white
        // COLOR_WINDOW brush, so the controls blend into the white surface.
        if ((msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) && !panel_is_dark()) {
            HDC hdc = (HDC)wp;
            SetBkMode(hdc, TRANSPARENT);
            handled = true;
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        // Panel backdrop: foobar's dark-mode hook (CDialogHook, stacked on top of
        // this proc by AddDialog) answers WM_CTLCOLOR* for the child controls, so
        // labels/checkboxes/edits get the right background. But CDialogHook has NO
        // WM_ERASEBKGND handler, and our panel class has a NULL background brush,
        // so we must fill the panel's own surface here. Ask the panel's own hook
        // for the themed brush (dark window color in dark mode); when it returns
        // NULL - light mode, or pre-v2 foobar with no hook - fall back to
        // COLOR_WINDOW, which is white, matching foobar's light prefs pages.
        if (msg == WM_ERASEBKGND) {
            HDC hdc = (HDC)wp;
            RECT rc; GetClientRect(m_content, &rc);
            // Choose the brush by the hook's dark state. We must NOT rely on the
            // hook returning NULL in light mode: when dark is off, CDialogHook does
            // SetMsgHandled(FALSE), so the WM_CTLCOLORSTATIC we send falls through
            // to DefWindowProc, which hands back the GREY COLOR_3DFACE brush - that
            // grey is exactly what kept showing. So: dark -> use the hook's themed
            // brush; light -> fill COLOR_WINDOW (white), matching foobar's light
            // prefs pages.
            HBRUSH br = nullptr;
            if (panel_is_dark()) {
                br = (HBRUSH)SendMessageW(m_content, WM_CTLCOLORSTATIC,
                                         (WPARAM)hdc, (LPARAM)m_content);
            }
            if (!br) br = GetSysColorBrush(COLOR_WINDOW);
            FillRect(hdc, &rc, br);
            handled = true;
            return 1;
        }
        // Owner-draw the color swatch buttons (must run even during construction).
        if (msg == WM_DRAWITEM) {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            int id = (int)wp;
            if (id >= ID_COLOR_BASE && id < ID_COLOR_BASE + kNumColors) {
                int i = id - ID_COLOR_BASE;
                int rgb = m_curCol[i];
                HBRUSH br = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
                FillRect(dis->hDC, &dis->rcItem, br);
                DeleteObject(br);
                FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
                if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
                handled = true;
                return TRUE;
            }
        }
        // Scrolling works regardless of construction/sync state.
        if (msg == WM_SIZE) { configScrollbar(); return 0; }
        if (msg == WM_VSCROLL) { onVScroll(LOWORD(wp)); return 0; }
        if (msg == WM_MOUSEWHEEL) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int view = viewHeight();
            int content = (m_curTab >= 0 && m_curTab < 3) ? m_tabContentH[m_curTab] : 0;
            int maxPos = content - view; if (maxPos < 0) maxPos = 0;
            int pos = m_scrollY - (delta / WHEEL_DELTA) * 48;
            if (pos < 0) pos = 0; if (pos > maxPos) pos = maxPos;
            scrollTo(pos);
            return 0;
        }
        if (m_syncing || m_building) return 0;
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            // Edit boxes edited -> update slider, live-preview.
            if (code == EN_CHANGE) {
                if (id == ID_EDIT_W) { syncSliderFromEdit(ID_SLIDER_W, ID_EDIT_W); livePreview(); changed(); }
                else if (id == ID_EDIT_H) { syncSliderFromEdit(ID_SLIDER_H, ID_EDIT_H); livePreview(); changed(); }
                else if (id == ID_EDIT_P) { syncSliderFromEdit(ID_SLIDER_P, ID_EDIT_P); livePreview(); changed(); }
                else if (id == ID_EDIT_PA) { syncSliderFromEdit(ID_SLIDER_PA, ID_EDIT_PA); livePreview(); changed(); }
                else if (id == ID_EDIT_BA) { syncSliderFromEdit(ID_SLIDER_BA, ID_EDIT_BA); livePreview(); changed(); }
                else if (id >= ID_FEDIT_BASE && id < ID_FEDIT_BASE + kNumFonts) {
                    int k = id - ID_FEDIT_BASE;
                    syncSliderFromEdit(ID_FSLIDER_BASE + k, ID_FEDIT_BASE + k);
                    livePreview(); changed();
                }
                else if (id == ID_EDIT_IT) { syncSliderFromEdit(ID_SLIDER_IT, ID_EDIT_IT); livePreview(); changed(); }
                else if (id == ID_EDIT_IS) { syncSliderFromEdit(ID_SLIDER_IS, ID_EDIT_IS); livePreview(); changed(); }
                else if (id == ID_EDIT_SB) { syncSliderFromEdit(ID_SLIDER_SB, ID_EDIT_SB); livePreview(); changed(); }
                else if (id == ID_EDIT_SV) { syncSliderFromEdit(ID_SLIDER_SV, ID_EDIT_SV); livePreview(); changed(); }
            }
            // Theme mode changed -> enable/disable color controls, live-preview.
            else if (id == ID_THEME_MODE && code == CBN_SELCHANGE) {
                updateColorEnabled();
                livePreview();
                changed();
            }
            // Font face changed -> live-preview.
            else if (id == ID_FONT_FACE && code == CBN_SELCHANGE) {
                readFace(m_curFace);
                livePreview();
                changed();
            }
            // Show-volume checkbox toggled.
            else if (id == ID_SHOW_VOLUME && code == BN_CLICKED) {
                livePreview();
                changed();
            }
            // Show-album-art-popup toggled: save right away so the strip's hover
            // behaviour updates live. No repaint needed (popup is hover-driven).
            else if (id == ID_SHOW_POPUP && code == BN_CLICKED) {
                strip_save_show_popup(SendMessageW(m_showPopup, BM_GETCHECK, 0, 0) == BST_CHECKED);
                changed();
            }
            // Master show-strip toggled: persist and apply visibility right away
            // (live, like other settings). Hiding removes the window; re-showing
            // re-renders it. Cancel-restore handled in the destructor.
            else if (id == ID_SHOW_STRIP && code == BN_CLICKED) {
                bool on = SendMessageW(m_showStrip, BM_GETCHECK, 0, 0) == BST_CHECKED;
                strip_save_show_strip(on);
                strip_apply_visibility();
                changed();
            }
            // Color buttons -> open the picker for that color index.
            else if (id >= ID_COLOR_BASE && id < ID_COLOR_BASE + kNumColors
                     && code == BN_CLICKED) {
                pickColor(id - ID_COLOR_BASE);
            }
        }
        // Slider moved -> update edit, live-preview.
        else if (msg == WM_HSCROLL) {
            HWND from = (HWND)lp;
            if (from == GetDlgItem(m_content, ID_SLIDER_W)) { syncEditFromSlider(ID_SLIDER_W, ID_EDIT_W); livePreview(); changed(); }
            else if (from == GetDlgItem(m_content, ID_SLIDER_H)) { syncEditFromSlider(ID_SLIDER_H, ID_EDIT_H); livePreview(); changed(); }
            else if (from == GetDlgItem(m_content, ID_SLIDER_P)) { syncEditFromSlider(ID_SLIDER_P, ID_EDIT_P); livePreview(); changed(); }
            else if (from == GetDlgItem(m_content, ID_SLIDER_PA)) { syncEditFromSlider(ID_SLIDER_PA, ID_EDIT_PA); livePreview(); changed(); }
            else if (from == GetDlgItem(m_content, ID_SLIDER_BA)) { syncEditFromSlider(ID_SLIDER_BA, ID_EDIT_BA); livePreview(); changed(); }
            else {
                for (int k = 0; k < kNumFonts; k++)
                    if (from == GetDlgItem(m_content, ID_FSLIDER_BASE + k)) {
                        syncEditFromSlider(ID_FSLIDER_BASE + k, ID_FEDIT_BASE + k);
                        livePreview(); changed();
                    }
                if (from == GetDlgItem(m_content, ID_SLIDER_IT)) { syncEditFromSlider(ID_SLIDER_IT, ID_EDIT_IT); livePreview(); changed(); }
                else if (from == GetDlgItem(m_content, ID_SLIDER_IS)) { syncEditFromSlider(ID_SLIDER_IS, ID_EDIT_IS); livePreview(); changed(); }
                else if (from == GetDlgItem(m_content, ID_SLIDER_SB)) { syncEditFromSlider(ID_SLIDER_SB, ID_EDIT_SB); livePreview(); changed(); }
                else if (from == GetDlgItem(m_content, ID_SLIDER_SV)) { syncEditFromSlider(ID_SLIDER_SV, ID_EDIT_SV); livePreview(); changed(); }
            }
        }
        return 0;
    }
    // Enable all color buttons only in Custom mode (index 3).
    void updateColorEnabled() {
        int mode = (int)SendMessageW(m_theme, CB_GETCURSEL, 0, 0);
        for (int i = 0; i < kNumColors; i++)
            EnableWindow(m_colBtn[i], mode == 3);
    }
    // Open the Windows color picker for color index i; live-preview on accept.
    void pickColor(int i) {
        if (i < 0 || i >= kNumColors) return;
        static COLORREF customColors[16] = {};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = m_hwnd;
        cc.lpCustColors = customColors;
        int rgb = m_curCol[i];   // current working value, 0xRRGGBB
        // COLORREF is 0x00BBGGRR - swap R and B from our 0xRRGGBB.
        cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            int r = GetRValue(cc.rgbResult), g = GetGValue(cc.rgbResult), b = GetBValue(cc.rgbResult);
            m_curCol[i] = (r << 16) | (g << 8) | b;
            InvalidateRect(m_colBtn[i], nullptr, TRUE);  // repaint swatch
            livePreview();
            changed();
        }
    }
    // Push the CURRENT control values to the live strip immediately, so dragging
    // a slider resizes the strip in real time (like the Dolby/channel-mixer
    // components). These writes are not "committed" until Apply - but since they
    // go through the same cfg_var, we remember the originals (m_origW/H) in the
    // constructor and restore them if the user cancels without applying.
    void livePreview() {
        int w = readEdit(ID_EDIT_W), h = readEdit(ID_EDIT_H), p = readEdit(ID_EDIT_P);
        if (w > 0 && h > 0) {
            strip_save_width(w);
            strip_save_height(h);
        }
        if (p > 0) strip_save_popup_size(p);
        strip_save_popup_alpha(readEdit(ID_EDIT_PA));
        strip_save_bg_alpha(readEdit(ID_EDIT_BA));
        for (int k = 0; k < kNumFonts; k++) {
            int s = readEdit(ID_FEDIT_BASE + k);
            if (s > 0) strip_save_font_size(k, s);
        }
        strip_save_font_face(m_curFace.get_ptr());
        { int it = readEdit(ID_EDIT_IT), is = readEdit(ID_EDIT_IS);
          if (it > 0) strip_save_icon_size(0, it);
          if (is > 0) strip_save_icon_size(1, is); }
        strip_save_show_volume(SendMessageW(m_showVol, BM_GETCHECK, 0, 0) == BST_CHECKED);
        strip_save_show_popup(SendMessageW(m_showPopup, BM_GETCHECK, 0, 0) == BST_CHECKED);
        { int sb = readEdit(ID_EDIT_SB), sv = readEdit(ID_EDIT_SV);
          strip_save_spacing(0, sb); strip_save_spacing(1, sv); }
        int mode = (int)SendMessageW(m_theme, CB_GETCURSEL, 0, 0);
        if (mode >= 0) strip_save_theme_mode(mode);
        for (int i = 0; i < kNumColors; i++) strip_save_color(i, m_curCol[i]);
        strip_apply_settings();
    }
    void syncSliderFromEdit(int sliderId, int editId) {
        m_syncing = true;
        SendMessageW(GetDlgItem(m_content, sliderId), TBM_SETPOS, TRUE, readEdit(editId));
        m_syncing = false;
    }
    void syncEditFromSlider(int sliderId, int editId) {
        m_syncing = true;
        int pos = (int)SendMessageW(GetDlgItem(m_content, sliderId), TBM_GETPOS, 0, 0);
        SetDlgItemInt(m_content, editId, (UINT)pos, FALSE);
        m_syncing = false;
    }
    void changed() { if (m_callback.is_valid()) m_callback->on_state_changed(); }

    HWND m_hwnd = nullptr, m_sliderW = nullptr, m_editW = nullptr,
         m_sliderH = nullptr, m_editH = nullptr, m_theme = nullptr,
         m_sliderP = nullptr, m_editP = nullptr, m_face = nullptr,
         m_sliderPA = nullptr, m_editPA = nullptr, m_tabs = nullptr,
         m_sliderBA = nullptr, m_editBA = nullptr,
         m_sliderIT = nullptr, m_editIT = nullptr, m_sliderIS = nullptr, m_editIS = nullptr,
         m_showVol = nullptr, m_showPopup = nullptr,
         m_sliderSB = nullptr, m_editSB = nullptr, m_sliderSV = nullptr, m_editSV = nullptr,
         m_showStrip = nullptr;
    std::vector<HWND> m_tabCtrls[3];        // controls per tab (General/Size/Text)
    int m_tabContentH[3] = {0, 0, 0};       // total content height per tab (scroll)
    int m_curTab = 0;                        // currently shown tab
    int m_scrollY = 0;                       // current vertical scroll offset
    double m_uiScale = 1.0;                    // layout scale (DPI), 1.0 at 96 DPI
    HWND m_lastSpin = nullptr;              // last spinner created by makeEdit
    HFONT m_boldFont = nullptr;             // bold font for section headings
    HFONT m_bodyFont = nullptr;             // dialog font (borrowed from parent)
    bool m_ownBodyFont = false;             // true only if we CreateFont'd it
    HWND m_colBtn[kNumColors] = {};         // the color swatch buttons
    HWND m_fslider[kNumFonts] = {};         // font size sliders (title/artist/time)
    HWND m_fedit[kNumFonts] = {};           // font size edits
    WNDPROC m_prevProc = nullptr;
    HWND m_content = nullptr;              // scrollable panel inside the tab frame
    WNDPROC m_prevContentProc = nullptr;  // m_content's original wndproc
    int m_savedW = 300, m_savedH = 46;
    int m_origW = 300, m_origH = 46;   // values at page-open, for cancel-restore
    int m_savedP = 300, m_origP = 300;     // popup size baseline / original
    int m_savedPA = 255, m_origPA = 255;   // popup alpha baseline / original
    int m_savedBA = 255, m_origBA = 255;   // strip background alpha baseline / original
    int m_curFont[kNumFonts] = {};         // working font sizes
    int m_savedFont[kNumFonts] = {};       // committed baseline
    int m_origFont[kNumFonts] = {};        // page-open originals
    pfc::string8 m_curFace, m_savedFace, m_origFace;   // font family name
    int m_savedIcon[2] = {20, 14}, m_origIcon[2] = {20, 14};  // transport / speaker
    bool m_savedShowVol = true, m_origShowVol = true;        // volume visible?
    bool m_savedShowPopup = true, m_origShowPopup = true;    // art popup visible?
    bool m_savedShowStrip = true, m_origShowStrip = true;    // whole strip visible?
    int m_savedSpace[2] = {0, 4}, m_origSpace[2] = {0, 4};   // btn gap / volume gap
    int m_savedMode = 1, m_origMode = 1;   // theme mode baseline / original
    int m_curCol[kNumColors] = {};         // working color values being edited
    int m_savedCol[kNumColors] = {};       // committed baseline
    int m_origCol[kNumColors] = {};        // page-open originals (cancel-restore)
    bool m_syncing = false;
    bool m_building = false;   // true during constructor; suppresses live-preview
    fb2k::CCoreDarkModeHooks m_dark;   // themes our controls to match foobar
    preferences_page_callback::ptr m_callback;
};

// ----------------------------------------------------------------------------
// The page service itself. This is the part foobar discovers and lists in the
// Preferences tree. It answers name/guid/parent and builds an instance.
// ----------------------------------------------------------------------------
class strip_prefs_page : public preferences_page_v3 {
public:
    // Name shown in the Preferences tree.
    const char* get_name() override { return "Floating Strip"; }

    // Our unique id (matches the right-click target).
    GUID get_guid() override { return g_prefs_page_guid; }

    // Where we live in the tree. guid_display puts us under the "Display" branch;
    // guid_tools would put us under "Tools". (These constants come from
    // preferences_page.) Components-style pages commonly sit under Tools.
    GUID get_parent_guid() override { return preferences_page::guid_display; }

    // Build the live window when the user opens our page.
    preferences_page_instance::ptr instantiate(
        fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override
    {
        return new service_impl_t<strip_prefs_instance>(parent, callback);
    }
};

// Register the page (the factory is what foobar scans for at load time).
static preferences_page_factory_t<strip_prefs_page> g_strip_prefs_factory;

} // namespace

// ----------------------------------------------------------------------------
// Called from the strip's right-click handler to open Preferences at our page.
// After opening, we force foobar's main window forward: the strip is a
// non-activating tool window, so the click that opened Preferences doesn't carry
// foreground rights, and the dialog can open BEHIND other apps when foobar's
// main window is visible-but-not-foreground. The AttachThreadInput trick grants
// us temporary permission to call SetForegroundWindow.
// ----------------------------------------------------------------------------
void strip_open_preferences() {
    ui_control::get()->show_preferences(g_prefs_page_guid);

    HWND fb = core_api::get_main_window();
    if (!fb) return;

    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();

    if (fgThread && fgThread != myThread)
        AttachThreadInput(myThread, fgThread, TRUE);

    SetForegroundWindow(fb);
    SetWindowPos(fb, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (fgThread && fgThread != myThread)
        AttachThreadInput(myThread, fgThread, FALSE);
}
