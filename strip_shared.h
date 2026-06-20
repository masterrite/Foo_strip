// strip_shared.h
// Shared declarations used by BOTH foo_strip.cpp and StripWindow.cpp.
// Having one definition of StripState (instead of a separate struct in each
// .cpp) is what lets the linker match strip_get_state() across the two files.
#pragma once

#include <string>
#include <mutex>

namespace Gdiplus { class Bitmap; }

struct StripState {
    std::wstring title = L"Nothing playing";
    std::wstring artist;
    double position = 0.0;   // seconds
    double length = 0.0;     // seconds (0 => unknown/streaming)
    bool playing = false;
    bool canSeek = false;
    double volume_linear = 1.0; // 0..1 (mapped from foobar's dB volume)
    bool muted = false;
    Gdiplus::Bitmap* art = nullptr; // owned; swapped under lock
    std::mutex lock;
};

// Implemented in foo_strip.cpp:
StripState& strip_get_state();
void strip_seek_to(double seconds);
void strip_set_volume(double linear);   // 0..1
void strip_toggle_mute();
void refresh_from_core();               // re-read playback state (position, volume, mute)
void strip_save_dark_mode(bool dark);   // persist theme choice to foobar cfg
bool strip_load_dark_mode();            // read persisted theme choice (default true)
void strip_save_position(int x, int y); // persist last-dragged window position
bool strip_load_position(int& x, int& y); // read saved position; false if unset

// Implemented in StripWindow.cpp:
void strip_create_window();
void strip_destroy_window();
void strip_notify_repaint();
void strip_reset_interp();  // reset position-interpolation baseline (track change/seek)
void strip_sync_interp(double position); // re-anchor baseline to a fresh reported position
