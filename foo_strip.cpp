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

// Prevent the component .dll from being renamed. The expected filename differs
// by architecture: 64-bit foobar2000 components carry an "-x64" suffix and live
// in user-components-x64\, while 32-bit components are plain and live in
// user-components\. A single hardcoded name can't satisfy both, so pick per arch.
#ifdef _WIN64
VALIDATE_COMPONENT_FILENAME("foo_strip-x64.dll");
#else
VALIDATE_COMPONENT_FILENAME("foo_strip.dll");
#endif

// The single shared StripState instance (declared in strip_shared.h).
StripState g_state;

// Forward decl
class StripWindow;
StripWindow* g_window = nullptr;

// ----------------------------------------------------------------------------
// Helpers to pull current playback info from the core (main thread only).
// ----------------------------------------------------------------------------
void refresh_from_core() {
    auto pc = playback_control::get();

    std::lock_guard<std::mutex> guard(g_state.lock);
    g_state.playing = pc->is_playing() && !pc->is_paused();
    g_state.position = pc->playback_get_position();
    g_state.length = pc->playback_get_length();
    g_state.canSeek = pc->playback_can_seek();
}

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
               flag_on_playback_time | flag_on_playback_dynamic_info_track;
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
    void on_volume_change(float) override {}

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

// ----------------------------------------------------------------------------
// initquit — create the floating window once foobar's main window exists.
// ----------------------------------------------------------------------------
class strip_initquit : public initquit {
public:
    void on_init() override {
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
