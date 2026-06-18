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
    Gdiplus::Bitmap* art = nullptr; // owned; swapped under lock
    std::mutex lock;
};

// Implemented in foo_strip.cpp:
StripState& strip_get_state();
void strip_seek_to(double seconds);

// Implemented in StripWindow.cpp:
void strip_create_window();
void strip_destroy_window();
void strip_notify_repaint();
void strip_reset_interp();  // reset position-interpolation baseline (track change/seek)
void strip_sync_interp(double position); // re-anchor baseline to a fresh reported position
