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
    unsigned artGen = 0;            // bumped on every art swap; cache key for the
                                    // strip thumbnail (pointer identity is NOT
                                    // reliable - the heap can reuse the freed
                                    // address for the next track's bitmap)
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
int  strip_load_theme_mode();           // 0=Light 1=Dark 2=Follow-system 3=Custom
void strip_save_theme_mode(int m);
int  strip_load_color_bg();             // custom background as 0xRRGGBB
void strip_save_color_bg(int rgb);
int  strip_load_color(int i);           // custom color i (0..5) as 0xRRGGBB
void strip_save_color(int i, int rgb);  // i: 0 bg,1 text,2 buttons,3 fill,4 track,5 popup
int  strip_load_popup_size();           // album-art popup size (px, pre-DPI)
void strip_save_popup_size(int s);
int  strip_load_popup_alpha();          // popup padding alpha 0..255
void strip_save_popup_alpha(int a);
int  strip_load_bg_alpha();             // strip background alpha 0..255
void strip_save_bg_alpha(int a);
int  strip_load_icon_size(int which);   // 0 transport, 1 speaker (base px)
void strip_save_icon_size(int which, int s);
bool strip_load_show_volume();          // show volume bar + speaker icon
void strip_save_show_volume(bool s);
bool strip_load_show_popup();           // show album-art hover/click popup
void strip_save_show_popup(bool s);
bool strip_load_show_strip();           // master strip visibility
void strip_save_show_strip(bool s);
void strip_apply_visibility();          // show/hide the strip per the setting
int  strip_load_spacing(int which);     // 0 between buttons, 1 before volume
void strip_save_spacing(int which, int s);
void strip_load_font_face(pfc::string_base& out); // font family name
void strip_save_font_face(const char* face);
int  strip_load_font_size(int which);   // which: 0 title, 1 artist, 2 time
void strip_save_font_size(int which, int s);
void strip_save_position(int x, int y); // persist last-dragged window position
bool strip_load_position(int& x, int& y); // read saved position; false if unset
int  strip_load_width();                // current configured width (clamped)
void strip_save_width(int w);           // persist configured width
int  strip_load_height();               // current configured height (clamped)
void strip_save_height(int h);          // persist configured height

// Implemented in StripWindow.cpp:
void strip_create_window();
void strip_destroy_window();
void strip_notify_repaint();
void strip_open_preferences();          // open foobar Preferences at our page
void strip_apply_settings();            // re-read config + resize/repaint window
void strip_reset_interp();  // reset position-interpolation baseline (track change/seek)
void strip_sync_interp(double position); // re-anchor baseline to a fresh reported position
