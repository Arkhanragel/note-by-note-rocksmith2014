// main.cpp: Note-by-Note for Rocksmith 2014, the mod's entry point and "wait mode" logic.
//
// Loaded by our RS_ASIO build (it loads NoteByNote.dll from the game folder at startup), or during
// development by nbn_inject.exe. Everything runs on one background thread:
//
//   every ~1 ms:  read new guitar samples from the GuitarTap -> NoteTracker -> note events
//                 read the game state (screen, song key, song clock, Dynamic Difficulty levels)
//                 WAIT MODE: when the clock reaches the next note ON THE HIGHWAY and it hasn't been
//                            played -> freeze the song; when the player plays it -> unfreeze
//                            (single notes: NoteTracker events; chords: ChordDetector checks after
//                            each pick attack, see detector.h)
//                 tell the overlay (overlay.h) what to show: "play fret 3 on the BLUE string"...
//
// Keys (polled, only while the game window is focused): F8 = the Note-by-Note menu (mode on/off,
// settings; the song is held while it is open), F9 = skip the note the song is waiting for.
//
// "The next note on the highway" = the next note of the chart, where each phrase iteration uses its
// CURRENT Dynamic Difficulty level. Both the chart (all levels) and the current levels are read from
// game memory (see game.h / chart.h), so any song or CDLC works without preparing anything.
//
// Configuration: NoteByNote.ini next to the DLL (created with defaults on first run).
// Log: NoteByNote.log next to the DLL. NoteByNote_report.txt: what was found in this game build and
// whether the checks passed (report.h; for testing other game versions).
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "chart.h"
#include "detector.h"
#include "crashfix.h"
#include "crashlog.h"
#include "fastintro.h"
#include "game.h"
#include "log.h"
#include "overlay.h"
#include "report.h"
#include "startup.h"
#include "stats.h"
#include "stringid.h"
#include "tap.h"
#include "tuning.h"

namespace nbn {
namespace {

HMODULE g_self = nullptr;

// ------------------------------------------------------------------ configuration
// Fixed settings (read once). The ones the player can change in the menu are overlay::Settings.
struct Config {
    overlay::Settings initial;           // Enabled, LeadMs, EarlyMs, AcceptOctaves, ShowBanner, WaitChords, ShowClock, Tab*
    int menuKey = VK_F8;
    int skipKey = VK_F9;
    std::string menuSuffix = "_Game";    // the mode only acts on screens whose name ends like this
    std::string menuSound = "Nav_InGame_Options";
    bool saveWaitAudio = false;          // record each wait to NoteByNote_debug\ (for bug reports)
    bool crashLog = true;                // write the game's serious errors to the log (crashlog.h)
    bool testUnverifiedGame = false;     // run on a game build nobody verified (addresses found by pattern)
    int testAutoPassMs = 0;              // a wait passes by itself after this long (testing without a guitar)
    bool testPatternsOnly = false;       // dev: ignore the verified addresses, use only the patterns
    bool testFastIntro = false;          // the fast intro also on a game build that isn't verified (to try it there)
    std::string stringCalibration;       // the string identification's calibration (stringid::Calibration text)
};

std::wstring DllDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring IniPath() { return DllDir() + L"NoteByNote.ini"; }

// The game's folder (Rocksmith2014.exe), with a trailing backslash.
std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// Wide -> narrow for text that is ASCII by nature (ini theme names, "#RRGGBB", our own file names).
// Any other character becomes '?' instead of a truncated wchar_t that could turn into an unrelated letter.
std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t ch : w) s += ch < 128 ? (char)ch : '?';
    return s;
}

int ParseKey(const std::wstring& k, int def) {
    if (k.size() >= 2 && (k[0] == L'F' || k[0] == L'f')) {
        int n = _wtoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (k.rfind(L"0x", 0) == 0) return (int)wcstol(k.c_str(), nullptr, 16);
    return def;
}

Config LoadConfig() {
    const std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First run: write a commented default file the player can edit.
        FILE* f = _wfopen(ini.c_str(), L"w");
        if (f) {
            std::fputs(
                "; Note-by-Note for Rocksmith 2014 - settings\n"
                "; The song waits at each note and chord until you play it.\n"
                "[NoteByNote]\n"
                "; Most of these can be changed in the game: press the menu key (F8) during a song.\n"
                "; 1 = the mode is switched on, 0 = off (the menu changes and saves this)\n"
                "Enabled=1\n"
                "; Key that opens the Note-by-Note menu (F1..F12). Avoid F10 (Windows menu key), F11 and\n"
                "; F12 (Steam screenshot).\n"
                "MenuKey=F8\n"
                "; Key that skips the note the song is waiting for\n"
                "SkipKey=F9\n"
                "; Stop this many milliseconds BEFORE the note reaches the line (0 = exactly on it; a little\n"
                ";     before keeps the game from counting the note as passed if you pause while it waits)\n"
                "LeadMs=30\n"
                "; A correct note played up to this many milliseconds early counts without stopping\n"
                "EarlyMs=300\n"
                "; A correct note played up to this many milliseconds late counts without stopping: the song\n"
                ";     stops only this long after the note if you haven't played it by then (0 = stop LeadMs\n"
                ";     before the note instead)\n"
                "LateMs=150\n"
                "; After a long wait (over 2 s) ends with the note played, count this many beats (3-2-1 on\n"
                ";     screen, at the song's tempo) before the song goes on; 0 = no count-in\n"
                "CountInBeats=3\n"
                "; 1 = the same note one octave higher/lower also counts\n"
                "AcceptOctaves=0\n"
                "; 1 = say when a string (or the whole guitar) sounds out of tune with the song\n"
                "TuningCheck=1\n"
                "; 1 = after a wrong single note, show only the spot you really played it on (told by its\n"
                ";     sound). Needs the calibration in the F8 menu (Playing page) and a clean guitar sound\n"
                "StringDetect=1\n"
                "; Written by the F8 menu's Calibrate (each open string's sound); empty = not calibrated\n"
                "StringCalibration=\n"
                "; 1 = the banner with the note to play, while the song waits for it (with StopSong=0 the\n"
                ";     banner is always shown: it is what that mode does)\n"
                "ShowBanner=1\n"
                "; 1 = the banner shows the note on a piece of fretboard (and where a wrong note was played),\n"
                ";     0 = as a small tab\n"
                "BannerFretboard=1\n"
                "; 1 = finger numbers on the fretboard, the hand's zone, and a hint when the hand has to move\n"
                "BannerHand=0\n"
                "; With BannerHand=1: 1 = also draw the hand under the fretboard, 0 = only the finger numbers\n"
                "BannerFingers=1\n"
                "; The banner's look: 0 = words beside a fretboard, with the next notes in a \"Then\" row;\n"
                ";     1 = cards, no sentences: a row of small fretboards, one per note, that stay in place\n"
                ";     while a highlight moves from one to the next\n"
                "BannerLayout=0\n"
                "; How many of the next notes or chords the banner also shows (0..5; 0 = only the one to play)\n"
                "BannerAhead=0\n"
                "; 0 = strings are numbered like in guitar books (high e = string 1), 1 = from the thickest\n"
                ";     (low E = string 1)\n"
                "StringsFromThickest=0\n"
                "; 1 = the song also waits at chords, 0 = chords pass (only single notes wait)\n"
                "WaitChords=1\n"
                "; With Enabled=1: 1 = the song stops at each note until you play it (menu: Wait for each note),\n"
                ";     0 = the song plays on, the banner shows the next note and moves on as the song passes it\n"
                ";     (menu: Show the notes)\n"
                "StopSong=1\n"
                "; 1 = after resuming from the game's pause screen, don't wait again for the notes the game\n"
                ";     replays greyed out (the few seconds before where you paused)\n"
                "SkipGreyedNotes=1\n"
                "; 1 = show the song time (top-left corner) while playing\n"
                "ShowClock=1\n"
                "; 1 = the practice bar under the game's progress bar (drag on it to practise a part of the song)\n"
                "ShowPracticeBar=1\n"
                "; Trouble spots (red on the practice bar): a note played on time this many times in a row\n"
                ";     no longer counts as trouble (1..10)\n"
                "TroubleClearAfter=3\n"
                "; 1 = show the notes coming up as a scrolling tab (left of the highway, below the lyrics)\n"
                "ShowTab=1\n"
                "; 1 = bar lines (with bar numbers) and beat lines in the tab, to read the rhythm\n"
                "TabBeats=1\n"
                "; 1 = rhythm under the tab, as in printed tab: a stem per note, beams = notes per beat\n"
                ";     (no beam = 1, 1 beam = 2, 2 beams = 4, 3 beams = 8; a small 3 = triplets)\n"
                "TabRhythm=0\n"
                "; 1 = pick strokes above the tab's notes (down / up): the song's, or suggested from the rhythm\n"
                "TabPicks=0\n"
                "; 1 = colour each note on the tab once the song has passed it: green = played on time,\n"
                ";     amber = the song waited for it, red = skipped or not played\n"
                "TabMarks=1\n"
                "; Seconds of music the tab shows ahead (2..8)\n"
                "TabSeconds=4\n"
                "; 1 = in fast passages the tab spreads the notes out so every fret can be read (the tab\n"
                ";     scrolls faster there), and a fast repeat of one fret shows once as \"12 x8\";\n"
                "; 0 = spacing exactly by time\n"
                "TabSpread=1\n"
                "; Size of the fret numbers in the tab, percent (60..130); fast passages make them a\n"
                "; little smaller by themselves\n"
                "TabNoteSize=100\n"
                "; 1 = the tab stands still and a cursor moves over the notes, turning the page near the\n"
                ";     right edge (easy to read fast parts); 0 = the notes scroll past a fixed line\n"
                "TabPages=1\n"
                "; Pages only: rows of tab, 1..4. With 2 or more, the cursor plays one row while the\n"
                ";     others already show the next pages (a row gets a new page as soon as the cursor\n"
                ";     leaves it)\n"
                "TabRows=1\n"
                "; Pages only: how much of the end of the previous page a new page repeats on its left,\n"
                ";     percent of the width (0..50); on rows still to come it's drawn dimmed\n"
                "TabRepeat=8\n"
                "; 1 = left-handed tab: the notes run right to left, string names on the right\n"
                "TabMirror=0\n"
                "; 1 = thickest string on top of the tab, 0 = thinnest on top (like printed tab)\n"
                "TabThickOnTop=0\n"
                "; Tab background, percent: 0 = see-through, 100 = solid (hides the game's text behind it)\n"
                "TabBackground=69\n"
                "; Colours of the banner, clock, tab and menu: Default, High contrast, Midnight, Vintage or\n"
                ";     Paper. Any single colour can be changed in the F8 menu (Colours), or here as a hex\n"
                ";     code, e.g. ColorChord=#FFCE54 (keys: ColorPanel, ColorText, ColorTextDim, ColorChord,\n"
                ";     ColorWarning, ColorHighlight, ColorGrid, ColorRhythm, ColorMenu; missing = the theme's).\n"
                ";     The string colours are the game's and don't change.\n"
                "Theme=Default\n"
                "; 1 = at game start, close the Ubisoft login and \"servers not available\" popups by\n"
                ";     themselves (the title's Press Enter and the profile choice stay yours)\n"
                "SkipUbisoftPopups=1\n"
                "; Play the start-up logos this many times faster (1 = normal speed, up to 8; 4 is what the menu sets)\n"
                "FastIntro=1\n"
                "; 1 = work around the game's own random crash / freeze (mostly at start-up): puts back\n"
                ";     a Windows function the game's copy protection redirects (in memory only)\n"
                "FixGameCrash=1\n"
                "; 1 = save the guitar audio of each wait to NoteByNote_debug\\wait_<time>.wav (useful\n"
                ";     to report a note that wasn't recognised; the files add up, delete them by hand)\n"
                "SaveWaitAudio=0\n"
                "; 1 = if the game hits a serious error (a crash), write where it happened to\n"
                ";     NoteByNote.log, so the log can be sent with a report right away (0 = off)\n"
                "CrashLog=1\n"
                "; For testers of other game versions (leave both at 0 otherwise):\n"
                "; 1 = on a game version Note-by-Note doesn't know yet, use the game addresses it finds by\n"
                ";     itself (see NoteByNote_report.txt)\n"
                "TestUnverifiedGame=0\n"
                "; Milliseconds after which a wait passes by itself, as if you played the note (0 = off;\n"
                "; to test without a guitar, e.g. 2000)\n"
                "TestAutoPassMs=0\n"
                "; Layout. Easier: open the menu and drag the parts with the mouse (corner = resize).\n"
                "; Positions and widths in 1080p pixels (scaled with the screen height), sizes in percent.\n"
                "; Banner: X of its centre from the screen centre, Y from the top\n"
                "BannerX=0\n"
                "BannerY=119\n"
                "BannerSize=100\n"
                "; Clock: top-left corner from the screen's top-left corner\n"
                "ClockX=24\n"
                "ClockY=24\n"
                "ClockSize=100\n"
                "; Tab: top-left corner, X from the screen centre, Y from the top\n"
                "TabX=-810\n"
                "TabY=385\n"
                "TabWidth=640\n"
                "TabSize=100\n"
                "; Wrong-note panel: offset from its place beside the banner (0, 0 = right beside it)\n"
                "MistakeX=0\n"
                "MistakeY=0\n"
                "MistakeSize=100\n",
                f);
            std::fclose(f);
        }
    }
    Config c;
    wchar_t buf[512];
    auto str = [&](const wchar_t* key, const wchar_t* def) {
        GetPrivateProfileStringW(L"NoteByNote", key, def, buf, 512, ini.c_str());
        return std::wstring(buf);
    };
    c.initial.enabled = GetPrivateProfileIntW(L"NoteByNote", L"Enabled", 1, ini.c_str()) != 0;
    // MenuKey; older ini files called it ToggleKey (it used to switch the mode directly).
    c.menuKey = ParseKey(str(L"MenuKey", str(L"ToggleKey", L"F8").c_str()), VK_F8);
    c.skipKey = ParseKey(str(L"SkipKey", L"F9"), VK_F9);
    c.initial.leadMs = GetPrivateProfileIntW(L"NoteByNote", L"LeadMs", 30, ini.c_str());
    c.initial.earlyMs = GetPrivateProfileIntW(L"NoteByNote", L"EarlyMs", 300, ini.c_str());
    c.initial.lateMs = std::max(0, std::min(400, (int)GetPrivateProfileIntW(L"NoteByNote", L"LateMs", 150, ini.c_str())));
    c.initial.countInBeats = std::max(0, std::min(4, (int)GetPrivateProfileIntW(L"NoteByNote", L"CountInBeats", 3, ini.c_str())));
    c.initial.acceptOctaves = GetPrivateProfileIntW(L"NoteByNote", L"AcceptOctaves", 0, ini.c_str()) != 0;
    c.initial.tuningCheck = GetPrivateProfileIntW(L"NoteByNote", L"TuningCheck", 1, ini.c_str()) != 0;
    c.initial.stringDetect = GetPrivateProfileIntW(L"NoteByNote", L"StringDetect", 1, ini.c_str()) != 0;
    c.stringCalibration = Narrow(str(L"StringCalibration", L""));
    c.initial.showBanner = GetPrivateProfileIntW(L"NoteByNote", L"ShowBanner", 1, ini.c_str()) != 0;
    c.initial.bannerNeck = GetPrivateProfileIntW(L"NoteByNote", L"BannerFretboard", 1, ini.c_str()) != 0;
    c.initial.bannerHand = GetPrivateProfileIntW(L"NoteByNote", L"BannerHand", 0, ini.c_str()) != 0;
    c.initial.bannerFingers = GetPrivateProfileIntW(L"NoteByNote", L"BannerFingers", 1, ini.c_str()) != 0;
    c.initial.bannerLayout = GetPrivateProfileIntW(L"NoteByNote", L"BannerLayout", 0, ini.c_str()) == 1 ? 1 : 0;
    c.initial.bannerAhead = std::max(0, std::min(5, (int)GetPrivateProfileIntW(L"NoteByNote", L"BannerAhead", 0, ini.c_str())));
    c.initial.troubleClear = std::max(1, std::min(10, (int)GetPrivateProfileIntW(L"NoteByNote", L"TroubleClearAfter", 3, ini.c_str())));
    c.initial.stringsFromThick = GetPrivateProfileIntW(L"NoteByNote", L"StringsFromThickest", 0, ini.c_str()) != 0;
    c.initial.waitChords = GetPrivateProfileIntW(L"NoteByNote", L"WaitChords", 1, ini.c_str()) != 0;
    c.initial.showClock = GetPrivateProfileIntW(L"NoteByNote", L"ShowClock", 1, ini.c_str()) != 0;
    c.initial.showPracticeBar = GetPrivateProfileIntW(L"NoteByNote", L"ShowPracticeBar", 1, ini.c_str()) != 0;
    c.initial.showTab = GetPrivateProfileIntW(L"NoteByNote", L"ShowTab", 1, ini.c_str()) != 0;
    c.initial.tabBeats = GetPrivateProfileIntW(L"NoteByNote", L"TabBeats", 1, ini.c_str()) != 0;
    c.initial.tabSeconds = std::max(2, std::min(8, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabSeconds", 4, ini.c_str())));
    c.initial.tabRhythm = GetPrivateProfileIntW(L"NoteByNote", L"TabRhythm", 0, ini.c_str()) != 0;
    c.initial.tabMarks = GetPrivateProfileIntW(L"NoteByNote", L"TabMarks", 1, ini.c_str()) != 0;
    c.initial.tabPicks = GetPrivateProfileIntW(L"NoteByNote", L"TabPicks", 0, ini.c_str()) != 0;
    c.initial.tabSpread = GetPrivateProfileIntW(L"NoteByNote", L"TabSpread", 1, ini.c_str()) != 0;
    c.initial.tabPage = GetPrivateProfileIntW(L"NoteByNote", L"TabPages", 1, ini.c_str()) != 0;
    // TabRows; older ini files have TabTwoRows=1 instead.
    const int twoRowsOld = GetPrivateProfileIntW(L"NoteByNote", L"TabTwoRows", 0, ini.c_str()) != 0 ? 2 : 1;
    c.initial.tabRows = std::max(1, std::min(4, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabRows", twoRowsOld, ini.c_str())));
    c.initial.tabRecap = std::max(0, std::min(50, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabRepeat", 8, ini.c_str())));
    c.initial.tabMirror = GetPrivateProfileIntW(L"NoteByNote", L"TabMirror", 0, ini.c_str()) != 0;
    c.initial.tabThickTop = GetPrivateProfileIntW(L"NoteByNote", L"TabThickOnTop", 0, ini.c_str()) != 0;
    c.initial.tabOpacity = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabBackground", 69, ini.c_str())));
    c.initial.tabNoteSize = std::max(60, std::min(130, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabNoteSize", 100, ini.c_str())));
    // Colours: the theme by name, then the player's own colours ("#RRGGBB"; missing or not a colour = the theme's).
    c.initial.theme = theme::FindTheme(Narrow(str(L"Theme", L"Default")));
    for (int i = 0; i < theme::kSlots; ++i) {
        const std::string key = theme::kSlotInfo[i].key;
        c.initial.colors[i] = theme::ParseHex(Narrow(str(std::wstring(key.begin(), key.end()).c_str(), L"")));
    }
    c.initial.skipGreyed = GetPrivateProfileIntW(L"NoteByNote", L"SkipGreyedNotes", 1, ini.c_str()) != 0;
    c.initial.stopSong = GetPrivateProfileIntW(L"NoteByNote", L"StopSong", 1, ini.c_str()) != 0;
    c.initial.skipPopups = GetPrivateProfileIntW(L"NoteByNote", L"SkipUbisoftPopups", 1, ini.c_str()) != 0;
    c.initial.fastIntro = std::max(1, std::min(8, (int)GetPrivateProfileIntW(L"NoteByNote", L"FastIntro", 1, ini.c_str())));
    c.initial.fixCrash = GetPrivateProfileIntW(L"NoteByNote", L"FixGameCrash", 1, ini.c_str()) != 0;
    c.saveWaitAudio = GetPrivateProfileIntW(L"NoteByNote", L"SaveWaitAudio", 0, ini.c_str()) != 0;
    c.crashLog = GetPrivateProfileIntW(L"NoteByNote", L"CrashLog", 1, ini.c_str()) != 0;
    c.testUnverifiedGame = GetPrivateProfileIntW(L"NoteByNote", L"TestUnverifiedGame", 0, ini.c_str()) != 0;
    c.testAutoPassMs = std::max(0, (int)GetPrivateProfileIntW(L"NoteByNote", L"TestAutoPassMs", 0, ini.c_str()));
    c.testPatternsOnly = GetPrivateProfileIntW(L"NoteByNote", L"TestPatternsOnly", 0, ini.c_str()) != 0;  // not in the default ini
    c.testFastIntro = GetPrivateProfileIntW(L"NoteByNote", L"TestFastIntro", 0, ini.c_str()) != 0;        // not in the default ini
    // Layout (defaults from overlay::Settings; sizes kept in the range the mouse allows).
    const overlay::Settings d;
    auto num = [&](const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"NoteByNote", key, def, ini.c_str()); };
    auto pct = [&](const wchar_t* key, int def) { return std::max(50, std::min(250, num(key, def))); };
    c.initial.bannerX = num(L"BannerX", d.bannerX);
    c.initial.bannerY = num(L"BannerY", d.bannerY);
    c.initial.bannerSize = pct(L"BannerSize", d.bannerSize);
    c.initial.clockX = num(L"ClockX", d.clockX);
    c.initial.clockY = num(L"ClockY", d.clockY);
    c.initial.clockSize = pct(L"ClockSize", d.clockSize);
    c.initial.tabX = num(L"TabX", d.tabX);
    c.initial.tabY = num(L"TabY", d.tabY);
    c.initial.tabWidth = std::max(250, num(L"TabWidth", d.tabWidth));
    c.initial.tabSize = pct(L"TabSize", d.tabSize);
    c.initial.mistakeX = num(L"MistakeX", d.mistakeX);
    c.initial.mistakeY = num(L"MistakeY", d.mistakeY);
    c.initial.mistakeSize = pct(L"MistakeSize", d.mistakeSize);
    return c;
}

// Writes the menu-editable settings back (the rest of the file, and its comments, stay as they are).
void SaveSettings(const overlay::Settings& st) {
    const std::wstring ini = IniPath();
    auto put = [&](const wchar_t* key, int v) {
        WritePrivateProfileStringW(L"NoteByNote", key, std::to_wstring(v).c_str(), ini.c_str());
    };
    put(L"Enabled", st.enabled);
    put(L"LeadMs", st.leadMs);
    put(L"EarlyMs", st.earlyMs);
    put(L"LateMs", st.lateMs);
    put(L"CountInBeats", st.countInBeats);
    put(L"AcceptOctaves", st.acceptOctaves);
    put(L"TuningCheck", st.tuningCheck);
    put(L"StringDetect", st.stringDetect);
    put(L"ShowBanner", st.showBanner);
    put(L"BannerFretboard", st.bannerNeck);
    put(L"BannerHand", st.bannerHand);
    put(L"BannerFingers", st.bannerFingers);
    put(L"BannerLayout", st.bannerLayout);
    put(L"BannerAhead", st.bannerAhead);
    put(L"TroubleClearAfter", st.troubleClear);
    put(L"StringsFromThickest", st.stringsFromThick);
    put(L"WaitChords", st.waitChords);
    put(L"SkipGreyedNotes", st.skipGreyed);
    put(L"StopSong", st.stopSong);
    put(L"ShowClock", st.showClock);
    put(L"ShowPracticeBar", st.showPracticeBar);
    put(L"ShowTab", st.showTab);
    put(L"TabBeats", st.tabBeats);
    put(L"TabSeconds", st.tabSeconds);
    put(L"TabSpread", st.tabSpread);
    put(L"TabNoteSize", st.tabNoteSize);
    put(L"TabBackground", st.tabOpacity);
    put(L"TabPages", st.tabPage);
    put(L"TabRows", st.tabRows);
    put(L"TabRepeat", st.tabRecap);
    WritePrivateProfileStringW(L"NoteByNote", L"TabTwoRows", nullptr, ini.c_str());  // replaced by TabRows
    put(L"TabMirror", st.tabMirror);
    put(L"TabThickOnTop", st.tabThickTop);
    put(L"TabRhythm", st.tabRhythm);
    put(L"TabMarks", st.tabMarks);
    put(L"TabPicks", st.tabPicks);
    const std::string themeName = theme::kThemes[st.theme >= 0 && st.theme < theme::kThemeCount ? st.theme : 0].name;
    WritePrivateProfileStringW(L"NoteByNote", L"Theme", std::wstring(themeName.begin(), themeName.end()).c_str(), ini.c_str());
    for (int i = 0; i < theme::kSlots; ++i) {  // own colours as "#RRGGBB"; the theme's = no key
        const std::string key = theme::kSlotInfo[i].key, hex = st.colors[i] >= 0 ? theme::ToHex((uint32_t)st.colors[i]) : "";
        WritePrivateProfileStringW(L"NoteByNote", std::wstring(key.begin(), key.end()).c_str(),
                                   st.colors[i] >= 0 ? std::wstring(hex.begin(), hex.end()).c_str() : nullptr, ini.c_str());
    }
    put(L"SkipUbisoftPopups", st.skipPopups);
    put(L"FastIntro", st.fastIntro);
    put(L"FixGameCrash", st.fixCrash);
    put(L"BannerX", st.bannerX);
    put(L"BannerY", st.bannerY);
    put(L"BannerSize", st.bannerSize);
    put(L"ClockX", st.clockX);
    put(L"ClockY", st.clockY);
    put(L"ClockSize", st.clockSize);
    put(L"TabX", st.tabX);
    put(L"TabY", st.tabY);
    put(L"TabWidth", st.tabWidth);
    put(L"TabSize", st.tabSize);
    put(L"MistakeX", st.mistakeX);
    put(L"MistakeY", st.mistakeY);
    put(L"MistakeSize", st.mistakeSize);
}

std::string Join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

// Friendly note description for the log (the overlay shows the same thing with string colours).
std::string Describe(const Chart& chart, const Target& t) {
    static const char* gtr[] = {"6th string (thickest)", "5th string", "4th string", "3rd string", "2nd string", "1st string (thinnest)"};
    static const char* bass[] = {"4th string (thickest)", "3rd string", "2nd string", "1st string (thinnest)"};
    if (t.chord) {
        std::string s = "chord " + (t.chordName.empty() ? std::string("(no name)") : t.chordName) + " (frets";
        for (int i = 0; i < (chart.bass ? 4 : 6); ++i) s += t.frets[i] < 0 ? " x" : " " + std::to_string(t.frets[i]);
        s += ";";
        for (int m : t.midi) s += " " + MidiName(m);
        return s + ")";
    }
    const char* s = (t.string >= 0 && t.string < (chart.bass ? 4 : 6)) ? (chart.bass ? bass : gtr)[t.string] : "?";
    char buf[128];
    if (t.fret == 0) std::snprintf(buf, sizeof(buf), "%s, open (%s)", s, MidiName(t.midi[0]).c_str());
    else std::snprintf(buf, sizeof(buf), "%s, fret %d (%s)", s, t.fret, MidiName(t.midi[0]).c_str());
    return buf;
}

// Single notes: does this note event match? (chords are matched by the ChordDetector instead)
bool Matches(const overlay::Settings& st, const Chart& chart, const Target& t, int midi) {
    if (t.chord || t.midi.empty()) return false;
    const int d = midi - t.midi[0];
    // bassUnsure: the chart could be a bass line, which sounds one octave lower than guitar notation.
    return d == 0 || (st.acceptOctaves && d % 12 == 0) || (chart.bassUnsure && d == -12);
}

bool Waitable(const overlay::Settings& st, const Target& t) { return !t.ignore && (!t.chord || st.waitChords); }

const std::vector<int> kNoChord;

// ------------------------------------------------------------------ debug recordings of each wait
// Keeps the last 20 s of guitar audio. When a wait ends (hit, pause menu, mode off), the audio from
// 2 s before the freeze until now is saved as NoteByNote_debug\wait_<song time>.wav, so a
// "it didn't accept my note" situation can be replayed and analyzed offline.
class DebugAudio {
public:
    void Push(const std::vector<float>& s) {
        for (float v : s) { ring_[pos_ % kSize] = v; ++pos_; }
    }
    long long Pos() const { return pos_; }
    // Samples [from, to) as doubles; false if they aren't all in the ring (too old, or not yet heard).
    bool Copy(long long from, long long to, std::vector<double>* out) const {
        if (from < 0 || to > pos_ || to <= from || pos_ - from > kSize) return false;
        out->resize((size_t)(to - from));
        for (long long i = from; i < to; ++i) (*out)[(size_t)(i - from)] = ring_[i % kSize];
        return true;
    }
    bool enabled = false;  // setting SaveWaitAudio (off = Save does nothing)
    void Save(const std::wstring& dir, double songTime, long long fromPos, unsigned sr) {
        if (!enabled) return;
        if (pos_ - fromPos > kSize) fromPos = pos_ - kSize;
        if (fromPos < 0) fromPos = 0;
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t name[64];
        swprintf_s(name, L"wait_%07.3f.wav", songTime);
        for (int n = 2; GetFileAttributesW((dir + name).c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
            swprintf_s(name, L"wait_%07.3f_%d.wav", songTime, n);  // the same note waited again: keep both
        FILE* f = _wfopen((dir + name).c_str(), L"wb");
        if (!f) return;
        const uint32_t n = (uint32_t)(pos_ - fromPos), bytes = n * 2;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16);
        std::fwrite("data", 1, 4, f); u32(bytes);
        for (long long i = fromPos; i < pos_; ++i) {
            float v = ring_[i % kSize];
            v = v > 1 ? 1 : (v < -1 ? -1 : v);
            const int16_t s = (int16_t)(v * 32767);
            std::fwrite(&s, 2, 1, f);
        }
        std::fclose(f);
        Log("  (saved the audio of this wait: NoteByNote_debug\\%s)", Narrow(name).c_str());  // no folder path in the log
    }

private:
    static constexpr long long kSize = 48000LL * 20;
    std::vector<float> ring_ = std::vector<float>(kSize, 0.0f);
    long long pos_ = 0;
};

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Report: what was read of a song's chart, and whether Dynamic Difficulty matches it.
void ReportChart(const Chart& chart, const std::string& songKey) {
    size_t notes = 0;
    for (int n : chart.levelCounts) notes += (size_t)n;
    std::vector<int> levels;
    double len = 0;
    const bool dd = game::GetPhraseLevels(&levels);
    game::GetSongLength(&len);
    report::Limited("chart", 4, "  Song %s: %s, %d levels, %zu phrase iterations, %zu notes, %zu beats, capo %d, %.1f s long",
                    songKey.empty() ? "?" : songKey.c_str(), chart.arrangement.c_str(), chart.Levels(), chart.pis.size(),
                    notes, chart.beats.size(), chart.capo, len);
    const std::string ddText = !dd ? "NOT READ"
                               : levels.size() == chart.pis.size() ? "one level per phrase iteration: OK"
                               : "DIFFERENT count (" + std::to_string(levels.size()) + " levels for " +
                                     std::to_string(chart.pis.size()) + " phrase iterations)";
    report::Limited("dd", 4, "  Dynamic Difficulty: %s", ddText.c_str());
}

// A key press edge detector (GetAsyncKeyState polling, only while the game window is focused).
struct KeyEdge {
    bool down = false;
    bool Pressed(int vk) {
        const bool d = GameFocused() && (GetAsyncKeyState(vk) & 0x8000);
        const bool p = d && !down;
        down = d;
        return p;
    }
};

// ------------------------------------------------------------------ main loop
TrackerConfig BassTrackerConfig() {  // bass needs a longer window and a lower lowest note
    TrackerConfig c;
    c.window = 4096;
    c.fmin = 35.0;
    return c;
}

// Everything the main loop keeps between iterations, and its steps: Run() calls them in order about
// every millisecond (0. what the overlay shows, 1. guitar input, 2. keys + menu + settings, 3. game
// screen, 4. chart, 5. menu hold, 6.-8. following the song and waiting at its notes).
struct MainLoop {
    const Config cfg;
    overlay::Settings st;  // the live settings (the menu can change them)

    KeyEdge menuKey, skipKey;
    TapReader tap;
    TrackerConfig guitarCfg, bassCfg = BassTrackerConfig();
    NoteTracker tracker{guitarCfg};
    bool trackerIsBass = false;
    std::vector<float> samples, pending;
    std::vector<NoteEvent> events;
    ChordDetector chordDet;
    std::vector<ChordResult> chordResults;
    std::vector<int> upcomingChord;   // the next chord on the highway (checked after each attack)
    std::vector<overlay::TabNote> tabNotes;  // the scrolling tab's notes (refreshed every 50 ms)
    std::vector<const Target*> tabTargets;
    std::vector<overlay::TabBeat> tabBeats;  // and its bar/beat lines
    std::vector<Beat> tabBeatsRaw;
    std::vector<int> tabLevels;
    DWORD nextTabRefresh = 0;

    std::string menu, lastMenu, lastKey, preMenu, lastPreMenu;
    bool menuOk = false;
    DWORD nextMenuTry = 0;
    Chart chart;
    uintptr_t chartData = 0;          // address of the song data the chart was read from
    bool chartOk = false;             // chart matches the song being played
    bool frozen = false;              // the song is held, waiting for waitFor
    bool menuHold = false;            // the song is held because OUR menu is open
    bool inSong = false, announced = false;
    std::vector<int> levels;
    double cursor = 0;                // song time of the last note that was hit/passed
    double nextWaitT = -1;            // the next note the song would stop at (for the tab's cursor), -1 = none
    Target nextTarget;                // that note (valid while nextWaitT >= 0): the banner shows it early
    double shownT = -1;               // the note the banner shows, and its hand anchor, and the anchor of
    int shownAnchor = 0, handFrom = 0;  // the note shown before it (for "Hand: move UP to fret 7")
    Target shownShape;                // the note the banner shows (its shape: SameShape), and the size of
    int repeatTotal = 1;              // the quick repeat it is part of (RepeatLeft)
    double runStartT = -1;            // and the song time of that repeat's first note (the note's own if none)
    std::vector<overlay::RunNote> runNotes;  // and each of its notes, from that first one (RunNotes)
    std::vector<overlay::AheadStep> aheadSteps;  // what comes after the banner's note (RefreshAhead), worked out
    double aheadFor = -1;             // for the note at this song time, again every 50 ms (the levels and
    DWORD nextAheadRefresh = 0;       // the practice parts can change under it)
    double lastT = -1;
    double peakT = -1;                // the furthest song time since the cursor was last synced
    bool rewinding = false;           // the song is going back (a Riff Repeater loop starting over)
    double loopStart = -1, loopEnd = -1;  // Riff Repeater's loop (-1 = none)
    DWORD rangesLogTick = 0;          // when to log the practice parts (0 = logged)
    double greyT = -1;                // notes before this are greyed out and not waited for (-1 = none)
    double unplayedT = -1;            // the note the song was waiting at when the pause screen opened: never
                                      // passed as greyed out (-1 = none)
    std::vector<overlay::Range> ranges;  // the practice parts (the practice bar): waits only inside; empty = all
    Target waitFor;                   // the note we're frozen on
    double frozenT = 0;               // the song time when it was held (the hold watchdog compares with it)
    DWORD countInEnd = 0;             // the count-in after a long wait: when it ends (0 = none), and one
    DWORD countInBeat = 0;            // beat of the song's tempo (ms)
    int holdRetries = 0;              // times the hold was re-applied during this wait
    hint::Line waitHint;              // how to fix the last wrong note played during this wait
    std::vector<hint::Mark> waitMarks;  // and where it was probably played (the banner's fretboard)
    bool waitWrong = false;           // a wrong note was played during this wait (its audio is kept)

    // String identification (stringid.h): which string a wrong note was played on, from its sound.
    stringid::Calibration stringCal;  // the open strings' sound (ini StringCalibration; the menu redoes it)
    struct PendingId {                // a pluck whose sound is being collected (kLength after its attack)
        bool on = false;
        bool forCalibration = false;  // a calibration pluck; else a wrong note while the song waits
        long long from = 0, to = 0;   // audio positions (DebugAudio::Pos units)
        double f0 = 0;                // the tracker's frequency
        int midi = 0;
        double waitT = -1;            // wrong note: the wait it belongs to (no answer once that wait is over)
        hint::Line guessHint;         // wrong note: the advice and marks without knowing the string, shown
        std::vector<hint::Mark> guessMarks;  // only if the sound can't tell (no flash of a guess first)
    } pendingId;
    std::vector<long long> eventPos;  // audio position (end of its block) of each of this loop's note events
    // Calibration in progress: the open string being plucked (0 = thickest, -1 = not calibrating), the
    // measures of its plucks so far, the new calibration, and the last message for the menu.
    static constexpr int kCalPlucks = 3;
    int calString = -1;
    std::vector<double> calValues;
    int calMidi = 0;
    stringid::Calibration calNew;
    std::string stringIdNote;
    bool stringIdNoteWarn = false;

    DWORD frozenTick = 0, lastTapTry = 0, nextFreezeTry = 0, lastHeartbeat = 0, lastUnloadCheck = 0, nextChartTry = 0,
          songScreenTick = 0;
    // Report: does the song clock run with the music? Checked once per song, over 1 s with the song
    // not held.
    bool clockChecked = false;
    DWORD clockTick = 0;
    int clockStill = 0;  // seconds the clock stood still (the song has not started yet)
    double clockT = 0;
    long long totalSamples = 0;
    DebugAudio debugAudio;
    long long waitAudioStart = 0;
    const std::wstring debugDir = DllDir() + L"NoteByNote_debug\\";
    // Trouble spots (stats.h): how each note went, per song; the practice bar shades its phrases by it.
    // In the game's folder, wherever the DLL is: a copy loaded by hand (from run\) keeps the same records.
    const std::wstring statsDir = GameDir() + L"NoteByNote_stats\\";
    stats::SongStats songStats;
    std::vector<float> phraseHeat;    // per phrase iteration (chart.pis), from songStats
    std::vector<std::pair<int, int>> phraseCleared;  // and its notes cleared, of those that went wrong
    std::string runSummary;           // this time in the song, for the menu
    bool statsChanged = true;         // phraseHeat / runSummary must be worked out again
    std::map<int, int> noteMarks;     // how each note went this time (ms -> TabNote::mark), for the tab
    std::set<int> clearedNow;         // notes cleared this time (ms): the tab keeps their dots, all filled
    // Tuning check (tuning.h): the steady pitch of the last note heard, and the note the song asked for
    // then (set by the wait logic: a waited note, or one played on time; string -1 = none, not counted).
    tuning::SteadyPitch steady;
    int steadyString = -1, steadyWanted = 0;
    tuning::Check tuneCheck;
    tuning::Finding tuneFound;        // what it says now (string -1 and !all = in tune, or not known)
    Target held;                      // the last note played that rings at least overlay::kHoldMinS (the
    bool holding = false;             // banner counts its sustain down: "Keep holding fret 9 ... 540 ms")
    DWORD lastHeardTick = 0;          // when the guitar last gave a note (a miss counts only while playing)

    explicit MainLoop(const Config& c) : cfg(c), st(c.initial) {
        debugAudio.enabled = cfg.saveWaitAudio;
        stringCal = stringid::Calibration::FromString(cfg.stringCalibration);
        Log("string id: %s", stringCal.Complete() ? ("calibrated " + stringCal.ToString()).c_str() : "not calibrated");
    }

    // Runs until the dev unload file appears; then releases the song if we're holding it.
    void Run() {
        for (;;) {
            Sleep(1);
            const DWORD now = GetTickCount();
            game::Tick();
            PublishView(now);  // 0. (state of the previous iteration; 1 ms old is fine)
            nextWaitT = -1;    // set again in Follow() while the mode can stop the song at a next note
            ReadGuitar(now);   // 1.
            const bool skip = ReadKeys();  // 2.
            ApplySettings();
            HandleCalibrationRequest();
            if (overlay::TakeForgetRequest()) {
                songStats.Forget();
                statsChanged = true;
                Log("trouble spots: this song's record forgotten (menu)");
            }
            if (UnloadRequested(now)) break;
            if (!UpdateScreen(now)) continue;  // 3. not (yet) in a song
            ReadChart(now);    // 4.
            CheckClock(now);
            HoldForMenu(now);  // 5.
            // Right after the song screen opens (song start, Riff Repeater, unpausing) the clock still
            // reports the old time for a moment: acting on it froze the song too early. Let it settle.
            if (now - songScreenTick < 400) continue;
            Follow(now, skip);  // 6.-8.
        }
        if (frozen || menuHold) game::Unfreeze();
        songStats.Save();
    }

    // Stops waiting for the current note. If our menu is open, the song stays held until it closes.
    void ReleaseWait() {
        if (!frozen) return;
        frozen = false;
        countInEnd = 0;
        if (overlay::MenuOpen()) menuHold = true;
        else game::Unfreeze();
    }

    // The guitar audio from 2 s before the current wait until now (setting SaveWaitAudio).
    void SaveWaitAudio() { debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000); }

    // A single note and the notes linked after it: while one is marked "parent", the next note on the
    // same string (within a few seconds) follows without picking again. At most 4 notes.
    std::vector<technique::Link> LinkedChain(const Target& first) const {
        std::vector<technique::Link> out{{first.tech, first.fret}};
        const Target* cur = &first;
        while ((cur->tech.mask & technique::kParent) && out.size() < 4) {
            const Target* next = nullptr;
            double after = cur->time;
            for (int i = 0; i < 8 && !next; ++i) {  // skip notes on other strings in between
                const Target* c = chart.NextTarget(after, levels);
                if (!c || c->time > cur->time + 4) break;
                if (!c->chord && c->string == cur->string) next = c;
                after = c->time;
            }
            if (!next) break;
            out.push_back({next->tech, next->fret});
            cur = next;
        }
        return out;
    }

    // A quick repeat of one note or chord: the same shape again (SameShape), each within kRepeatGap of the one
    // before (quarter notes at 110 bpm or faster: the chart's times wobble a ms or two, 0.5 s missed some
    // at 120 bpm). The banner shows "x5" and counts down as they're played, since its text alone doesn't
    // change from one to the next.
    static constexpr double kRepeatGap = 0.55;

    // The same thing to play: a single note on the same string and fret, or a chord (or double stop) with
    // the same fret on every string (a power chord strummed again and again).
    static bool SameShape(const Target& a, const Target& b) {
        if (a.chord != b.chord) return false;
        if (!a.chord) return a.string == b.string && a.fret == b.fret;
        return std::equal(std::begin(a.frets), std::end(a.frets), std::begin(b.frets));
    }

    // How many of the run starting at `first` are still to play, counting it (1 = no repeat). Only the
    // notes the mode waits for count (a greyed-out or ignored one ends the run). last: the run's last note.
    int RepeatLeft(const Target& first, const Target** last = nullptr) const {
        int n = 1;
        const Target* cur = &first;
        for (; n < 99; ++n) {
            const Target* c = chart.NextTarget(cur->time, levels);
            if (!c || !SameShape(*c, first) || c->time - cur->time > kRepeatGap || !CanWait(*c))
                break;
            cur = c;
        }
        if (last) *last = cur;
        return n;
    }

    // Each note of the quick repeat starting at `first` (the run RepeatLeft counts): how it's played.
    void RunNotes(const Target& first, std::vector<overlay::RunNote>* out) const {
        out->clear();
        const Target* cur = &first;
        while (out->size() < 99) {
            // A slide's direction: where it goes, from the note's fret (a chord's: the fret its technique refers to).
            const int to = (cur->tech.mask & technique::kSlide) ? cur->tech.slideTo
                         : (cur->tech.mask & technique::kUnpitchedSlide) ? cur->tech.slideUnpitchTo : -1;
            const int from = cur->chord ? cur->techFret : cur->fret;
            out->push_back({cur->tech.mask, cur->pick, (to < 0 || from < 0) ? 0 : to > from ? 1 : to < from ? -1 : 0});
            const Target* c = chart.NextTarget(cur->time, levels);
            if (!c || !SameShape(*c, first) || c->time - cur->time > kRepeatGap || !CanWait(*c)) break;
            cur = c;
        }
    }

    // What comes after the banner's note (setting bannerAhead), for its "Then" row and its cards: the next
    // notes and chords the mode waits for, in order, a quick repeat as one step. Worked out when the
    // banner's note changes and again every 50 ms.
    void RefreshAhead(const Target& note, DWORD now) {
        if (note.time == aheadFor && now < nextAheadRefresh) return;
        aheadFor = note.time;
        nextAheadRefresh = now + 50;
        aheadSteps.clear();
        const Target* cur = &note;
        RepeatLeft(note, &cur);  // past the banner's own repeat
        for (int guard = 0; (int)aheadSteps.size() < st.bannerAhead && guard < 200; ++guard) {
            const Target* c = chart.NextTarget(cur->time, levels);
            if (!c) break;
            cur = c;
            if (!CanWait(*c)) continue;  // not waited for: the banner never shows it
            overlay::AheadStep a;
            a.chord = c->chord;
            a.string = c->string;
            a.fret = c->fret;
            a.chordName = c->chordName;
            std::copy(std::begin(c->frets), std::end(c->frets), a.frets);
            std::copy(std::begin(c->fingers), std::end(c->fingers), a.fingers);
            a.tech = c->tech;
            if (c->chord) std::copy(std::begin(c->strings), std::end(c->strings), a.strings);
            a.anchorFret = c->anchorFret;
            a.anchorWidth = c->anchorWidth;
            a.pick = c->pick;
            a.time = c->time;
            RunNotes(*c, &a.run);
            a.count = RepeatLeft(*c, &cur);
            aheadSteps.push_back(std::move(a));
        }
    }

    // A note went one way or another (stats.h): kept for the trouble spots.
    void RecordNote(double t, stats::Result r, double waitS = 0, bool wrongNote = false) {
        if (songStats.Record(t, r, waitS, wrongNote, st.troubleClear)) {
            Log("trouble spots: %.3f cleared (%d times on time in a row)", t, st.troubleClear);
            clearedNow.insert((int)std::lround(t * 1000.0));
        }
        // (a note that never went wrong, now played right that many times: its dots stay, all filled, this time)
        if (r == stats::Result::kOnTime && songStats.Streak(t) == st.troubleClear) clearedNow.insert((int)std::lround(t * 1000.0));
        statsChanged = true;
        noteMarks[(int)std::lround(t * 1000.0)] = r == stats::Result::kOnTime ? 1 : r == stats::Result::kWaited ? 2 : 3;
    }

    // A note was played: if it rings long enough, the banner counts its sustain down.
    void StartHold(const Target& x) {
        if (!LongNote(x)) return;
        held = x;
        holding = true;
    }

    // The song went back (a loop starting over, a rewind): the notes from there on are to be played again.
    void ForgetMarksFrom(double t) {
        noteMarks.erase(noteMarks.lower_bound((int)std::lround(t * 1000.0)), noteMarks.end());
        if (holding && held.time >= t) holding = false;
    }

    // The trouble per phrase (the practice bar's red) and the menu's "this time" line, worked out again
    // only after something changed.
    void UpdateTroubleSpots() {
        if (!statsChanged) return;
        statsChanged = false;
        std::vector<std::pair<double, double>> phrases;
        if (chartOk)
            for (const auto& p : chart.pis) phrases.emplace_back(p.start, p.end);
        phraseHeat.clear();
        phraseCleared.clear();
        for (const auto& sp : songStats.Spots(phrases, st.troubleClear)) {
            phraseHeat.push_back(sp.heat);
            phraseCleared.emplace_back(sp.cleared, sp.troubled);
        }
        const auto& r = songStats.ThisRun();
        runSummary.clear();
        if (r.stops + r.skips + r.onTime + r.missed == 0) return;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d played on time, %d waited for, %d skipped", r.onTime, r.stops, r.skips);
        runSummary = buf;
        if (r.missed) runSummary += ", " + std::to_string(r.missed) + " not played";
        if (r.cleared) runSummary += "; " + std::to_string(r.cleared) + (r.cleared == 1 ? " note" : " notes") + " cleared";
        if (r.longestAt >= 0) {
            const int at = (int)r.longestAt;
            std::snprintf(buf, sizeof(buf), "; the longest wait: %.1f s at %d:%02d", r.longestWait, at / 60, at % 60);
            runSummary += buf;
        }
    }

    // ---- 0. what the overlay shows
    void PublishView(DWORD now) {
        overlay::View v;
        v.inSong = inSong;
        // The game's pause screen and its Riff Repeater screen: the practice bar works there too, on the
        // game's own bar (the parts can be chosen with the song stopped, beside Riff Repeater's selection).
        auto endsWith = [&](const char* tail) {
            const size_t n = std::char_traits<char>::length(tail);
            return menu.size() >= n && menu.compare(menu.size() - n, n, tail) == 0;
        };
        v.songMenu = !inSong && menuOk && chartOk && (endsWith("_Pause") || endsWith("_RiffRepeater"));
        v.waiting = frozen;
        v.waitTime = frozen ? waitFor.time : -1;
        v.nextWaitTime = frozen ? -1 : nextWaitT;
        {  // the tab dims greyed-out notes like the highway does (only when they aren't waited for)
            double g = -1, ts = -1;
            if (st.skipGreyed && inSong && game::GetGreyTime(&g) && game::GetSongTime(&ts) && g > ts && g < ts + 20) v.greyTime = g;
        }
        v.bass = chart.bass;
        // The banner's note: the one waited for, or while the song plays towards the next stop, that
        // one (so between fast notes the banner changes its text instead of disappearing).
        v.upcoming = !frozen && nextWaitT >= 0;
        const Target& note = v.upcoming ? nextTarget : waitFor;
        v.string = note.string;
        v.fret = note.fret;
        v.chord = note.chord;
        v.chordName = note.chordName;
        std::copy(std::begin(note.frets), std::end(note.frets), v.frets);
        std::copy(std::begin(note.notes), std::end(note.notes), v.notes);
        v.midi = (!note.chord && !note.midi.empty()) ? note.midi[0] : -1;
        v.sustain = LongNote(note) ? note.sustain : 0;  // (the banner's "Hold" only for a long one)
        v.pick = note.pick;
        v.inShape = !note.chord && note.shapeEnd > 0;
        v.shapeName = v.inShape ? note.shapeName : "";
        std::copy(std::begin(note.shapeFrets), std::end(note.shapeFrets), v.shapeFrets);
        if (holding) {
            v.holdFrom = held.time;
            v.holdLen = held.sustain;
            v.holdFret = held.fret;
            v.holdString = held.string;
            v.holdName = held.chord ? (held.chordName.empty() ? std::string("the chord") : held.chordName) : "";
        }
        v.tech = note.tech;
        v.techFret = note.techFret;
        for (int s = 0; s < 6; ++s) v.strings[s] = note.chord ? note.strings[s] : technique::Technique{};
        v.anchorFret = note.anchorFret;
        v.anchorWidth = note.anchorWidth;
        std::copy(std::begin(note.fingers), std::end(note.fingers), v.fingers);
        // A quick repeat: the run's size is taken when it starts (its first note shown), and kept while
        // the banner moves along it.
        const int left = RepeatLeft(note);
        // The hand moves from the anchor of the note shown before this one.
        if (note.time != shownT) {
            const bool sameRun = shownT >= 0 && SameShape(note, shownShape) && note.time > shownT &&
                                 note.time - shownT <= kRepeatGap;
            repeatTotal = sameRun ? std::max(repeatTotal, left) : left;
            if (!sameRun) {
                runStartT = note.time;
                RunNotes(note, &runNotes);
            }
            shownShape = note;
            handFrom = shownAnchor;
            shownT = note.time;
            shownAnchor = note.anchorFret;
        }
        v.handFrom = handFrom;
        repeatTotal = std::max(repeatTotal, left);
        v.repeatLeft = left;
        v.repeatTotal = repeatTotal;
        v.stepTime = runStartT;
        v.run = runNotes;
        if (chartOk && st.bannerAhead > 0 && (frozen || v.upcoming)) {
            RefreshAhead(note, now);
            v.ahead = aheadSteps;
        } else {
            aheadFor = -1;
        }
        v.phraseStarts.clear();
        if (chartOk)
            for (const auto& p : chart.pis) v.phraseStarts.push_back(p.start);
        UpdateTroubleSpots();
        v.phraseHeat = phraseHeat;
        v.phraseCleared = phraseCleared;
        v.runSummary = runSummary;
        // The count-in's number: beats left (3, 2, 1).
        v.countIn = (frozen && countInEnd && countInBeat) ? (int)((countInEnd - std::min(countInEnd, now) + countInBeat - 1) / countInBeat) : 0;
        if (!note.chord) v.chain = LinkedChain(note);
        if (frozen) {
            v.hint = waitHint;
            v.heardAt = waitMarks;
        }
        StringIdStatus(&v);
        // The clock works even with the mode off or without a chart (it's just the song time).
        const bool songBar = inSong || v.songMenu;
        if (!songBar || !game::GetSongTime(&v.songTime)) v.songTime = -1;
        if (songBar && !game::GetSongLength(&v.songLength)) v.songLength = 0;
        // The song's sections (the tab, the clock, the practice bar and page name them).
        v.sections.clear();
        v.section.clear();
        if (chartOk && songBar)
            for (const auto& sec : chart.sections) {
                v.sections.push_back({sec.start, sec.name});
                if (v.songTime >= sec.start - 0.05 && v.songTime < sec.end) v.section = sec.name;
            }
        // The scrolling tab (works with the mode off too).
        if (inSong && chartOk && st.showTab && v.songTime >= 0) {
            if (now >= nextTabRefresh) RefreshTab(v.songTime, now);
            v.tab = tabNotes;
            v.tabBeats = tabBeats;
        } else {
            tabNotes.clear();
            tabBeats.clear();
            nextTabRefresh = 0;
        }
        v.chartOk = chartOk;
        if (chartOk) v.chartInfo = "Song notes read from the game (" + chart.arrangement + ", " + std::to_string(chart.Levels()) + " levels)";
        else if (inSong) v.chartInfo = "Couldn't read this song's notes yet: it plays normally";
        else v.chartInfo = "Start a song to use Note-by-Note";
        overlay::SetView(v);
    }

    // The tab's notes: the ones the highway shows from 1 s ago to the end of the tab (+1 s margin, the
    // list is only refreshed every 50 ms).
    void RefreshTab(double songTime, DWORD now) {
        nextTabRefresh = now + 50;
        if (!game::GetPhraseLevels(&tabLevels)) tabLevels.clear();
        // The past part: a page (tab pages mode) can show up to ~90 % of a page behind the cursor.
        const double back = st.tabPage ? st.tabSeconds * 1.3 + 1.0 : 1.0;
        // The future part: several rows also show the next pages (each ~1.1 tabs long, a little more
        // with a big recap).
        const int rows = st.tabPage ? std::max(1, std::min(4, st.tabRows)) : 1;
        const double ahead = (rows > 1 ? st.tabSeconds * 1.2 * rows + st.tabSeconds * rows * st.tabRecap / 100.0
                                       : st.tabSeconds) + 1.0;
        chart.TargetsBetween(songTime - back, songTime + ahead, tabLevels, &tabTargets);
        chart.BeatsBetween(songTime - back, songTime + ahead, &tabBeatsRaw);
        tabBeats.clear();  // always sent: the rhythm needs them even with the lines off
        for (const Beat& b : tabBeatsRaw) tabBeats.push_back({b.time, b.measure, b.downbeat});
        tabNotes.clear();
        for (const Target* t : tabTargets) {
            overlay::TabNote tn;
            tn.time = t->time;
            tn.chord = t->chord;
            tn.ignore = t->ignore;
            tn.pick = t->pick;
            tn.pickFromSong = chart.picksFromSong;
            if (!t->chord && t->shapeEnd > 0) {
                tn.shapeName = t->shapeName;
                tn.shapeStart = t->shapeStart;
                tn.shapeEnd = t->shapeEnd;
            }
            if (t->chord) {
                std::copy(std::begin(t->frets), std::end(t->frets), tn.frets);
                for (int s = 0; s < 6; ++s) {  // each string's technique, plus the chord's own palm mute / mute / accent
                    tn.tech[s] = t->strings[s];
                    tn.tech[s].mask |= t->tech.mask & (technique::kPalmMute | technique::kAccent);
                    if (t->tech.mask & technique::kChordMute) tn.tech[s].mask |= technique::kMute;
                }
            }
            else if (t->string >= 0 && t->string < 6) {
                tn.frets[t->string] = t->fret;
                tn.tech[t->string] = t->tech;
            }
            tn.name = t->chordName;
            tn.sustain = t->sustain;
            if (st.tabMarks) {
                const auto m = noteMarks.find((int)std::lround(t->time * 1000.0));
                if (m != noteMarks.end()) tn.mark = m->second;
            }
            // Progress dots: every note the mode waits for in a trouble spot (a red phrase), and any note that
            // went wrong until it's cleared (and this time, after): its good tries in a row so far. (Only the
            // notes that went wrong had them at first; the user missed them on the rest of the spot.)
            // And every note the mode waits for that hasn't been played right that many times in a row yet,
            // so a song played for the first time has its (empty) dots from the first note on. (They used to
            // appear only after the first note the song had to wait for: "not activated until a bit late".)
            int streak = 0;
            const int ms = (int)std::lround(t->time * 1000.0);
            const bool troubled = songStats.Progress(t->time, &streak);
            const bool inSpot = t->pi >= 0 && t->pi < (int)phraseHeat.size() && phraseHeat[t->pi] > 0 && CanWait(*t);
            const bool toLearn = CanWait(*t) && (songStats.Streak(t->time) < st.troubleClear || clearedNow.count(ms));
            if ((troubled && (streak < st.troubleClear || clearedNow.count(ms))) || inSpot || toLearn) {
                tn.streak = troubled ? streak : songStats.Streak(t->time);
                tn.need = st.troubleClear;
            }
            tabNotes.push_back(tn);
        }
    }

    // ---- 1. guitar -> note events (and chord results)
    void ReadGuitar(DWORD now) {
        if (!tap.IsOpen() && now - lastTapTry > 1000) {
            lastTapTry = now;
            if (tap.Open()) Log("guitar input connected (GuitarTap, %u Hz)", tap.SampleRate());
        }
        events.clear();
        eventPos.clear();
        chordResults.clear();
        samples.clear();
        tap.ReadNew(samples);
        totalSamples += (long long)samples.size();
        debugAudio.Push(samples);
        pending.insert(pending.end(), samples.begin(), samples.end());
        // The chord to check: the one we're waiting for, or the next one on the highway (early hits).
        const std::vector<int>& expectChord = frozen ? (waitFor.chord ? waitFor.midi : kNoChord) : upcomingChord;
        const long long base = debugAudio.Pos() - (long long)pending.size();  // audio position of pending[0]
        size_t used = 0;
        for (; used + NoteTracker::kBlock <= pending.size(); used += NoteTracker::kBlock) {
            NoteEvent ev;
            const bool got = tracker.Process(&pending[used], &ev);
            if (got) {
                events.push_back(ev);
                eventPos.push_back(base + (long long)(used + NoteTracker::kBlock));
            }
            // The tuning check: a note's steady pitch (a new note ends the one before).
            double pitch = 0;
            if (got ? steady.Stop(&pitch) : steady.Frame(tracker.FramePitch(), &pitch)) TuneNote(pitch);
            if (got) {
                steady.Start(ev.midi);
                steadyString = -1;  // (until the wait logic says which note it was meant to be)
            }
            ChordResult cr;
            if (chordDet.Process(&pending[used], expectChord, &cr)) chordResults.push_back(cr);
        }
        pending.erase(pending.begin(), pending.begin() + used);
        if (!events.empty()) lastHeardTick = now;
        StringIdAudio();
    }

    // ---- 1b. string identification: collect the sound after a pick attack, then measure it
    // (stringid.h). While calibrating, the plucks are for the calibration, not for the song.
    void StringIdAudio() {
        for (size_t i = 0; i < events.size(); ++i)  // a new attack ends the sound of the note before
            if (events[i].attack && pendingId.on && eventPos[i] > pendingId.from)
                pendingId.to = std::min(pendingId.to, eventPos[i] - (long long)(0.01 * stringid::kSr));
        if (pendingId.on && debugAudio.Pos() >= pendingId.to) FinishStringId();
        if (calString < 0) return;
        for (size_t i = 0; i < events.size(); ++i) {
            const NoteEvent& ev = events[i];
            if (!ev.attack) continue;
            if (std::abs(ev.midi - CalOpenMidi(calString)) > 2) {  // +-2: a guitar tuned down still calibrates
                stringIdNote = "Heard " + MidiName(ev.midi) + ", that's not " + StringName(calString) + " open";
                stringIdNoteWarn = true;
                continue;
            }
            StartStringId(ev, eventPos[i], true);
        }
        events.clear();
        chordResults.clear();
    }

    // The open string the calibration asks for: the song's tuning when a guitar part is loaded, else standard.
    int CalOpenMidi(int s) const {
        static const int kStd[6] = {40, 45, 50, 55, 59, 64};
        return (chartOk && !chart.bass) ? chart.open[s] : kStd[s];
    }

    // "string 4 (D)", numbered like the banner does (menu text, no colour).
    std::string StringName(int s) const {
        static const char* kLetter[6] = {"E", "A", "D", "G", "B", "e"};
        return "string " + std::to_string(st.stringsFromThick ? s + 1 : 6 - s) + " (" + kLetter[s] + ")";
    }

    // Can a wrong note's string be identified now? (setting on, a clean calibration, a guitar part)
    bool StringIdReady() const {
        return st.stringDetect && calString < 0 && stringCal.Complete() && stringid::CheckCalibration(stringCal).empty() &&
               !(chartOk && chart.bass);
    }

    void StartStringId(const NoteEvent& ev, long long pos, bool forCalibration) {
        pendingId.on = true;
        pendingId.forCalibration = forCalibration;
        pendingId.from = pos + (long long)(stringid::kStartAfter * stringid::kSr);
        pendingId.to = pendingId.from + (long long)(stringid::kLength * stringid::kSr);
        pendingId.f0 = ev.freq;
        pendingId.midi = ev.midi;
        pendingId.waitT = frozen ? waitFor.time : -1;
        pendingId.guessHint.clear();
        pendingId.guessMarks.clear();
    }

    void FinishStringId() {
        pendingId.on = false;
        std::vector<double> x;
        if (pendingId.to - pendingId.from < (long long)(stringid::kMinLength * stringid::kSr) ||
            !debugAudio.Copy(pendingId.from, pendingId.to, &x)) {
            if (pendingId.forCalibration && calString >= 0) {
                stringIdNote = "Too short: let each pluck ring for a moment";
                stringIdNoteWarn = true;
            }
            if (!pendingId.forCalibration) ShowGuess();  // the next note came too soon to tell
            return;
        }
        const stringid::Measure m = stringid::Analyze(x.data(), (int)x.size(), pendingId.f0);
        if (pendingId.forCalibration) CalibrationPluck(m);
        else WrongNoteSpot(m);
    }

    // A wrong note whose string the sound couldn't tell: the advice without knowing it (HeardWaitedNote's guess).
    void ShowGuess() {
        if (!frozen || waitFor.time != pendingId.waitT || pendingId.guessHint.empty()) return;
        waitHint = pendingId.guessHint;
        waitMarks = pendingId.guessMarks;
        Log("  advice: %s", hint::Text(waitHint).c_str());
    }

    // After a wrong note: if its sound says where it was played, the advice and the red X are for that
    // spot only; else the guess with faint marks on the other spots with the same pitch.
    void WrongNoteSpot(const stringid::Measure& m) {
        if (!frozen || waitFor.time != pendingId.waitT || waitFor.chord || waitFor.midi.empty()) return;
        if (!m.ok) {
            Log("  string id: %s - %s (%.2f s of sound at %.1f dB, %d overtones, highest %d)", MidiName(pendingId.midi).c_str(),
                m.unstretched ? "overtones not stretched (a harmonic, several strings ringing, or a moving pitch)"
                              : "too few overtones to tell the string",
                (pendingId.to - pendingId.from) / (double)stringid::kSr, m.levelDb, m.partials, m.maxK);
            ShowGuess();
            return;
        }
        std::vector<std::pair<int, int>> cands;  // (string, sounding fret) for the heard pitch
        for (int s = 0; s < 6; ++s) {
            const int sf = pendingId.midi - chart.open[s];
            if (sf >= chart.capo && sf <= 24) cands.push_back({s, sf});
        }
        // The hand is at the note the song waits for (its sounding fret: an open string with a capo = the capo).
        const int handFret = (waitFor.fret == 0 && chart.capo > 0) ? chart.capo : waitFor.fret;
        const bool weak = m.maxK < stringid::kWeakTopK;
        const stringid::Guess g = stringid::Identify(stringCal, chart.open, m.logB, cands, waitFor.string, handFret, weak);
        Log("  string id: %s log10 B %.2f%s -> %s fret %d (off by %.2f, next best %.2f further): %s", MidiName(pendingId.midi).c_str(),
            m.logB, weak ? " (weak: overtones up to 7 only)" : "", g.string >= 0 ? StringName(g.string).c_str() : "?", g.fret, g.dist, g.margin,
            !g.sure ? "not sure" : g.byHand ? "sure (the sound fits several spots; the one near the hand)" : "sure");
        hint::Mark at;
        const hint::Line l = g.sure ? hint::ForNotePlayedOn(MakeNeck(), waitFor.string, waitFor.fret, waitFor.midi[0], pendingId.midi, g.string, &at)
                                    : hint::Line{};
        if (l.empty() || at.string < 0) {
            ShowGuess();
            return;
        }
        waitHint = l;
        waitMarks = {at};
        Log("  advice: %s", hint::Text(waitHint).c_str());
    }

    // One calibration pluck measured: kCalPlucks per string, thickest first; then check and save.
    void CalibrationPluck(const stringid::Measure& m) {
        if (calString < 0) return;
        if (!m.ok) {
            stringIdNote = "Couldn't measure that pluck: pluck again and let it ring";
            stringIdNoteWarn = true;
            return;
        }
        Log("string id: calibration %s open (%s): log10 B %.3f, %d overtones", StringName(calString).c_str(),
            MidiName(pendingId.midi).c_str(), m.logB, m.partials);
        stringIdNote.clear();
        calValues.push_back(m.logB);
        calMidi = pendingId.midi;
        if ((int)calValues.size() < kCalPlucks) return;
        std::sort(calValues.begin(), calValues.end());
        calNew.has[calString] = true;
        calNew.midi[calString] = calMidi;
        calNew.logB[calString] = calValues[calValues.size() / 2];  // the median
        calValues.clear();
        if (++calString < 6) return;
        calString = -1;
        const std::string why = stringid::CheckCalibration(calNew);
        Log("string id: calibration %s: %s", calNew.ToString().c_str(), why.empty() ? "clean, saved" : why.c_str());
        if (!why.empty()) {
            stringIdNote = "Not saved: " + why + ".";
            stringIdNoteWarn = true;
            return;
        }
        stringCal = calNew;
        const std::string text = stringCal.ToString();
        WritePrivateProfileStringW(L"NoteByNote", L"StringCalibration", std::wstring(text.begin(), text.end()).c_str(), IniPath().c_str());
        stringIdNote = "Calibrated. After a wrong note, the banner shows the spot you played when the sound is clear enough.";
        stringIdNoteWarn = false;
        overlay::Toast("String detection calibrated", 2000);
    }

    // The menu's Calibrate / Cancel buttons.
    void HandleCalibrationRequest() {
        const int r = overlay::TakeCalibrationRequest();
        if (r == 1) {
            calString = 0;
            calValues.clear();
            calNew = {};
            pendingId.on = false;
            stringIdNote.clear();
            Log("string id: calibration started");
        } else if (calString >= 0 && (r == 2 || !overlay::MenuOpen())) {
            // Cancel, or the menu was closed: the plucks must go back to the song (it would never hear
            // the note it waits for).
            calString = -1;
            pendingId.on = false;
            stringIdNote = "Calibration cancelled (the previous one is kept).";
            stringIdNoteWarn = false;
            Log("string id: calibration cancelled");
        }
    }

    // The menu's line about the string identification.
    void StringIdStatus(overlay::View* v) const {
        v->calibrating = calString >= 0;
        v->calibrated = stringCal.Complete();
        v->stringIdWarn = false;
        if (calString >= 0) {
            v->stringIdStatus = "Pluck " + StringName(calString) + " open and let it ring  (" + std::to_string(calValues.size() + 1) +
                                " of " + std::to_string(kCalPlucks) + ")";
            if (!stringIdNote.empty()) v->stringIdStatus += "\n" + stringIdNote;
            v->stringIdWarn = stringIdNoteWarn;
        } else if (!stringIdNote.empty()) {
            v->stringIdStatus = stringIdNote;
            v->stringIdWarn = stringIdNoteWarn;
        } else if (!stringCal.Complete()) {
            v->stringIdStatus = "Not calibrated yet: press Calibrate, then pluck each open string 3 times.";
            v->stringIdWarn = true;
        } else if (!stringid::CheckCalibration(stringCal).empty()) {
            v->stringIdStatus = "The saved calibration sounds processed: calibrate again with a clean sound.";
            v->stringIdWarn = true;
        } else if (chartOk && chart.bass) {
            v->stringIdStatus = "Guitar only (this song part is for bass).";
        } else {
            v->stringIdStatus = "Calibrated. After a wrong note, the banner shows the spot you played when the sound is clear enough.";
        }
    }

    // ---- 2. keys and the menu. Returns true when the player asked to skip the note (key or menu).
    bool ReadKeys() {
        if (menuKey.Pressed(cfg.menuKey)) {
            overlay::ToggleMenu();
            game::PostUiEvent(cfg.menuSound.c_str());
            Log("menu %s", overlay::MenuOpen() ? "opened" : "closed");
        }
        // Both are called every loop (not `a || b`): Pressed() tracks the key's up/down edge and
        // TakeSkipRequest() clears the menu's request, so neither may be skipped.
        const bool keySkip = skipKey.Pressed(cfg.skipKey);
        const bool menuSkip = overlay::TakeSkipRequest();
        return keySkip || menuSkip;
    }

    // Settings changed in the menu: act on the ones that matter right now, and save them.
    void ApplySettings() {
        const overlay::Settings newSt = overlay::GetSettings();
        if (newSt == st) return;
        // The mode (the menu's Off / Show the notes / Wait for each note): one message for it.
        if (newSt.enabled != st.enabled || (newSt.enabled && newSt.stopSong != st.stopSong)) {
            const char* mode = !newSt.enabled ? "Off" : newSt.stopSong ? "Wait for each note" : "Show the notes (the song plays on)";
            Log("Note-by-Note: %s", mode);
            overlay::Toast(std::string("Note-by-Note: ") + mode);
        }
        if (newSt.enabled != st.enabled) {
            if (!newSt.enabled && frozen) {
                SaveWaitAudio();
                ReleaseWait();
            }
            if (newSt.enabled) lastT = -1;  // re-sync to the current position
        }
        if (newSt.stopSong != st.stopSong) {
            if (!newSt.stopSong && frozen) {  // switched off while waiting: the song goes on from this note
                cursor = waitFor.time;
                ReleaseWait();
            }
        }
        if (!newSt.waitChords && frozen && waitFor.chord) {  // chord waits switched off while waiting at one
            cursor = waitFor.time;
            ReleaseWait();
        }
        if (newSt.troubleClear != st.troubleClear) statsChanged = true;
        st = newSt;
        SaveSettings(st);
        Log("settings: enabled=%d lead=%dms early=%dms late=%dms octaves=%d banner=%d chords=%d (saved)", st.enabled,
            st.leadMs, st.earlyMs, st.lateMs, st.acceptOctaves, st.showBanner, st.waitChords);
    }

    // ---- dev: unload when the file NoteByNote.unload appears next to the DLL
    bool UnloadRequested(DWORD now) {
        if (now - lastUnloadCheck <= 500) return false;
        lastUnloadCheck = now;
        return DeleteFileW((DllDir() + L"NoteByNote.unload").c_str()) != FALSE;
    }

    // ---- 3. game state. Returns true while a song is on screen.
    bool UpdateScreen(DWORD now) {
        // Before the first dialog the menu pointer isn't valid and every read of it fails with an
        // exception (caught); don't do that every millisecond while the game is loading.
        if (menuOk || now >= nextMenuTry) {
            menuOk = game::GetMenu(&menu);
            if (!menuOk) {
                nextMenuTry = now + 50;
                if (game::GetPreMenu(&preMenu) && preMenu != lastPreMenu) {
                    Log("pre-menu: %s", preMenu.c_str());
                    lastPreMenu = preMenu;
                }
            }
        }
        fastintro::Tick(menuOk || lastPreMenu == "TitleScreen");
        crashfix::Tick();  // the protector's redirect can appear after the mod started
        if (now - lastHeartbeat > 5000) Heartbeat(now);
        startup::Tick(st.skipPopups, menuOk, menu, overlay::GameWindow(), now);  // Ubisoft popups at game start
        if (!menuOk) return false;
        if (menu != lastMenu) {
            Log("screen: %s", menu.c_str());
            report::Limited(("screen " + menu).c_str(), 1, "  Screen: %s", menu.c_str());
            lastMenu = menu;
        }
        inSong = menu.size() >= cfg.menuSuffix.size() &&
                 menu.compare(menu.size() - cfg.menuSuffix.size(), std::string::npos, cfg.menuSuffix) == 0;
        std::string key;
        if (game::GetSongKey(&key)) {  // only valid while browsing songs
            if (key != lastKey) report::Limited("songkey", 3, "  Song highlighted in the list: %s", key.c_str());
            lastKey = key;
        }
        if (!inSong) {
            LeftSong();
            return false;
        }
        if (!songScreenTick) songScreenTick = now;
        return true;
    }

    // What the mod sees, every 5 s (diagnostics).
    void Heartbeat(DWORD now) {
        lastHeartbeat = now;
        songStats.Save();  // (only if something changed: a game crash or exit loses at most 5 s)
        double ht = -1;
        const bool tOk = game::GetSongTime(&ht);
        game::GetPhraseLevels(&levels);
        Log("status: menu=%s key=%s chart=%s enabled=%d t=%s%.3f cursor=%.3f frozen=%d hold=%d tap=%d samples=%lld levels=[%s]",
            menuOk ? menu.c_str() : "?", lastKey.c_str(), chartOk ? chart.arrangement.c_str() : "-", st.enabled,
            tOk ? "" : "(n/a)", ht, cursor, frozen, menuHold, tap.IsOpen(), totalSamples, Join(levels).c_str());
    }

    // Pause menu, song end, other screens: the game is in charge. If we were holding the song, just
    // forget it (the game's own pause stops/restarts the music and resets its clock flag).
    void LeftSong() {
        if (frozen || menuHold) {
            Log("left the song screen while holding the song; releasing");
            if (frozen) {
                SaveWaitAudio();
                unplayedT = waitFor.time;
            }
            frozen = menuHold = false;
            countInEnd = 0;
        }
        songStats.Save();
        game::ResetSongCache();
        lastT = -1;
        announced = false;
        clockTick = 0;
        upcomingChord.clear();
        songScreenTick = 0;
    }

    // ---- 4. the chart of the arrangement being played, read from game memory (again whenever the
    //         game loads another song/arrangement; retried while the song is still loading)
    void ReadChart(DWORD now) {
        const uintptr_t data = game::SongDataAddress();
        if (data != chartData) chartOk = false;
        if (!chartOk && data && now >= nextChartTry) {
            nextChartTry = now + 500;
            if (game::ReadSongChart(&chart)) {
                chartOk = true;
                chartData = data;
                lastT = -1;
                unplayedT = -1;
                overlay::ClearRanges();  // a new song: no practice parts yet
                songStats.Open(statsDir, stats::RecordName(lastKey, chart.arrangement, chart.levelCounts, chart.pis.size()),
                               stats::RecordTail(chart.arrangement, chart.levelCounts, chart.pis.size()));
                {
                    const std::wstring& rec = songStats.Path();
                    Log("trouble spots: record %s (%d notes)", Narrow(rec.substr(rec.find_last_of(L'\\') + 1)).c_str(),
                        (int)songStats.Count());
                }
                songStats.ResetRun();
                statsChanged = true;
                noteMarks.clear();
                clearedNow.clear();
                holding = false;
                tuneCheck.Reset();  // (another tuning, maybe)
                tuneFound = {};
                clockChecked = false;
                clockStill = 0;
                clockTick = 0;
                ReportChart(chart, lastKey);
                const bool bassTracker = chart.bass || chart.bassUnsure;  // bass needs a longer window
                if (bassTracker != trackerIsBass) {
                    trackerIsBass = bassTracker;
                    tracker = NoteTracker(trackerIsBass ? bassCfg : guitarCfg);
                }
            }
        }
        if (!announced && (chartOk || now - songScreenTick > 10000)) {  // once per song: say what's going on
            announced = true;
            if (!chartOk) {
                Log("chart: couldn't read the song's notes from memory");
                overlay::Toast("Note-by-Note: couldn't read this song's notes, it plays normally", 4000);
            } else if (st.enabled) {
                overlay::Toast("Note-by-Note ON  -  F8 menu", 3500);
            }
        }
    }

    // Report: the song clock must run with the music (1 s with the song not held).
    void CheckClock(DWORD now) {
        if (!chartOk || clockChecked) return;
        double ct;
        if (frozen || menuHold || rewinding || !game::GetSongTime(&ct)) {  // (a Riff Repeater rewind isn't a fault)
            clockTick = 0;
        } else if (!clockTick) {
            clockTick = now;
            clockT = ct;
        } else if (now - clockTick >= 1000) {
            const double moved = ct - clockT;
            // A second that began at exactly 0 doesn't count either: when the music starts the clock
            // jumps to where the music is (seen: 0.00 -> 2.96 s), which is not its speed.
            if ((moved == 0 || clockT == 0) && ++clockStill < 20) {
                clockTick = 0;  // not started yet (the song's intro): measure again
            } else {
                report::Limited("clock", 3, "  Song clock: moved %.2f s in 1 s: %s", moved,
                                (moved > 0.5 && moved < 1.6) ? "OK"
                                : moved == 0 ? "FAILED (it never moved in 20 s)" : "FAILED (or the game was paused)");
                clockChecked = true;
            }
        }
    }

    // ---- 5. our menu holds the song while it is open (so settings can be changed calmly)
    void HoldForMenu(DWORD now) {
        if (overlay::MenuOpen()) {
            if (!frozen && !menuHold && now >= nextFreezeTry) {
                if (game::Freeze()) { menuHold = true; Log("menu: song held"); }
                else nextFreezeTry = now + 500;  // failed (no music playing yet): retry in 0.5 s
            }
        } else if (menuHold) {
            game::Unfreeze();
            menuHold = false;
            Log("menu: song resumed");
        }
    }

    // A note the mode waits for: not ignored, chords only if chord waits are on, not greyed out, and
    // inside the part being practised.
    bool CanWait(const Target& x) const { return Waitable(st, x) && !Greyed(x) && Inside(x); }

    // The song stops a few ms PAST its note, so when the player opens the game's pause screen while it
    // waits, the game counts that note as passed and greys it out on resuming; it was never played, so
    // it (and what follows) is waited for anyway. Before this, every pause during a wait lost a note.
    // Not in Riff Repeater: there the game greys it even when the song stopped 25 ms before it, and
    // the loop comes round again anyway.
    bool Greyed(const Target& x) const {
        const bool unplayed = loopEnd < 0 && unplayedT >= 0 && x.time >= unplayedT - 0.001;
        return greyT > 0 && x.time < greyT - 0.001 && !unplayed;
    }

    // Inside the part being practised: the practice parts of the practice bar, if any (outside them the
    // song plays on without waiting), and in Riff Repeater also inside the loop (its end is where it
    // starts over: the next phrase's first note isn't waited for, the game rewinds there; the lead-in
    // before it isn't either). Practice parts that miss the loop don't count there: a part left from
    // before, elsewhere in the song, stopped every wait in the loop.
    bool Inside(const Target& x) const {
        const bool loop = loopEnd > 0;
        if (loop && !(x.time >= loopStart - 0.001 && x.time < loopEnd - 0.001)) return false;
        bool any = false, inside = false;
        for (const auto& r : ranges) {
            if (loop && (r.second < loopStart || r.first >= loopEnd)) continue;
            any = true;
            inside = inside || (x.time >= r.first - 0.001 && x.time <= r.second + 0.001);
        }
        return !any || inside;
    }

    // ---- 6.-8. follow the song: keep the cursor in sync, wait at the next note, or pass it
    void Follow(DWORD now, bool skip) {
        double t;
        if (!chartOk || !st.enabled || !game::GetSongTime(&t)) { upcomingChord.clear(); return; }
        // The practice parts, if the player chose some (logged once they stop changing: a drag on the
        // practice bar changes them every frame).
        std::vector<overlay::Range> parts = overlay::GetRanges();
        if (parts != ranges) {
            ranges = std::move(parts);
            rangesLogTick = now + 500;
        }
        if (rangesLogTick && now >= rangesLogTick) {
            rangesLogTick = 0;
            std::string list;
            for (const auto& r : ranges) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%s%.2f-%.2f", list.empty() ? "" : ", ", r.first, r.second);
                list += buf;
            }
            Log("practice parts: %s", ranges.empty() ? "none (the whole song)" : list.c_str());
        }
        // Riff Repeater's loop (logged when it changes).
        double ls = -1, le = -1;
        if (!game::GetLoop(&ls, &le)) ls = le = -1;
        if (ls != loopStart || le != loopEnd) {
            if (le > 0) Log("Riff Repeater loop %.3f - %.3f s: waits only inside it", ls, le);
            else if (loopEnd > 0) Log("Riff Repeater loop off");
            loopStart = ls;
            loopEnd = le;
        }
        // Greyed-out notes (setting SkipGreyedNotes): after resuming from the game's pause screen the
        // song replays a few seconds with the notes already passed greyed out; those aren't waited for
        // again. Only a grey time a little ahead of the song counts (a resume goes back ~3 s), so a
        // stale value can never switch off the waits for a whole song.
        greyT = -1;
        if (st.skipGreyed && game::GetGreyTime(&greyT) && !(greyT > t && greyT < t + 20)) greyT = -1;
        if (!game::GetPhraseLevels(&levels)) levels.clear();

        // ---- 6. keep the cursor in sync with the song position
        // (song start, Riff Repeater loops, the rewind after the game's pause, the mode switched on...)
        // lastT < 0 (a new song, the mode switched on) re-syncs even while our menu holds the song: the
        // mode is switched on IN the menu, so the song is always held then. (Skipping it there left the
        // cursor where the mode was switched off, and the song then stopped at a note seconds behind.)
        // Riff Repeater doesn't jump back to the loop's start: the highway rewinds over ~1 s, the clock
        // going down a few ms at a time. So a step back is measured from the furthest point the song
        // reached (peakT), and while it rewinds the cursor follows it down and nothing is waited for
        // (a note the rewind passes would otherwise stop the song mid-rewind).
        if (rewinding) {
            if (lastT < 0 || t > lastT) {  // playing forward again (or re-synced since)
                rewinding = false;
                if (lastT >= 0) Log("  (rewound to %.2f s)", lastT);
            } else {
                cursor = std::min(cursor, t - 0.05);
                ForgetMarksFrom(t - 0.05);
                lastT = peakT = t;
                upcomingChord.clear();
                return;
            }
        }
        const bool back = lastT >= 0 && t < peakT - 0.25, ahead = lastT >= 0 && t > lastT + 1.0;
        if (!frozen && (lastT < 0 || ((back || ahead) && !menuHold))) {
            if (lastT >= 0) Log("song position jumped %.2f -> %.2f s", back ? peakT : lastT, t);
            else Log("song position %.3f s: following from here", t);
            cursor = t - 0.05;
            if (back) ForgetMarksFrom(t - 0.05);
            peakT = t;
            rewinding = back;
        }
        lastT = t;
        peakT = std::max(peakT, t);
        if (rewinding) {
            upcomingChord.clear();
            return;
        }
        if (unplayedT >= 0 && !frozen && cursor >= unplayedT - 0.001) unplayedT = -1;  // played or skipped since

        // ---- 7. waiting: the player's notes, or a skip
        if (frozen) {
            Waiting(now, skip);
            return;
        }
        if (menuHold) return;

        const double leadS = st.leadMs / 1000.0, earlyS = st.earlyMs / 1000.0;
        const Target* next = PassNotes(t, earlyS);
        if (!next) { upcomingChord.clear(); return; }  // end of the chart
        next = CheckOnTime(next, t, earlyS);
        upcomingChord = (next && next->chord && CanWait(*next)) ? next->midi : kNoChord;
        if (!next || !CanWait(*next)) return;
        nextWaitT = next->time;  // the song stops here unless it's played (the tab's cursor won't pass it)
        if (nextTarget.time != next->time || nextTarget.level != next->level) nextTarget = *next;  // (copy once)
        // Guide only (setting StopSong off): the song never stops.
        if (!st.stopSong) {
            GuideFollow(*next, t, now);
            return;
        }
        // A strum is checked 90 and 180 ms after its attack: while one is being checked, give it a
        // moment before stopping the song (so a chord played right on time doesn't stop it).
        if (next->chord && chordDet.Pending() && t < next->time + 0.2) return;

        // ---- 8. reached the next note without it being played -> wait for it
        // With a late window (LateMs) the song goes on a little past the note: a note played on the beat
        // is heard some 50-150 ms after its time (the attack, the detector, the audio buffer), and
        // stopping before it made on-beat playing stop-and-go. Otherwise it stops LeadMs before the note.
        const double stopAt = st.lateMs > 0 ? next->time + st.lateMs / 1000.0 : next->time - leadS;
        if (t < stopAt - 0.3) game::PrepareFreeze();  // (the slow search of the first stop, done before it)
        if (t >= stopAt && now >= nextFreezeTry) FreezeAt(*next, t, now);
    }

    // "Show the notes": the banner shows the note until its time, then already the note after it, while
    // that one can still be played a little late (LateMs; a chord at least 0.2 s: its strum is checked
    // 90 and 180 ms after it); a hit in that time is logged by CheckOnTime. Past that, or when the note
    // after it is played, it wasn't played: a miss (only counted while the player is playing, so
    // watching the notes go by doesn't fill the trouble spots).
    void GuideFollow(const Target& note, double t, DWORD now) {
        if (t < note.time) return;
        const double late = std::max(st.lateMs / 1000.0, note.chord ? 0.2 : 0.0);
        const Target* after = NextWaitable(note);
        const bool movedOn = after && !after->chord && t >= after->time - st.earlyMs / 1000.0 && HeardNow(*after);
        // From here on the tab's cursor and the banner go by the note after it. (Also on the loop that
        // passes the note: left on the note, the tab's cursor, already past it, jumped back for a frame.)
        nextWaitT = (after && !movedOn) ? after->time : -1;
        if (after) nextTarget = *after;
        if (t < note.time + late && !movedOn) return;
        if (now - lastHeardTick < 2000) {
            Log("miss %.3f %s", note.time, Describe(chart, note).c_str());
            RecordNote(note.time, stats::Result::kMissed);
        }
        cursor = note.time;
        if (movedOn) {  // (this loop's notes would be gone by the next one)
            Log("hit  %.3f %s on time (%+.0f ms, level %d)", after->time, Describe(chart, *after).c_str(),
                (t - after->time) * 1000.0, after->level);
            RecordNote(after->time, stats::Result::kOnTime);
            StartHold(*after);
            TuneFor(*after);
            cursor = after->time;
        }
    }

    // The next note the mode waits for after `x` (nullptr = none in the next 64).
    const Target* NextWaitable(const Target& x) const {
        const Target* c = chart.NextTarget(x.time, levels);
        for (int i = 0; c && !CanWait(*c) && i < 64; ++i) c = chart.NextTarget(c->time, levels);
        return (c && CanWait(*c)) ? c : nullptr;
    }

    // What the guitar gave in this loop is `x` (a chord: the chord check matched the chord expected).
    bool HeardNow(const Target& x) const {
        if (x.chord) {
            for (const auto& cr : chordResults)
                if (cr.match) return true;
            return false;
        }
        for (const auto& ev : events)
            if (Matches(st, chart, x, ev.midi)) return true;
        return false;
    }

    // Next note on the highway (using the current level of each phrase), passing the ones not waited
    // for (ignored notes, chords if the player turned chord waits off, greyed-out notes) once the song
    // reaches them. nullptr = the end of the chart.
    const Target* PassNotes(double t, double earlyS) {
        const Target* next = chart.NextTarget(cursor, levels);
        while (next && !CanWait(*next) && next->time <= t + earlyS) {
            // Logged: chords, and greyed-out notes (not the notes outside the practised part).
            const bool waitable = Waitable(st, *next), inside = Inside(*next);
            if ((next->chord && inside) || (waitable && inside && Greyed(*next)))
                Log("pass %.3f %s (%s)", next->time, Describe(chart, *next).c_str(),
                    waitable ? "greyed out after resuming" : next->ignore ? "ignored" : "chord waits off");
            cursor = next->time;
            next = chart.NextTarget(cursor, levels);
        }
        return next;
    }

    // Played on time (or a little early): no need to stop there. Returns the note after it, or `next`.
    const Target* CheckOnTime(const Target* next, double t, double earlyS) {
        if (!CanWait(*next) || t < next->time - earlyS) return next;
        if (!HeardNow(*next)) return next;
        Log("hit  %.3f %s on time (%+.0f ms, level %d)", next->time, Describe(chart, *next).c_str(),
            (t - next->time) * 1000.0, next->level);
        RecordNote(next->time, stats::Result::kOnTime);
        StartHold(*next);
        TuneFor(*next);
        cursor = next->time;
        return chart.NextTarget(cursor, levels);
    }

    // Stops the song at `next` (t = the song time now).
    void FreezeAt(const Target& next, double t, DWORD now) {
        // Only a FAILED freeze waits 0.5 s before the next try (e.g. the song is still loading).
        // (It used to wait after every freeze: with fast notes, or right after F9, the next stop
        // then came up to ~0.35 s late and the song ran past the note.)
        if (!game::Freeze()) {
            nextFreezeTry = now + 500;
            return;
        }
        frozen = true;
        frozenT = t;
        holdRetries = 0;
        waitFor = next;
        waitHint.clear();
        waitMarks.clear();
        waitWrong = false;
        frozenTick = now;
        waitAudioStart = debugAudio.Pos() - 2LL * 48000;
        // "+N ms": how far past the note the song stopped (chords: up to 200 ms, see Follow()).
        Log("WAIT %.3f (phrase iteration %d, level %d, stopped at %+d ms): play %s", next.time, next.pi, next.level,
            (int)std::lround((t - next.time) * 1000), Describe(chart, next).c_str());
    }

    // ---- 7. while the song waits: a skip, or the note played (then the song goes on)
    void Waiting(DWORD now, bool skip) {
        if (!HoldKept(now)) return;
        if (countInEnd) {  // counting in: the note was played, the song goes on when the count ends
            if (skip || now >= countInEnd) ReleaseWait();
            return;
        }
        // Testing without a guitar: after TestAutoPassMs the wait passes as if the note was played.
        const bool autoPass = !skip && cfg.testAutoPassMs > 0 && now - frozenTick >= (DWORD)cfg.testAutoPassMs;
        if (skip || autoPass) {
            Log("%s %.3f %s after waiting %.2f s", skip ? "SKIP" : "AUTO-PASS (test)", waitFor.time,
                Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
            if (skip) {
                SaveWaitAudio();  // skipped = maybe not detected
                RecordNote(waitFor.time, stats::Result::kSkipped);
            }
            overlay::Toast(skip ? "Skipped" : "Test: passed by itself", 1200);
            cursor = waitFor.time;
            ReleaseWait();
            return;
        }
        if (!HeardWaitedNote(now)) return;
        Log("HIT  %.3f %s after waiting %.2f s", waitFor.time, Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
        RecordNote(waitFor.time, stats::Result::kWaited, (now - frozenTick) / 1000.0, waitWrong);
        StartHold(waitFor);
        // Keep the audio of long waits, of waits with a wrong note (string identification), and of every
        // chord for now (to tune the detection offline).
        if (waitFor.chord || waitWrong || now - frozenTick > 3000) SaveWaitAudio();
        cursor = waitFor.time;
        // After a long wait, a count-in in the song's tempo before it goes on (the player finds the beat
        // again). The note was played: the song stays held only for the count.
        if (st.countInBeats > 0 && now - frozenTick > 2000) {
            countInBeat = (DWORD)std::lround(BeatSeconds(waitFor.time) * 1000.0);
            countInEnd = now + countInBeat * st.countInBeats;
            Log("count-in: %d beats of %u ms", st.countInBeats, countInBeat);
            return;
        }
        ReleaseWait();
    }

    // A note held longer than a usual pick: it rings at least about one beat of the song (a little less
    // counts: a chart's tails end a bit before the beat), and at least overlay::kHoldMinS. The banner then
    // says "Hold" and counts it down. (From 0.2 s, almost every note of a riff of eighth notes said "Hold"
    // (user): those just ring until the next pick.)
    bool LongNote(const Target& x) const {
        return x.sustain >= std::max(overlay::kHoldMinS, 0.9 * BeatSeconds(x.time));
    }

    // One beat of the song around song time t (from the beat grid), for the count-in; 0.5 s if unknown.
    double BeatSeconds(double t) const {
        const auto& b = chart.beats;
        for (size_t i = 1; i < b.size(); ++i)
            if (b[i].time > t) {
                const double d = b[i].time - b[i - 1].time;
                return (d > 0.2 && d < 2.0) ? d : 0.5;
            }
        return 0.5;
    }

    // The hold watchdog: the song must not move while it waits. Right after coming back from Riff
    // Repeater (and maybe elsewhere) the game restarts the music just after we paused it: the pause
    // said OK, but the new playback runs, and the song used to go on with the mod still "waiting".
    // Paused again, the new playback stops (Freeze looks up the current one). If it can't be held,
    // stop waiting and follow the song from where it really is (the next note stops it again).
    // The song can also jump BACK while held: at the end of a Riff Repeater loop the game starts the
    // loop over even then (the mod had stopped at the first note after the loop). That note isn't
    // coming, so the wait ends there too. (Before, the cursor stayed past the loop: no more waits.)
    bool HoldKept(DWORD now) {
        double t;
        if (!game::GetSongTime(&t)) return true;
        if (t < frozenT - 0.25) {
            Log("song position jumped %.2f -> %.2f s while waiting for %.3f: following from there", frozenT, t,
                waitFor.time);
            ReleaseWait();
            cursor = t - 0.05;  // follow the rewind down (see Follow)
            ForgetMarksFrom(cursor);
            lastT = peakT = t;
            rewinding = true;
            return false;
        }
        if (t - frozenT < 0.3) return true;
        if (holdRetries < 2 && game::Freeze()) {
            ++holdRetries;
            Log("the song kept playing while held (%.2f s past the stop): paused it again", t - frozenT);
            frozenT = t;
            return true;
        }
        Log("couldn't hold the song at %.3f (it kept playing, now %.3f s): following from here", waitFor.time, t);
        frozen = false;
        game::Unfreeze();
        lastT = -1;  // re-sync the cursor to the song on the next loop
        nextFreezeTry = now + 1000;
        return false;
    }

    // True if what was just played is the note/chord being waited for. Logs what was heard, and after a
    // wrong note or chord sets the "how to fix it" advice (waitHint) the banner shows.
    // The instrument, for the "how to fix it" advice.
    hint::Neck MakeNeck() const {
        hint::Neck neck;
        neck.strings = chart.bass ? 4 : 6;
        std::copy(std::begin(chart.open), std::end(chart.open), neck.open);
        neck.capo = chart.capo;
        neck.bassUnsure = chart.bassUnsure;
        neck.fromThick = st.stringsFromThick;
        return neck;
    }

    // The note just heard was meant to be `x` (a waited note, or one played on time): its steady pitch
    // goes to the tuning check. Not for notes whose pitch moves on purpose (bends, slides, vibrato,
    // harmonics) or has none (mutes).
    void TuneFor(const Target& x) {
        using namespace technique;
        constexpr uint32_t kMoves = kBend | kSlide | kUnpitchedSlide | kVibrato | kHarmonic | kPinchHarmonic | kMute | kTremolo;
        if (x.chord || x.midi.empty() || x.string < 0 || (x.tech.mask & kMoves) || !steady.On()) return;
        steadyString = x.string;
        steadyWanted = x.midi[0];
    }

    // A note's steady pitch is known (fractional MIDI).
    void TuneNote(double pitch) {
        if (steadyString < 0 || calString >= 0) return;
        const double off = pitch - steadyWanted;
        tuneCheck.Add(steadyString, steadyWanted, pitch);
        if (std::abs(off) >= 0.25 && std::abs(off) <= 2.5)
            Log("tuning: %c string, %s wanted, heard %+.0f cents off", "EADGBe"[steadyString], MidiName(steadyWanted).c_str(),
                off * 100.0);
        steadyString = -1;
        const tuning::Finding f = tuneCheck.Get();
        // Said again only when it changes: another string, another way, or a step more or less.
        auto same = [](const tuning::Finding& a, const tuning::Finding& b) {
            return a.string == b.string && a.all == b.all && a.steps == b.steps && (a.offset < 0) == (b.offset < 0);
        };
        if (same(f, tuneFound)) {
            tuneFound = f;
            return;
        }
        tuneFound = f;
        if (f.string < 0 && !f.all) {
            Log("tuning: sounds in tune now");
            if (st.tuningCheck) overlay::Toast("Sounds in tune now", 2500);
            return;
        }
        const std::string text = tuning::AdviceText(f, MakeNeck());
        Log("tuning: %s", text.c_str());
        if (st.tuningCheck) overlay::Toast(text.substr(text.find("  -  ") + 5), 7000);
    }

    // A wrong note `heard` while waiting for `x` is what the guitar's tuning gives there: a string (or
    // all of them) a half step or two off gives exactly that many; a little off, one half step that way.
    bool TuningCaused(const Target& x, int heard) const {
        if (!st.tuningCheck || x.midi.empty() || (tuneFound.string != x.string && !tuneFound.all)) return false;
        const int d = heard - x.midi[0];
        if (tuneFound.steps != 0) return d == tuneFound.steps;
        return d == (tuneFound.offset < 0 ? -1 : 1);
    }

    bool HeardWaitedNote(DWORD now) {
        bool hit = false;
        const hint::Neck neck = MakeNeck();
        // A wrong note gets advice only when it was picked (an attack), and not in the first moment of
        // the wait (that is still the previous note ringing).
        const bool adviseNow = now - frozenTick > 150;
        if (adviseNow && !events.empty()) TuneFor(waitFor);  // (the last note heard: the one being measured)
        for (size_t i = 0; i < events.size(); ++i) {
            const NoteEvent& ev = events[i];
            if (Matches(st, chart, waitFor, ev.midi)) { hit = true; break; }
            Log("  heard %s (%+.0f cents, %.1f dB, aper %.2f%s), waiting for %s", MidiName(ev.midi).c_str(), ev.cents, ev.levelDb,
                ev.aperiodicity, ev.attack ? ", attack" : "", Describe(chart, waitFor).c_str());
            if (ev.attack && adviseNow && !waitFor.chord && !waitFor.midi.empty()) {
                waitWrong = true;
                // Without knowing the string: the likely spot and, faded, the others with the same pitch.
                hint::Mark at;
                hint::Line guess = hint::ForNote(neck, waitFor.string, waitFor.fret, waitFor.midi[0], ev.midi, &at);
                std::vector<hint::Mark> marks;
                if (at.string >= 0) {
                    marks.push_back(at);
                    for (const auto& m : hint::SameNoteElsewhere(neck, at)) marks.push_back(m);
                }
                if (guess.empty()) continue;
                // The guitar is out of tune, and this is the note it gives instead of the right one: say
                // that, not "move a fret".
                if (TuningCaused(waitFor, ev.midi)) {
                    waitHint = tuning::Advice(tuneFound, neck);
                    waitMarks.clear();
                    pendingId.on = false;
                    Log("  advice: %s", hint::Text(waitHint).c_str());
                    continue;
                }
                if (StringIdReady()) {
                    // The note's sound may tell where it was played: nothing is shown until it's measured
                    // (~0.25 s); then the spot, or this guess if the sound can't tell (WrongNoteSpot).
                    StartStringId(ev, eventPos[i], false);
                    pendingId.guessHint = guess;
                    pendingId.guessMarks = marks;
                    continue;
                }
                waitHint = guess;
                waitMarks = marks;
                Log("  advice: %s", hint::Text(waitHint).c_str());
            }
        }
        for (const auto& cr : chordResults) {  // only produced while waiting for a chord
            Log("  %s", cr.Describe().c_str());
            hit = hit || cr.match;
            if (!cr.match && !cr.quiet && !cr.heard.empty() && adviseNow) {
                hint::ChordHeard detected{{}, cr.extra, cr.hits, cr.needed};
                detected.midi.reserve(cr.heard.size());
                for (const auto& h : cr.heard) detected.midi.push_back(h.first);
                std::vector<hint::Mark> at;
                hint::Line l = hint::ForChord(neck, waitFor.frets, waitFor.notes, detected, &at);
                if (!l.empty()) {
                    waitHint = l;
                    waitMarks = at;
                    Log("  advice: %s", hint::Text(waitHint).c_str());
                }
            }
        }
        return hit;
    }
};

DWORD WINAPI MainThread(LPVOID) {
    LogOpen(DllDir() + L"NoteByNote.log");
    Log("Note-by-Note starting");
    report::Open(DllDir() + L"NoteByNote_report.txt");
    const Config cfg = LoadConfig();
    const overlay::Settings& st = cfg.initial;
    Log("config: enabled=%d menuKey=0x%X skipKey=0x%X lead=%dms early=%dms late=%dms octaves=%d banner=%d chords=%d",
        st.enabled, cfg.menuKey, cfg.skipKey, st.leadMs, st.earlyMs, st.lateMs, st.acceptOctaves, st.showBanner,
        st.waitChords);
    crashfix::Start(st.fixCrash);    // first of all: the game can crash any moment until then
    if (cfg.crashLog) crashlog::Start();  // and if it does crash, the log says where
    else Log("crash log: off (CrashLog=0)");
    if (cfg.testUnverifiedGame || cfg.testAutoPassMs)
        report::Line("Test settings: TestUnverifiedGame=%d, TestAutoPassMs=%d", cfg.testUnverifiedGame, cfg.testAutoPassMs);
    if (!game::Init(cfg.testUnverifiedGame, cfg.testPatternsOnly)) return 0;
    // After Init: it tells which build this is and waits until the game's code is decrypted (the mod
    // can start while the protector is still at work). The Windows clocks are only made faster on a
    // build where that has been seen to work; on a build under test the game must start as it does
    // without the mod. TestFastIntro=1 tries it there.
    if (game::Verified() || cfg.testFastIntro)
        fastintro::Start(st.fastIntro);  // the logos are already playing
    else
        Log("fast intro: off (this game version isn't verified yet; TestFastIntro=1 tries it)");
    overlay::Start(st);

    timeBeginPeriod(1);
    {
        MainLoop loop(cfg);
        loop.Run();  // until the dev unload
    }

    // Unload (dev): the loop released the song; remove the overlay, then free the DLL.
    fastintro::Stop();  // before the overlay: it uninitializes MinHook
    overlay::Stop();
    timeEndPeriod(1);
    Log("Note-by-Note unloaded");
    LogClose();
    FreeLibraryAndExitThread(g_self, 0);
}

}  // namespace
}  // namespace nbn

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        nbn::g_self = mod;
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(nullptr, 0, nbn::MainThread, nullptr, 0, nullptr);  // no real work in DllMain
        if (t) CloseHandle(t);
    }
    return TRUE;
}
