// overlay.h: the on-screen part of Note-by-Note (drawn inside the game with Dear ImGui).
//
// How it gets on screen: Rocksmith draws with Direct3D 9. We hook IDirect3DDevice9::Present (the call
// that shows each finished frame) with MinHook. Hooking d3d9.dll is allowed; only the game's own
// code is protected by VMProtect. Right before each frame is shown we draw our own things on top:
//
//   - the "waiting" banner: which string (name + highway colour) and fret to play, with a tiny tab
//   - short messages ("toasts"): "Note-by-Note ON", "Skipped", "No chart for this song"...
//   - the MENU (toggle key, F8 by default): mode on/off, skip note, timing settings
//
// Threads: the mod's main loop (main.cpp) and the game's render thread both use this module. They
// share one small state object protected by a mutex. The main loop writes what to show (View) and
// reads what the player asked for (Settings, skip requests); the render thread does the opposite.
#pragma once
#include <windows.h>

#include <string>

namespace nbn::overlay {

// Player settings the menu can change. main.cpp loads them from NoteByNote.ini, and saves them
// back when the menu changes them.
struct Settings {
    bool enabled = true;         // the mode itself (the song waits for each note)
    int leadMs = 0;              // stop this long BEFORE the note reaches the line
    int earlyMs = 300;           // a correct note up to this early counts without stopping
    bool acceptOctaves = false;  // the same note one octave higher/lower also counts
    bool showBanner = true;      // show "play this" while the song is waiting
};

// What the main loop wants on screen. Sent every loop iteration with SetView().
struct View {
    bool inSong = false;         // on a playing screen ("..._Game")
    bool waiting = false;        // the song is frozen, waiting for a note
    bool bass = false;           // 4-string layout
    int string = 0;              // 0 = thickest string (low E), like the charts
    int fret = 0;                // 0 = open string
    std::string chartInfo;       // one line for the menu, e.g. "Lead - matches the song"
    bool chartOk = false;
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
