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
//     with bar lines + bar numbers, faint beat lines, tails on held notes and rhythm stems/beams
//     under the staff, to read the rhythm; fast passages spread out so every fret stays readable
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
#include "technique.h"
#include "theme.h"

namespace nbn::overlay {

// Player settings the menu can change. main.cpp loads them from NoteByNote.ini, and saves them
// back when the menu changes them.
struct Settings {
    bool enabled = true;         // the mode itself (the song waits for each note)
    int leadMs = 30;             // stop this long BEFORE the note reaches the line. A little before, not
                                 // on it: stopped a few ms PAST a note, the game counts it as passed (on
                                 // its own pause screen it greys it out, and its fretboard already shows
                                 // the next chord)
    int earlyMs = 300;           // a correct note up to this early counts without stopping
    int countInBeats = 3;        // after a long wait (over 2 s) ends with the note played, count this many
                                 // beats of the song's tempo (3-2-1 on screen) before it goes on; 0 = off
    bool acceptOctaves = false;  // the same note one octave higher/lower also counts
    bool showBanner = true;      // show "play this" while the song is waiting
    bool stringsFromThick = false; // strings are named by number ("string 4 (D)", in the string's colour):
                                 // off = the standard numbering, 1 = the thinnest (high e); on = 1 is the
                                 // thickest (low E)
    bool bannerHand = true;      // fingers and hand position: finger numbers on the fretboard's dots, the
                                 // hand's zone shaded, and "Hand: move UP to fret 7" when it has to move
    bool bannerNeck = true;      // the banner's picture is a piece of fretboard (the note as a dot in its
                                 // string's colour with the fret number, a red X where a wrong note was
                                 // played); off = the small tab
    bool waitChords = true;      // also wait at chords (off = chords pass, only single notes wait)
    bool stopSong = true;        // the song stops at each note until it's played; off = guide only: the song
                                 // plays on, the banner shows the next note and moves on as the song passes it
    bool stringDetect = true;    // after a wrong single note, tell from its sound which string it was played on
                                 // (stringid.h) and show only that spot; needs a calibration (each open
                                 // string plucked a few times) and a clean signal. Off / not sure = the
                                 // guess plus faint marks on the other spots with that pitch
    bool skipGreyed = true;      // after resuming from the game's pause screen, the notes the game replays
                                 // greyed out are not waited for again
    bool showClock = true;       // show the song time while playing
    bool showPracticeBar = true; // the practice bar on the game's progress bar: drag on it with the mouse to
                                 // choose parts of the song; the mod only waits inside them
    bool showTab = true;         // show the scrolling tab while playing
    bool tabBeats = true;        // bar lines (with bar numbers) and beat lines in the tab
    bool tabRhythm = true;       // rhythm under the tab: stems + beams (how many notes per beat)
    int tabSeconds = 4;          // seconds of music ahead of the "now" line
    bool tabSpread = true;       // fast notes get a minimum gap (readable runs) and a fast repeat of
                                 // one fret is drawn once as "12 x8"; off = spacing exactly by time
    bool tabPage = true;         // the tab stands still and a cursor moves over it, turning the page
                                 // near the right edge; off = the notes scroll past a fixed line
    int tabRows = 1;             // pages only: 1..4 rows, one under the other. The cursor plays one row
                                 // while the others already show the next pages; when the cursor jumps
                                 // to the next row, the row it left gets the page after the last one
                                 // (no page turn to wait for)
    int tabRecap = 8;            // pages: how much of the previous page a new page repeats on its left,
                                 // percent of its width (0..50); the cursor starts a page right after it.
                                 // On the rows still to come, that repeated part is drawn dimmed
    bool tabMirror = false;      // left-handed: the tab runs right to left (string names on the right);
                                 // the banner's small tabs follow. Numbers and text are never mirrored
    bool tabThickTop = false;    // thickest string on top (off = thinnest on top, like printed tab);
                                 // the tab and the banner's small tabs follow
    int tabOpacity = 69;       // tab background, percent: 0 = none, 100 = solid (hides the game behind)
    int tabNoteSize = 100;       // size of the tab's fret numbers, percent (60..130); fast passages
                                 // shrink them a little below this by themselves
    bool skipPopups = true;      // at game start, answer the Ubisoft login/server dialogs (startup.h)
    int fastIntro = 4;           // start-up logos this many times faster, 1 = normal (fastintro.h)
    bool fixCrash = true;        // remove the protector's NtProtectVirtualMemory redirect (crashfix.h)

    // Colours (theme.h): a ready-made theme, and the player's own colour for any of its slots
    // (0xRRGGBB, -1 = the theme's). The string colours are always the game's.
    int theme = 0;               // index into theme::kThemes
    int colors[theme::kSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

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
    int mistakeX = 0, mistakeY = 0;  // the wrong-note panel: offset from its place beside the banner (right
                                     // of it, tops level), so it follows the banner; 0, 0 = beside it (on
                                     // its left or under it when there's no room on the right)
    int mistakeSize = 100;           // the wrong-note panel's size

    bool operator==(const Settings&) const = default;
};

static_assert(theme::kSlots == 9, "Settings::colors needs one -1 per theme slot");

// The settings with the layout fields (positions and sizes) back to their defaults.
Settings WithDefaultLayout(Settings st);

// The colour in use for a theme slot: the player's own, or else the theme's (0xRRGGBB).
uint32_t Color(const Settings& st, theme::Slot slot);

// One note or chord of the scrolling tab.
struct TabNote {
    double time = 0;             // song time (s)
    bool chord = false;
    bool ignore = false;         // not scored by the game: drawn faded
    int frets[6] = {-1, -1, -1, -1, -1, -1};  // per string (0 = thickest): -1 = not played
    std::string name;            // chord name ("A5"), empty for single notes / double stops
    double sustain = 0;          // seconds held: drawn as a tail after the fret number
    technique::Technique tech[6];  // per string: how it's played (slide, bend, hammer-on...), drawn in tab
                                   // notation around the fret number ("7/", "12 ^1", "h", "PM", "~")
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
    double waitTime = -1;        // while waiting: the song time of that note (the tab highlights it;
                                 // the song clock stops a few ms after it, so "now" can't tell)
    double greyTime = -1;        // notes before this song time are greyed out on the highway and not
                                 // waited for (after resuming from the pause screen); -1 = none
    double nextWaitTime = -1;    // while playing: the next note the song will stop at if it isn't
                                 // played (-1 = none). The tab's cursor never passes it
    bool upcoming = false;       // not waiting: the note fields below are the NEXT note the song will stop
                                 // at (the banner shows it early, so it doesn't vanish between fast notes)
    bool bass = false;           // 4-string layout
    int string = 0;              // 0 = thickest string (low E), like the charts
    int fret = 0;                // 0 = open string
    int midi = -1;               // the note (single notes), for its name ("C", "F#")
    int repeatLeft = 1;          // a quick repeat of this note (same string and fret, one right after the
    int repeatTotal = 1;         // other): how many are still to play, counting this one, and how many
                                 // the run has. The banner shows "x5" when the run has 2 or more
    technique::Technique tech;  // how to play it (slide, bend...; the banner explains it); chords: all
                                 // strings merged, techFret = the fret it refers to
    int techFret = -1;
    technique::Technique strings[6];  // chords: each string's technique (the fretboard draws their slides)
    int anchorFret = 0, anchorWidth = 0;  // where the fretting hand is (0 = unknown)
    int handFrom = 0;            // the anchor of the note before (0 = unknown): the hand moves from there
    int fingers[6] = {-1, -1, -1, -1, -1, -1};  // per string: 1 = index .. 4 = little, 0 = thumb, -1 = none
    int countIn = 0;             // the count-in's number on screen (3, 2, 1), 0 = no count-in now
    std::vector<double> phraseStarts;  // when each phrase iteration starts (the practice bar's ticks and snaps)
    std::vector<technique::Link> chain;  // the note and the notes linked after it (same string, not picked
                                         // again), for the banner's steps; empty = just `tech`
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
    std::vector<hint::Mark> heardAt;  // and where it was probably played (red X on the banner's fretboard)
    // String identification (the menu's "Wrong notes" part): a line about its state, shown as a warning
    // when something is wrong (no calibration, processed signal...); calibrating = the menu shows Cancel.
    std::string stringIdStatus;
    bool stringIdWarn = false;
    bool calibrating = false;
    bool calibrated = false;
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

// Once after the player pressed the menu's Calibrate (1) or Cancel (2) button for the string
// identification; 0 = nothing asked.
int TakeCalibrationRequest();

// The practice parts the player chose on the practice bar (song seconds, in time order, not
// overlapping): the mod waits only for the notes inside them. Empty = the whole song. ClearRanges: a
// new song starts with none.
using Range = std::pair<double, double>;
std::vector<Range> GetRanges();
void ClearRanges();

// The game window (nullptr until the overlay is ready). Any thread.
HWND GameWindow();

}  // namespace nbn::overlay
