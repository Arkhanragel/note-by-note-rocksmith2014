// overlay.h: the on-screen part of Note-by-Note (drawn inside the game with Dear ImGui).
//
// How it gets on screen: Rocksmith draws with Direct3D 9. We hook IDirect3DDevice9::Present (the call
// that shows each finished frame) with MinHook. Hooking d3d9.dll is allowed; only the game's own
// code is protected by VMProtect. Right before each frame is shown we draw our own things on top:
//
//   - the "waiting" banner: which string (name + highway colour) and fret to play, with a tiny tab
//     (for a chord: its name, every string's fret in colour, and the chord shape as a tab); after a
//     wrong note, a line saying how to fix it ("move UP 2 frets", "that's the ORANGE string")
//   - short messages ("toasts"): "Note-by-Note ON", "Skipped", "No chart for this song"...
//   - the song clock ("1:23 / 4:28") in the top-left corner while a song plays
//   - the scrolling tab: the next few seconds of the song as guitar tab, moving right to left in
//     step with the highway (left of the highway, below the lyrics by default),
//     with bar lines + bar numbers, faint beat lines and tails on held notes, to read the rhythm
//   - the MENU (toggle key, F8 by default): mode on/off, skip note, timing settings. While it is
//     open the banner, clock and tab can be dragged with the mouse (body = move, bottom-right
//     corner = resize); the menu has a button to put them all back
//
// Threads: the mod's main loop (main.cpp) and the game's render thread both use this module. They
// share one small state object protected by a mutex. The main loop writes what to show (View) and
// reads what the player asked for (Settings, skip requests); the render thread does the opposite.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "hint.h"

namespace nbn::overlay {

// Player settings the menu can change. main.cpp loads them from NoteByNote.ini, and saves them
// back when the menu changes them.
struct Settings {
    bool enabled = true;         // the mode itself (the song waits for each note)
    int leadMs = 0;              // stop this long BEFORE the note reaches the line
    int earlyMs = 300;           // a correct note up to this early counts without stopping
    bool acceptOctaves = false;  // the same note one octave higher/lower also counts
    bool showBanner = true;      // show "play this" while the song is waiting
    bool waitChords = true;      // also wait at chords (off = chords pass, only single notes wait)
    bool showClock = true;       // show the song time while playing
    bool showTab = true;         // show the scrolling tab while playing
    bool tabBeats = true;        // bar lines (with bar numbers) and beat lines in the tab
    int tabSeconds = 4;          // seconds of music ahead of the "now" line

    // Layout: where each part is and how big (the player drags them while the menu is open). All
    // positions and widths are in 1080p pixels, scaled with the screen height like the game's own
    // layout; sizes are percent of the normal size.
    int bannerX = 0, bannerY = 119;  // banner's top-centre: x from the screen centre, y from the top
    int bannerSize = 100;
    int clockX = 24, clockY = 24;    // clock's top-left corner, from the screen's top-left corner
    int clockSize = 100;
    int tabX = -810, tabY = 385;     // tab's top-left corner: x from the screen centre, y from the top
    int tabWidth = 640;              // tab's width (more width = more room between the notes)
    int tabSize = 100;               // tab's height and text size

    bool operator==(const Settings&) const = default;
};

// The settings with the layout fields (positions and sizes) back to their defaults.
Settings WithDefaultLayout(Settings st);

// One note or chord of the scrolling tab.
struct TabNote {
    double time = 0;             // song time (s)
    bool chord = false;
    bool ignore = false;         // not scored by the game: drawn faded
    int frets[6] = {-1, -1, -1, -1, -1, -1};  // per string (0 = thickest): -1 = not played
    std::string name;            // chord name ("A5"), empty for single notes / double stops
    double sustain = 0;          // seconds held: drawn as a tail after the fret number
};

// One beat line of the scrolling tab.
struct TabBeat {
    double time = 0;             // song time (s)
    int measure = 0;             // bar number (shown at bar lines)
    bool downbeat = false;       // first beat of a bar: a strong bar line
};

// What the main loop wants on screen. Sent every loop iteration with SetView().
struct View {
    bool inSong = false;         // on a playing screen ("..._Game")
    bool waiting = false;        // the song is frozen, waiting for a note
    bool bass = false;           // 4-string layout
    int string = 0;              // 0 = thickest string (low E), like the charts
    int fret = 0;                // 0 = open string
    int midi = -1;               // the note (single notes), for its name ("C", "F#")
    bool chord = false;          // waiting for a chord: chordName + frets instead of string/fret
    std::string chordName;       // as in the song ("Em", "A5"...); empty for double stops
    int frets[6] = {-1, -1, -1, -1, -1, -1};  // per string (0 = thickest): -1 = not played
    int notes[6] = {-1, -1, -1, -1, -1, -1};  // MIDI per string (for the note names)
    std::string chartInfo;       // one line for the menu, e.g. "Lead - matches the song"
    bool chartOk = false;
    double songTime = -1;        // seconds; < 0 = unknown (no clock shown)
    double songLength = 0;       // seconds; 0 = unknown
    std::vector<TabNote> tab;    // notes on the highway around songTime (empty = no tab)
    std::vector<TabBeat> tabBeats;  // the beat grid over the same time span (empty = no lines)
    hint::Line hint;             // while waiting: how to fix the last wrong note/chord (empty = none)
};

// Starts a thread that waits for d3d9.dll and installs the hooks. Returns immediately.
void Start(const Settings& initial);

// Removes the hooks and the window hook, frees ImGui (waits up to ~2 s for a frame). Call before
// the DLL unloads.
void Stop();

void SetView(const View& v);
void Toast(const std::string& text, DWORD ms = 2500);

// The menu: opened/closed by the main loop's toggle key (it polls the keyboard), or closed from
// inside the menu (Close button / Escape).
void ToggleMenu();
bool MenuOpen();

// The current settings (possibly changed by the menu since the last call).
Settings GetSettings();
void SetEnabled(bool on);

// True once after the player pressed "Skip this note" in the menu.
bool TakeSkipRequest();

}  // namespace nbn::overlay
