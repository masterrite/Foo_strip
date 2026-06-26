// foo_strip.cpp
// A standalone floating playback strip for foobar2000, rendered with GDI+.
// Because this runs in-process, it reads position/length and seeks DIRECTLY
// through playback_control - no SMTC, no bridge, no second component.
//
// Architecture:
//   - initquit service: creates/destroys the floating top-level window in step
//     with foobar's lifetime (on_init after main window exists, on_quit before).
//   - play_callback_static: receives playback events on the MAIN THREAD and
//     mirrors them into a small shared StripState (guarded by a lock).
//   - StripWindow: its own top-level layered window with a ~60ms repaint timer
//     for a smooth scrubber; paints with GDI+; hit-tests buttons + seek bar.
//
// THREADING NOTE: All foobar SDK playback calls must happen on the main thread.
// play_callback already fires there. For the seek action (triggered from the
// window's own message loop, which IS the main thread for a window created
// during on_init on fb2k's UI thread), we are on the main thread as well, so we
// can call playback_control directly. We still keep state access lock-guarded.

#include "stdafx.h"
#include "strip_shared.h"
#include <gdiplus.h>
#include <mutex>
#include <string>
#include <climits>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace {

// ----------------------------------------------------------------------------
// Component declaration (shows in Preferences > Components)
// ----------------------------------------------------------------------------
DECLARE_COMPONENT_VERSION(
    "Floating Playback Strip",
    "1.3.0",
    "A draggable floating strip with album art, title, transport, and a working "
    "seek bar. Reads playback directly in-process.\n");

// The packaged component contains foo_strip.dll (32-bit, at the archive root)
// and x64\foo_strip.dll (64-bit, in the x64 subfolder) - both named the same,
// per foobar's packaging convention. So a single filename validates both.
VALIDATE_COMPONENT_FILENAME("foo_strip.dll");

// The single shared StripState instance (declared in strip_shared.h).
StripState g_state;

// Forward decl
class StripWindow;
StripWindow* g_window = nullptr;

// ----------------------------------------------------------------------------
// Helpers to pull current playback info from the core (main thread only).
// refresh_from_core has EXTERNAL linkage (declared in strip_shared.h) so the
// window code can call it; it is defined just below the anonymous namespace.
// ----------------------------------------------------------------------------

// Extract "Artist - Title" via titleformat against the now-playing track.
void refresh_metadata() {
    auto pc = playback_control::get();

    static titleformat_object::ptr tf_title, tf_artist;
    if (tf_title.is_empty())
        titleformat_compiler::get()->compile_safe(tf_title, "%title%");
    if (tf_artist.is_empty())
        titleformat_compiler::get()->compile_safe(tf_artist, "%artist%");

    pfc::string8 title8, artist8;
    pc->playback_format_title(nullptr, title8, tf_title, nullptr,
                              playback_control::display_level_all);
    pc->playback_format_title(nullptr, artist8, tf_artist, nullptr,
                              playback_control::display_level_all);

    auto to_w = [](const pfc::string8& s) {
        pfc::stringcvt::string_wide_from_utf8 w(s.c_str());
        return std::wstring(w.get_ptr());
    };

    std::lock_guard<std::mutex> guard(g_state.lock);
    g_state.title = title8.is_empty() ? L"(unknown)" : to_w(title8);
    g_state.artist = to_w(artist8);
}

} // namespace

// External-linkage: called from StripWindow.cpp (declared in strip_shared.h).
void refresh_from_core() {
    auto pc = playback_control::get();

    // Read everything from the core FIRST, without holding our lock. foobar can
    // synchronously fire callbacks (e.g. on_volume_change) from these calls; if
    // we held g_state.lock here and a callback re-entered refresh_from_core, the
    // non-recursive mutex would self-deadlock ("resource deadlock would occur").
    bool playing = pc->is_playing() && !pc->is_paused();
    double position = pc->playback_get_position();
    double length = pc->playback_get_length();
    bool canSeek = pc->playback_can_seek();
    float db = pc->get_volume();
    bool muted = pc->is_muted();

    // Map foobar's dB volume to a 0..1 slider position. Volume in dB is
    // logarithmic, so sliderPos = 10^(dB/coeff). The coefficient 34 is taken
    // from Eldarien's DeskbandControls (GPL-3.0), whose author tuned it to
    // "match best with foobar2000 volume control behaviour" - it makes this
    // slider track foobar's own native volume slider.
    //   Source: https://github.com/Eldarien/DeskbandControls
    //   (dcmFoobar2000/Code/Controller.cs, _volumeDbCoeff)
    const double kVolCoeff = 34.0;
    double lin;
    if (db <= -100.0f) lin = 0.0;
    else lin = pow(10.0, (double)db / kVolCoeff);
    if (lin < 0) lin = 0; if (lin > 1) lin = 1;

    // Now take the lock only to publish the values (no core calls inside).
    std::lock_guard<std::mutex> guard(g_state.lock);
    g_state.playing = playing;
    g_state.position = position;
    g_state.length = length;
    g_state.canSeek = canSeek;
    g_state.volume_linear = lin;
    g_state.muted = muted;
}

namespace {

// ----------------------------------------------------------------------------
// play_callback_static - playback events, always on the main thread.
// ----------------------------------------------------------------------------
class strip_play_callback : public play_callback_static {
public:
    // Which events we want.
    unsigned get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_stop |
               flag_on_playback_pause | flag_on_playback_seek |
               flag_on_playback_time | flag_on_playback_dynamic_info_track |
               flag_on_volume_change;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        refresh_metadata();
        refresh_from_core();

        // Length straight from the new track's info is more reliable than asking
        // playback_control the instant the track changes (it may not be ready).
        if (track.is_valid()) {
            double len = track->get_length();
            std::lock_guard<std::mutex> guard(g_state.lock);
            if (len > 0) g_state.length = len;
            g_state.position = 0.0;       // new track starts at 0
        }

        // Reset the window's interpolation baseline so it doesn't carry the old
        // track's elapsed time into the new one (this is what made the seek
        // anchor stick on skip).
        strip_reset_interp();

        load_album_art();
        strip_notify_repaint();
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        {
            std::lock_guard<std::mutex> guard(g_state.lock);
            g_state.title = L"Nothing playing";
            g_state.artist.clear();
            g_state.position = g_state.length = 0.0;
            g_state.playing = false;
            g_state.canSeek = false;
            delete g_state.art;
            g_state.art = nullptr;
        }
        strip_notify_repaint();
    }

    void on_playback_pause(bool) override {
        refresh_from_core();
        strip_notify_repaint();
    }

    void on_playback_seek(double t) override {
        {
            std::lock_guard<std::mutex> guard(g_state.lock);
            g_state.position = t;
        }
        strip_reset_interp();  // re-anchor interpolation to the seeked position
        strip_notify_repaint();
    }

    // Fires ~once per second; the window timer interpolates between these.
    void on_playback_time(double t) override {
        {
            std::lock_guard<std::mutex> guard(g_state.lock);
            g_state.position = t;
        }
        // Re-anchor the interpolation baseline to this fresh reading. Without
        // this, the timer's dt keeps growing while position also jumps each
        // second, making the clock tick at ~2x speed.
        strip_sync_interp(t);
        strip_notify_repaint();
    }

    void on_playback_dynamic_info_track(const file_info&) override {
        refresh_metadata();
        strip_notify_repaint();
    }

    // Unused events (must be present).
    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_volume_change(float) override {
        refresh_from_core();
        strip_notify_repaint();
    }

private:
    // Pull album art for the current track and hand a GDI+ bitmap to the state.
    void load_album_art();
};

// Album art extraction via album_art_manager_v2.
void strip_play_callback::load_album_art() {
    Gdiplus::Bitmap* newBmp = nullptr;
    try {
        auto pc = playback_control::get();
        metadb_handle_ptr track;
        if (!pc->get_now_playing(track) || track.is_empty()) return;

        // Build the single-item handle list and single-id list the API expects.
        metadb_handle_list items;
        items.add_item(track);

        pfc::list_t<GUID> ids;
        ids.add_item(album_art_ids::cover_front);

        auto aamv2 = album_art_manager_v2::get();
        abort_callback_dummy abort;
        auto extractor = aamv2->open(items, ids, abort);

        album_art_data_ptr data = extractor->query(album_art_ids::cover_front, abort);

        // Radio streams and untagged files often have no embedded cover. Fall back
        // to foobar's configured stub image (Display > Album Art > Stub image) so
        // the thumbnail shows something sensible instead of a blank placeholder.
        // The stub is served by a SEPARATE extractor obtained via open_stub(),
        // not a method on the regular extractor.
        if (!(data.is_valid() && data->get_size() > 0)) {
            try {
                auto stubEx = aamv2->open_stub(abort);
                if (stubEx.is_valid())
                    data = stubEx->query(album_art_ids::cover_front, abort);
            } catch (...) { /* no stub configured -> leave blank, handled below */ }
        }

        if (data.is_valid() && data->get_size() > 0) {
            // Wrap raw bytes (JPEG/PNG) in an IStream for GDI+.
            IStream* stream = SHCreateMemStream(
                static_cast<const BYTE*>(data->get_ptr()),
                static_cast<UINT>(data->get_size()));
            if (stream) {
                newBmp = Gdiplus::Bitmap::FromStream(stream);
                stream->Release();
                if (newBmp && newBmp->GetLastStatus() != Gdiplus::Ok) {
                    delete newBmp;
                    newBmp = nullptr;
                }
            }
        }
    } catch (...) {
        // No art / extraction failed - leave null, window draws a placeholder.
    }

    std::lock_guard<std::mutex> guard(g_state.lock);
    delete g_state.art;
    g_state.art = newBmp;
}

static play_callback_static_factory_t<strip_play_callback> g_play_cb_factory;

// Defined after the factories below; touching the factory addresses from here
// (called in on_init) keeps MSVC /OPT:REF from stripping them.
static void strip_anchor_factories();

// ----------------------------------------------------------------------------
// initquit - create the floating window once foobar's main window exists.
// ----------------------------------------------------------------------------
class strip_initquit : public initquit {
public:
    void on_init() override {
        strip_anchor_factories();
        strip_create_window();
        // Prime initial state in case playback is already running.
        if (playback_control::get()->is_playing()) {
            refresh_metadata();
            refresh_from_core();
            strip_notify_repaint();
        }
    }
    void on_quit() override {
        strip_destroy_window();
        std::lock_guard<std::mutex> guard(g_state.lock);
        delete g_state.art;
        g_state.art = nullptr;
    }
};

static initquit_factory_t<strip_initquit> g_initquit_factory;

// ---- Main menu command: toggle strip visibility ----------------------------
// Registers a command under View menu that flips the master "Show strip" setting
// and applies it. Because it's a mainmenu_commands service, foobar also exposes
// it for keyboard shortcuts and for binding to toolbar buttons (e.g. ColumnUI).
static const GUID g_guid_toggle_strip =
    { 0x9a3f1c40, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x60 } };

class strip_mainmenu : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 1; }
    GUID get_command(t_uint32 index) override {
        return index == 0 ? g_guid_toggle_strip : pfc::guid_null;
    }
    void get_name(t_uint32 index, pfc::string_base& out) override {
        if (index == 0) out = "Toggle floating strip";
    }
    bool get_description(t_uint32 index, pfc::string_base& out) override {
        if (index == 0) { out = "Show or hide the floating playback strip."; return true; }
        return false;
    }
    GUID get_parent() override { return mainmenu_groups::view; }
    void execute(t_uint32 index, service_ptr_t<service_base>) override {
        if (index != 0) return;
        strip_save_show_strip(!strip_load_show_strip());
        strip_apply_visibility();
    }
};
static mainmenu_commands_factory_t<strip_mainmenu> g_mainmenu_factory;

// Anchor: reference both factory globals through a volatile sink so the linker
// keeps them under /OPT:REF (MSVC has no built-in retention for these). Called
// once from on_init, which always runs.
static void strip_anchor_factories() {
    static void* volatile keep[3];
    keep[0] = (void*)&g_play_cb_factory;
    keep[1] = (void*)&g_initquit_factory;
    keep[2] = (void*)&g_mainmenu_factory;
}

} // namespace

// Expose state + a seek entry point to the window module.
StripState& strip_get_state() { return g_state; }

void strip_seek_to(double seconds) {
    // Called from the window (main thread). Safe to hit the core directly.
    auto pc = playback_control::get();
    if (pc->playback_can_seek()) {
        pc->playback_seek(seconds);
    }
}

void strip_set_volume(double sliderPos) {
    // Slider position 0..1 -> dB, inverse of refresh_from_core's mapping:
    //   dB = coeff * log10(pos), coeff = 34 (matches foobar's native slider,
    //   per the original Deskband Controls plugin). pos 1.0 -> 0 dB,
    //   0.5 -> ~-10 dB, 0 -> -100 dB (foobar's floor).
    const double kVolCoeff = 34.0;
    if (sliderPos < 0) sliderPos = 0;
    if (sliderPos > 1) sliderPos = 1;
    float db = (sliderPos <= 0.0) ? -100.0f
                                  : (float)(kVolCoeff * log10(sliderPos));
    if (db < -100.0f) db = -100.0f;
    playback_control::get()->set_volume(db);
}

void strip_toggle_mute() {
    playback_control::get()->volume_mute_toggle();
}

// ----------------------------------------------------------------------------
// Theme persistence via foobar's config system. cfg_bool saves to foobar's
// configuration automatically and restores on startup. The GUID is a unique
// identifier for this setting - generate your own if you fork this.
// ----------------------------------------------------------------------------
static cfg_bool g_cfg_dark_mode(
    GUID{ 0x9a3f1c20, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x44 } },
    true /* default: dark */);

void strip_save_dark_mode(bool dark) { g_cfg_dark_mode = dark; }
bool strip_load_dark_mode() { return g_cfg_dark_mode; }

// Theme mode: 0=Light, 1=Dark, 2=Follow system, 3=Custom. Replaces the old
// dark/light bool as the source of truth (the bool is kept above only so old
// configs still load). Default 1 = Dark.
static cfg_int g_cfg_theme_mode(
    GUID{ 0x9a3f1c21, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x45 } }, 1);

int  strip_load_theme_mode()      { int m = (int)g_cfg_theme_mode;
                                    if (m < 0 || m > 3) m = 1; return m; }
void strip_save_theme_mode(int m) { if (m < 0 || m > 3) m = 1;
                                    g_cfg_theme_mode = m; }

// Custom colors (used only when theme mode == 3 / Custom). Stored as 0x00RRGGBB.
// Index contract shared with StripPrefs.cpp / theme():
//   0 background, 1 text (title+artist+time), 2 buttons (icons),
//   3 bar fill (accent), 4 bar track (groove), 5 popup padding (art backdrop).
static cfg_int g_cfg_col0(GUID{ 0x9a3f1c22, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x46 } }, (24 << 16)  | (26 << 8)  | 31);
static cfg_int g_cfg_col1(GUID{ 0x9a3f1c23, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x47 } }, (242 << 16) | (244 << 8) | 248);
static cfg_int g_cfg_col2(GUID{ 0x9a3f1c24, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x48 } }, (212 << 16) | (218 << 8) | 227);
static cfg_int g_cfg_col3(GUID{ 0x9a3f1c25, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x49 } }, (91 << 16)  | (167 << 8) | 245);
static cfg_int g_cfg_col4(GUID{ 0x9a3f1c26, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4a } }, (60 << 16)  | (64 << 8)  | 72);
static cfg_int g_cfg_col5(GUID{ 0x9a3f1c27, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4b } }, (18 << 16)  | (20 << 8)  | 24);

static cfg_int* color_cfg(int i) {
    switch (i) {
        case 0: return &g_cfg_col0; case 1: return &g_cfg_col1;
        case 2: return &g_cfg_col2; case 3: return &g_cfg_col3;
        case 4: return &g_cfg_col4; case 5: return &g_cfg_col5;
        default: return nullptr;
    }
}
int  strip_load_color(int i)         { cfg_int* c = color_cfg(i); return c ? ((int)*c & 0xFFFFFF) : 0; }
void strip_save_color(int i, int rgb){ cfg_int* c = color_cfg(i); if (c) *c = rgb & 0xFFFFFF; }

// Back-compat shims (background == index 0).
int  strip_load_color_bg()        { return strip_load_color(0); }
void strip_save_color_bg(int rgb) { strip_save_color(0, rgb); }

// Album-art popup size (square, in px before DPI scaling). Default 300. Clamped
// to a range that keeps it usable on small screens / not absurdly large.
static cfg_int g_cfg_popup_size(
    GUID{ 0x9a3f1c28, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4c } }, 300);

int  strip_load_popup_size()      { int s = (int)g_cfg_popup_size;
                                    if (s < 150) s = 150; if (s > 600) s = 600;
                                    return s; }
void strip_save_popup_size(int s) { if (s < 150) s = 150; if (s > 600) s = 600;
                                    g_cfg_popup_size = s; }

// Popup padding alpha (0 = fully transparent backdrop, 255 = fully opaque).
// Only the padding/backdrop uses this; the album art always draws opaque.
// Default 255 keeps the original solid look until the user dials it down.
static cfg_int g_cfg_popup_alpha(
    GUID{ 0x9a3f1c2d, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x51 } }, 255);

int  strip_load_popup_alpha()      { int a = (int)g_cfg_popup_alpha;
                                     if (a < 0) a = 0; if (a > 255) a = 255; return a; }
void strip_save_popup_alpha(int a) { if (a < 0) a = 0; if (a > 255) a = 255;
                                     g_cfg_popup_alpha = a; }

// Strip background alpha (0 = fully see-through, 255 = opaque). Only the strip's
// background fill uses this; all content (art/text/buttons/bars) stays opaque.
// Default 255 keeps the original solid look until the user dials it down.
static cfg_int g_cfg_bg_alpha(
    GUID{ 0x9a3f1c2e, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x52 } }, 255);

int  strip_load_bg_alpha()      { int a = (int)g_cfg_bg_alpha;
                                  if (a < 0) a = 0; if (a > 255) a = 255; return a; }
void strip_save_bg_alpha(int a) { if (a < 0) a = 0; if (a > 255) a = 255;
                                  g_cfg_bg_alpha = a; }

// Icon base sizes (px at default height; scaled by height like fonts). Index:
// 0 = transport (prev/play/next), 1 = speaker (volume/mute). Defaults match the
// previous hardcoded US(20)/US(14).
static cfg_int g_cfg_icon_transport(
    GUID{ 0x9a3f1c2f, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x53 } }, 20);
static cfg_int g_cfg_icon_speaker(
    GUID{ 0x9a3f1c30, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x54 } }, 14);

static cfg_int* icon_cfg(int which) {
    switch (which) { case 0: return &g_cfg_icon_transport;
                     case 1: return &g_cfg_icon_speaker; default: return nullptr; }
}
int  strip_load_icon_size(int which)       { cfg_int* c = icon_cfg(which); if (!c) return 20;
                                             int s = (int)*c; if (s < 8) s = 8; if (s > 60) s = 60; return s; }
void strip_save_icon_size(int which, int s){ cfg_int* c = icon_cfg(which); if (!c) return;
                                             if (s < 8) s = 8; if (s > 60) s = 60; *c = s; }

// Show/hide the volume bar + speaker icon. When hidden, the transport buttons
// reflow right to fill the space. Default true (shown).
static cfg_bool g_cfg_show_volume(
    GUID{ 0x9a3f1c31, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x55 } }, true);

bool strip_load_show_volume()        { return g_cfg_show_volume; }
void strip_save_show_volume(bool s)  { g_cfg_show_volume = s; }

// Show/hide the album-art hover/click popup (the large art + its padding). When
// false, hovering/clicking the inline thumbnail does not open the popup at all.
// Default true (shown).
static cfg_bool g_cfg_show_popup(
    GUID{ 0x9a3f1c31, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x5c } }, true);

bool strip_load_show_popup()         { return g_cfg_show_popup; }
void strip_save_show_popup(bool s)   { g_cfg_show_popup = s; }

// Master strip visibility. When false the whole strip window is hidden; the only
// way back is this setting (the strip isn't there to click). Persists across
// restarts (the create path respects it). Default true (shown).
static cfg_bool g_cfg_show_strip(
    GUID{ 0x9a3f1c34, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x58 } }, true);

bool strip_load_show_strip()        { return g_cfg_show_strip; }
void strip_save_show_strip(bool s)  { g_cfg_show_strip = s; }

// Control spacing (px at default width; scaled by width). Index 0 = gap between
// transport buttons (default 0, i.e. flush), 1 = gap between the button group
// and the volume control (default 4, the old fixed value).
static cfg_int g_cfg_space_btn(
    GUID{ 0x9a3f1c32, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x56 } }, 0);
static cfg_int g_cfg_space_vol(
    GUID{ 0x9a3f1c33, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x57 } }, 4);

static cfg_int* space_cfg(int which) {
    switch (which) { case 0: return &g_cfg_space_btn;
                     case 1: return &g_cfg_space_vol; default: return nullptr; }
}
int  strip_load_spacing(int which)       { cfg_int* c = space_cfg(which); if (!c) return 0;
                                           int s = (int)*c; if (s < 0) s = 0; if (s > 40) s = 40; return s; }
void strip_save_spacing(int which, int s){ cfg_int* c = space_cfg(which); if (!c) return;
                                           if (s < 0) s = 0; if (s > 40) s = 40; *c = s; }

// Font face (family name). cfg_string persists a UTF-8 string. Empty default
// means "use the built-in default" (Segoe UI), resolved in the paint code.
static cfg_string g_cfg_font_face(
    GUID{ 0x9a3f1c29, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4d } }, "Segoe UI");

void strip_load_font_face(pfc::string_base& out) { out = g_cfg_font_face; }
void strip_save_font_face(const char* face)       { g_cfg_font_face = face; }

// Font sizes (px, pre-DPI) for title / artist / time. Defaults match the
// original hardcoded relative sizes (13 / 10 / 12). Clamped to a sane range.
static cfg_int g_cfg_font_title(
    GUID{ 0x9a3f1c2a, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4e } }, 13);
static cfg_int g_cfg_font_artist(
    GUID{ 0x9a3f1c2b, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x4f } }, 10);
static cfg_int g_cfg_font_time(
    GUID{ 0x9a3f1c2c, 0x4b7e, 0x4e8a, { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x50 } }, 12);

static cfg_int* font_cfg(int which) {
    switch (which) { case 0: return &g_cfg_font_title;
                     case 1: return &g_cfg_font_artist;
                     case 2: return &g_cfg_font_time; default: return nullptr; }
}
int  strip_load_font_size(int which)       { cfg_int* c = font_cfg(which); if (!c) return 12;
                                             int s = (int)*c; if (s < 6) s = 6; if (s > 48) s = 48; return s; }
void strip_save_font_size(int which, int s){ cfg_int* c = font_cfg(which); if (!c) return;
                                             if (s < 6) s = 6; if (s > 48) s = 48; *c = s; }

// Window position persistence. INT_MIN sentinel = "never moved yet" so the very
// first run falls back to the default corner. Separate GUIDs per value.
static const int kPosUnset = INT_MIN;
static cfg_int g_cfg_pos_x(
    GUID{ 0x2c8e7d11, 0x6a4f, 0x4b2c,
          { 0x83, 0x55, 0x1d, 0x9e, 0x44, 0x2a, 0x77, 0x10 } }, kPosUnset);
static cfg_int g_cfg_pos_y(
    GUID{ 0x2c8e7d12, 0x6a4f, 0x4b2c,
          { 0x83, 0x55, 0x1d, 0x9e, 0x44, 0x2a, 0x77, 0x11 } }, kPosUnset);

void strip_save_position(int x, int y) {
    g_cfg_pos_x = x;
    g_cfg_pos_y = y;
}

bool strip_load_position(int& x, int& y) {
    int sx = (int)g_cfg_pos_x;
    int sy = (int)g_cfg_pos_y;
    if (sx == kPosUnset || sy == kPosUnset) return false; // never set
    x = sx;
    y = sy;
    return true;
}

// Strip width (the first customizable layout setting). cfg_int persists it to
// foobar's config and restores it on startup. Default 300 = the original width.
// The value is the UNSCALED base width in pixels (DPI scaling is applied on top
// in kWidth()). Clamped to a sane range when read so a bad value can't make the
// strip unusable.
static cfg_int g_cfg_width(
    GUID{ 0x3d9a2e55, 0x7c1b, 0x4f6d,
          { 0xa8, 0x23, 0x5e, 0x4f, 0x1c, 0x6b, 0x90, 0x22 } }, 300);

int  strip_load_width()       { int w = (int)g_cfg_width;
                                if (w < 300) w = 300; if (w > 800) w = 800;
                                return w; }
void strip_save_width(int w)  { if (w < 300) w = 300; if (w > 800) w = 800;
                                g_cfg_width = w; }

// Strip height. Same pattern as width. Default 46 = original height. Clamped to
// a range that keeps the strip usable (too short clips controls; too tall looks
// odd on a taskbar). The interior scales with height (see g_contentScale).
static cfg_int g_cfg_height(
    GUID{ 0x3d9a2e56, 0x7c1b, 0x4f6d,
          { 0xa8, 0x23, 0x5e, 0x4f, 0x1c, 0x6b, 0x90, 0x23 } }, 46);

int  strip_load_height()      { int h = (int)g_cfg_height;
                                if (h < 40) h = 40; if (h > 120) h = 120;
                                return h; }
void strip_save_height(int h) { if (h < 40) h = 40; if (h > 120) h = 120;
                                g_cfg_height = h; }
