// foo_strip.cpp
// A standalone floating playback strip for foobar2000, rendered with GDI+.
// Because this runs in-process, it reads position/length and seeks DIRECTLY
// through playback_control — no SMTC, no bridge, no second component.
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
    "1.0.0",
    "A draggable floating strip with album art, title, transport, and a working "
    "seek bar. Reads playback directly in-process.\n");

// The packaged component contains foo_strip.dll (32-bit, at the archive root)
// and x64\foo_strip.dll (64-bit, in the x64 subfolder) — both named the same,
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
    // "match best with foobar2000 volume control behaviour" — it makes this
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
// play_callback_static — playback events, always on the main thread.
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
        // No art / extraction failed — leave null, window draws a placeholder.
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
// initquit — create the floating window once foobar's main window exists.
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

// Anchor: reference both factory globals through a volatile sink so the linker
// keeps them under /OPT:REF (MSVC has no built-in retention for these). Called
// once from on_init, which always runs.
static void strip_anchor_factories() {
    static void* volatile keep[2];
    keep[0] = (void*)&g_play_cb_factory;
    keep[1] = (void*)&g_initquit_factory;
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
// identifier for this setting — generate your own if you fork this.
// ----------------------------------------------------------------------------
static cfg_bool g_cfg_dark_mode(
    GUID{ 0x9a3f1c20, 0x4b7e, 0x4e8a,
          { 0x9c, 0x12, 0x7f, 0x3a, 0x6e, 0x5d, 0x21, 0x44 } },
    true /* default: dark */);

void strip_save_dark_mode(bool dark) { g_cfg_dark_mode = dark; }
bool strip_load_dark_mode() { return g_cfg_dark_mode; }

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
