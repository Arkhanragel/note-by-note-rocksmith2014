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
    int lateMs = 150;            // a correct note up to this LATE counts without stopping: the song stops only
                                 // this long after the note (if it wasn't played by then), so playing on the
                                 // beat doesn't stop it for a moment at every note. 0 = stop before the note
                                 // (leadMs). (Stopped past the note, the game counts it as passed: see leadMs.)
    int countInBeats = 3;        // after a long wait (over 2 s) ends with the note played, count this many
                                 // beats of the song's tempo (3-2-1 on screen) before it goes on; 0 = off
    bool acceptOctaves = false;  // the same note one octave higher/lower also counts
    bool tuningCheck = true;     // say when the guitar sounds out of tune with the song (tuning.h)
    bool showBanner = true;      // the banner with the note to play, while the song waits for it. (With
                                 // stopSong off, "Show the notes", the banner is the mode: always shown)
    bool stringsFromThick = false; // strings are named by number ("string 4 (D)", in the string's colour):
                                 // off = the standard numbering, 1 = the thinnest (high e); on = 1 is the
                                 // thickest (low E)
    bool bannerHand = true;      // fingers and hand position: finger numbers on the fretboard's dots, the
                                 // hand's zone shaded, and "Hand: move UP to fret 7" when it has to move
    bool bannerFingers = true;   // with bannerHand: the hand drawn under the banner's fretboard (four fingers
                                 // over the frets they cover); off = only the numbers on the dots
    int troubleClear = 3;        // trouble spots: a note played on time this many times in a row is no trouble
                                 // any more (1..10; stats.h)
    bool bannerNeck = true;      // the banner's picture is a piece of fretboard (the note as a dot in its
                                 // string's colour with the fret number, a red X where a wrong note was
                                 // played); off = the small tab
    int bannerLayout = 0;        // the banner's look: 0 = words beside a fretboard ("Play fret 7 on string 3 (G)")
                                 // with the next notes in a "Then" row; 1 = cards, no sentences: a row of small
                                 // fretboards, one per note or chord, that stay in place while a highlight moves
                                 // from one to the next (like the tab's pages)
    int bannerAhead = 3;         // how many of the next notes or chords the banner also shows (0..5; 0 = only
                                 // the one to play). A quick repeat of one note or chord counts once ("x4")
    bool waitChords = true;     // also wait at chords (off = chords pass, only single notes wait)
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
    bool tabPicks = true;        // pick strokes: marks above the tab's notes, and a line on the banner (picking.h)
    bool tabMarks = true;        // a note's box is coloured once the song passed it: green = played on time,
                                 // amber = the song waited for it, red = skipped or missed (TabNote::mark)
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

// A long note gets "Hold" on the banner: how long, and a countdown once it's played. The main loop sends
// View::sustain only for a note ringing about a beat or more (MainLoop::LongNote), never under this (s).
// (Shorter tails are just the note ringing until the next pick.)
constexpr double kHoldMinS = 0.2;

// One note or chord of the scrolling tab.
struct TabNote {
    double time = 0;             // song time (s)
    bool chord = false;
    bool ignore = false;         // not scored by the game: drawn faded
    int frets[6] = {-1, -1, -1, -1, -1, -1};  // per string (0 = thickest): -1 = not played
    std::string name;            // chord name ("A5"), empty for single notes / double stops
    double sustain = 0;          // seconds held: drawn as a tail after the fret number
    int streak = -1;             // a trouble spot's note (it went wrong before): played on time this many times in
    int need = 0;                // a row since, of `need` to clear it; -1 = not one (drawn as dots under the box)
    int mark = 0;                // how it went (setting tabMarks): 0 = not yet / not waited for, 1 = played
                                 // on time, 2 = the song waited for it, 3 = skipped or missed
    technique::Technique tech[6];  // per string: how it's played (slide, bend, hammer-on...), drawn in tab
                                   // notation around the fret number ("7/", "12 ^1", "h", "PM", "~")
    int pick = -1;               // pick stroke (setting tabPicks): 0 = down, 1 = up, -1 = none (not picked)
    bool pickFromSong = false;   // the song's own (else suggested from the rhythm: drawn a little fainter)
    std::string shapeName;       // a single note inside a held chord shape: its chord's name (may be empty),
    double shapeStart = -1;      // and when the shape starts and ends (the tab names it over its notes);
    double shapeEnd = -1;        // shapeEnd < 0 = not in one
};

// One beat line of the scrolling tab.
struct TabBeat {
    double time = 0;             // song time (s)
    int measure = 0;             // bar number (shown at bar lines)
    bool downbeat = false;       // first beat of a bar: a strong bar line
};

// One note of a quick repeat (the same note or chord again and again): how this one is played. The
// cards list them, since each can differ (picked, then hammered on, then palm-muted...).
struct RunNote {
    uint32_t tech = 0;           // its technique bits (technique.h; a chord's: all its strings' merged)
    int pick = -1;               // pick stroke: 0 = down, 1 = up, -1 = none
    int slide = 0;               // a slide from this note: +1 = up (to a higher fret), -1 = down, 0 = none
};

// One of the notes or chords that come after the banner's (setting bannerAhead): what the banner's "Then"
// row and its cards draw. A quick repeat of one note or chord is one step.
struct AheadStep {
    bool chord = false;
    int string = 0, fret = 0;    // single notes (0 = thickest string, fret 0 = open)
    std::string chordName;       // chords: as in the song; empty for double stops
    int frets[6] = {-1, -1, -1, -1, -1, -1};    // chords, per string (0 = thickest): -1 = not played
    int fingers[6] = {-1, -1, -1, -1, -1, -1};  // per string: 1 = index .. 4 = little, 0 = thumb, -1 = none
    technique::Technique tech;   // how to play it (as View::tech)
    technique::Technique strings[6];  // chords: each string's technique
    int anchorFret = 0, anchorWidth = 0;  // where the fretting hand is (0 = unknown)
    int count = 1;               // times in a row (a quick repeat: "x4")
    std::vector<RunNote> run;    // each note of that repeat, in order (one entry for a note played once)
    int pick = -1;               // pick stroke: 0 = down, 1 = up, -1 = none
    double time = -1;            // song time of its first note: tells the steps apart (the cards keep each
                                 // one in its place while the highlight moves over them)
};

// What the main loop wants on screen. Sent every loop iteration with SetView().
struct View {
    bool inSong = false;         // on a playing screen ("..._Game")
    bool songMenu = false;       // on the game's pause screen or its Riff Repeater screen, with a song loaded:
                                 // they show the same progress bar as the song, so the practice bar works
                                 // there too (songTime, songLength, sections and phrases are sent as in a song)
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
    int pick = -1;               // pick stroke (setting tabPicks): 0 = down, 1 = up, -1 = none (the banner says it)
    bool inShape = false;        // a single note inside a held chord shape: keep the chord pressed, pick its
    std::string shapeName;       // strings one by one. The chord's name (may be empty), its frets per string
    int shapeFrets[6] = {-1, -1, -1, -1, -1, -1};  // (-1 = not played): the banner's fretboard shows them faintly
    double sustain = 0;          // how long it rings (s, the tail on the highway); from kHoldMinS the banner
                                 // says how long to hold it
    double holdFrom = -1;        // the held note just played (from kHoldMinS): its song time and sustain, for
    double holdLen = 0;          // the countdown on the banner's last line ("Keep holding fret 9 ... 540 ms");
    int holdFret = 0;            // holdFrom -1 = none. Fret and string (single notes) or the chord's name
    int holdString = 0;
    std::string holdName;        // (chords; "" = a single note)
    int repeatLeft = 1;          // a quick repeat of this note (same string and fret, one right after the
    int repeatTotal = 1;         // other): how many are still to play, counting this one, and how many
                                 // the run has. The banner shows "x5" when the run has 2 or more
    std::vector<RunNote> run;    // each note of that repeat, in order, from the first one the banner showed
                                 // (the one to play now = the last repeatLeft of them)
    std::vector<AheadStep> ahead;  // what comes after it, in order (setting bannerAhead; empty = nothing)
    double stepTime = -1;        // the banner's own step, as AheadStep::time: the song time of the note itself, or
                                 // of the first note of the quick repeat it belongs to
    technique::Technique tech;  // how to play it (slide, bend...; the banner explains it); chords: all
                                 // strings merged, techFret = the fret it refers to
    int techFret = -1;
    technique::Technique strings[6];  // chords: each string's technique (the fretboard draws their slides)
    int anchorFret = 0, anchorWidth = 0;  // where the fretting hand is (0 = unknown)
    int handFrom = 0;            // the anchor of the note before (0 = unknown): the hand moves from there
    int fingers[6] = {-1, -1, -1, -1, -1, -1};  // per string: 1 = index .. 4 = little, 0 = thumb, -1 = none
    int countIn = 0;             // the count-in's number on screen (3, 2, 1), 0 = no count-in now
    std::vector<double> phraseStarts;  // when each phrase iteration starts (the practice bar's ticks and snaps)
    std::vector<std::pair<double, std::string>> sections;  // the song's sections: start, name ("Verse 2")
    std::string section;               // the one playing now (the clock shows it); "" = none
    std::vector<float> phraseHeat;     // trouble spots, per phrase iteration (as phraseStarts): 0 = none .. 1 = the
                                       // song's hardest (stats.h); red on the practice bar, listed in the menu
    std::string runSummary;            // this time in the song ("12 played on time, 3 waited for..."), for the menu
    std::vector<std::pair<int, int>> phraseCleared;  // per phrase iteration: its notes cleared, of those that went wrong
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

// True once after the player pressed "Forget this song's trouble spots" in the menu.
bool TakeForgetRequest();

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
