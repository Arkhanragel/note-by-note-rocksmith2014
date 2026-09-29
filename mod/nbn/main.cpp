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
#include <string>
#include <vector>

#include "chart.h"
#include "detector.h"
#include "crashfix.h"
#include "fastintro.h"
#include "game.h"
#include "log.h"
#include "overlay.h"
#include "report.h"
#include "startup.h"
#include "tap.h"

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
    bool testUnverifiedGame = false;     // run on a game build nobody verified (addresses found by pattern)
    int testAutoPassMs = 0;              // a wait passes by itself after this long (testing without a guitar)
    bool testPatternsOnly = false;       // dev: ignore the verified addresses, use only the patterns
};

std::wstring DllDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring IniPath() { return DllDir() + L"NoteByNote.ini"; }

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
                "; Stop this many milliseconds BEFORE the note reaches the line (0 = exactly on it)\n"
                "LeadMs=0\n"
                "; A correct note played up to this many milliseconds early counts without stopping\n"
                "EarlyMs=300\n"
                "; 1 = the same note one octave higher/lower also counts\n"
                "AcceptOctaves=0\n"
                "; 1 = show what to play (string, colour, fret) while the song waits\n"
                "ShowBanner=1\n"
                "; 1 = the song also waits at chords, 0 = chords pass (only single notes wait)\n"
                "WaitChords=1\n"
                "; 1 = show the song time (top-left corner) while playing\n"
                "ShowClock=1\n"
                "; 1 = show the notes coming up as a scrolling tab (left of the highway, below the lyrics)\n"
                "ShowTab=1\n"
                "; 1 = bar lines (with bar numbers) and beat lines in the tab, to read the rhythm\n"
                "TabBeats=1\n"
                "; 1 = rhythm under the tab, as in printed tab: a stem per note, beams = notes per beat\n"
                ";     (no beam = 1, 1 beam = 2, 2 beams = 4, 3 beams = 8; a small 3 = triplets)\n"
                "TabRhythm=1\n"
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
                "; Pages only: 1 = two rows; the cursor plays one while the other already shows the\n"
                ";     next page (swapped in as soon as the cursor leaves it), 0 = one row\n"
                "TabTwoRows=0\n"
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
                "; Play the start-up logos this many times faster (1 = normal speed, up to 8)\n"
                "FastIntro=4\n"
                "; 1 = work around the game's own random crash / freeze (mostly at start-up): puts back\n"
                ";     a Windows function the game's copy protection redirects (in memory only)\n"
                "FixGameCrash=1\n"
                "; 1 = save the guitar audio of each wait to NoteByNote_debug\\wait_<time>.wav (useful\n"
                ";     to report a note that wasn't recognised; the files add up, delete them by hand)\n"
                "SaveWaitAudio=0\n"
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
                "TabSize=100\n",
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
    c.initial.leadMs = GetPrivateProfileIntW(L"NoteByNote", L"LeadMs", 0, ini.c_str());
    c.initial.earlyMs = GetPrivateProfileIntW(L"NoteByNote", L"EarlyMs", 300, ini.c_str());
    c.initial.acceptOctaves = GetPrivateProfileIntW(L"NoteByNote", L"AcceptOctaves", 0, ini.c_str()) != 0;
    c.initial.showBanner = GetPrivateProfileIntW(L"NoteByNote", L"ShowBanner", 1, ini.c_str()) != 0;
    c.initial.waitChords = GetPrivateProfileIntW(L"NoteByNote", L"WaitChords", 1, ini.c_str()) != 0;
    c.initial.showClock = GetPrivateProfileIntW(L"NoteByNote", L"ShowClock", 1, ini.c_str()) != 0;
    c.initial.showTab = GetPrivateProfileIntW(L"NoteByNote", L"ShowTab", 1, ini.c_str()) != 0;
    c.initial.tabBeats = GetPrivateProfileIntW(L"NoteByNote", L"TabBeats", 1, ini.c_str()) != 0;
    c.initial.tabSeconds = std::max(2, std::min(8, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabSeconds", 4, ini.c_str())));
    c.initial.tabRhythm = GetPrivateProfileIntW(L"NoteByNote", L"TabRhythm", 1, ini.c_str()) != 0;
    c.initial.tabSpread = GetPrivateProfileIntW(L"NoteByNote", L"TabSpread", 1, ini.c_str()) != 0;
    c.initial.tabPage = GetPrivateProfileIntW(L"NoteByNote", L"TabPages", 1, ini.c_str()) != 0;
    c.initial.tabTwoRows = GetPrivateProfileIntW(L"NoteByNote", L"TabTwoRows", 0, ini.c_str()) != 0;
    c.initial.tabMirror = GetPrivateProfileIntW(L"NoteByNote", L"TabMirror", 0, ini.c_str()) != 0;
    c.initial.tabThickTop = GetPrivateProfileIntW(L"NoteByNote", L"TabThickOnTop", 0, ini.c_str()) != 0;
    c.initial.tabOpacity =std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabBackground", 69, ini.c_str())));
    c.initial.tabNoteSize =std::max(60, std::min(130, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabNoteSize", 100, ini.c_str())));
    auto narrow = [](const std::wstring& w) { return std::string(w.begin(), w.end()); };  // ASCII only
    // Colours: the theme by name, then the player's own colours ("#RRGGBB"; missing or not a colour = the theme's).
    c.initial.theme = theme::FindTheme(narrow(str(L"Theme", L"Default")));
    for (int i = 0; i < theme::kSlots; ++i) {
        const std::string key = theme::kSlotInfo[i].key;
        c.initial.colors[i] = theme::ParseHex(narrow(str(std::wstring(key.begin(), key.end()).c_str(), L"")));
    }
    c.initial.skipPopups = GetPrivateProfileIntW(L"NoteByNote", L"SkipUbisoftPopups", 1, ini.c_str()) != 0;
    c.initial.fastIntro = std::max(1, std::min(8, (int)GetPrivateProfileIntW(L"NoteByNote", L"FastIntro", 4, ini.c_str())));
    c.initial.fixCrash = GetPrivateProfileIntW(L"NoteByNote", L"FixGameCrash", 1, ini.c_str()) != 0;
    c.saveWaitAudio = GetPrivateProfileIntW(L"NoteByNote", L"SaveWaitAudio", 0, ini.c_str()) != 0;
    c.testUnverifiedGame = GetPrivateProfileIntW(L"NoteByNote", L"TestUnverifiedGame", 0, ini.c_str()) != 0;
    c.testAutoPassMs = std::max(0, (int)GetPrivateProfileIntW(L"NoteByNote", L"TestAutoPassMs", 0, ini.c_str()));
    c.testPatternsOnly = GetPrivateProfileIntW(L"NoteByNote", L"TestPatternsOnly", 0, ini.c_str()) != 0;  // not in the default ini
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
    put(L"AcceptOctaves", st.acceptOctaves);
    put(L"ShowBanner", st.showBanner);
    put(L"WaitChords", st.waitChords);
    put(L"ShowClock", st.showClock);
    put(L"ShowTab", st.showTab);
    put(L"TabBeats", st.tabBeats);
    put(L"TabSeconds", st.tabSeconds);
    put(L"TabSpread", st.tabSpread);
    put(L"TabNoteSize", st.tabNoteSize);
    put(L"TabBackground", st.tabOpacity);
    put(L"TabPages", st.tabPage);
    put(L"TabTwoRows", st.tabTwoRows);
    put(L"TabMirror", st.tabMirror);
    put(L"TabThickOnTop", st.tabThickTop);
    put(L"TabRhythm", st.tabRhythm);
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
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }  // ASCII paths/names only

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
        Log("  (saved the audio of this wait: %s)", Narrow(dir + name).c_str());
    }

private:
    static constexpr long long kSize = 48000 * 20;
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
DWORD WINAPI MainThread(LPVOID) {
    LogOpen(DllDir() + L"NoteByNote.log");
    Log("Note-by-Note starting");
    report::Open(DllDir() + L"NoteByNote_report.txt");
    const Config cfg = LoadConfig();
    overlay::Settings st = cfg.initial;  // the live settings (the menu can change them)
    Log("config: enabled=%d menuKey=0x%X skipKey=0x%X lead=%dms early=%dms octaves=%d banner=%d chords=%d", st.enabled,
        cfg.menuKey, cfg.skipKey, st.leadMs, st.earlyMs, st.acceptOctaves, st.showBanner, st.waitChords);
    crashfix::Start(st.fixCrash);    // first of all: the game can crash any moment until then
    fastintro::Start(st.fastIntro);  // then: the logos are already playing
    if (cfg.testUnverifiedGame || cfg.testAutoPassMs)
        report::Line("Test settings: TestUnverifiedGame=%d, TestAutoPassMs=%d", cfg.testUnverifiedGame, cfg.testAutoPassMs);
    if (!game::Init(cfg.testUnverifiedGame, cfg.testPatternsOnly)) { fastintro::Tick(true); return 0; }
    overlay::Start(st);

    KeyEdge menuKey, skipKey;
    TapReader tap;
    TrackerConfig guitarCfg, bassCfg;
    bassCfg.window = 4096;
    bassCfg.fmin = 35.0;
    NoteTracker tracker(guitarCfg);
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
    double lastT = -1;
    Target waitFor;                   // the note we're frozen on
    hint::Line waitHint;              // how to fix the last wrong note played during this wait
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
    debugAudio.enabled = cfg.saveWaitAudio;
    long long waitAudioStart = 0;
    const std::wstring debugDir = DllDir() + L"NoteByNote_debug\\";

    // Stops waiting for the current note. If our menu is open, the song stays held until it closes.
    auto releaseWait = [&]() {
        if (!frozen) return;
        frozen = false;
        if (overlay::MenuOpen()) menuHold = true;
        else game::Unfreeze();
    };

    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        const DWORD now = GetTickCount();
        game::Tick();

        // ---- 0. what the overlay shows (state of the previous iteration; 1 ms old is fine)
        {
            overlay::View v;
            v.inSong = inSong;
            v.waiting = frozen;
            v.bass = chart.bass;
            v.string = waitFor.string;
            v.fret = waitFor.fret;
            v.chord = waitFor.chord;
            v.chordName = waitFor.chordName;
            std::copy(std::begin(waitFor.frets), std::end(waitFor.frets), v.frets);
            std::copy(std::begin(waitFor.notes), std::end(waitFor.notes), v.notes);
            v.midi = (!waitFor.chord && !waitFor.midi.empty()) ? waitFor.midi[0] : -1;
            if (frozen) v.hint = waitHint;
            // The clock works even with the mode off or without a chart (it's just the song time).
            if (!inSong || !game::GetSongTime(&v.songTime)) v.songTime = -1;
            if (inSong && !game::GetSongLength(&v.songLength)) v.songLength = 0;
            // The scrolling tab: the notes the highway shows from 1 s ago to the end of the tab (+1 s
            // margin, the list is only refreshed every 50 ms). Works with the mode off too.
            if (inSong && chartOk && st.showTab && v.songTime >= 0) {
                if (now >= nextTabRefresh) {
                    nextTabRefresh = now + 50;
                    if (!game::GetPhraseLevels(&tabLevels)) tabLevels.clear();
                    // The past part: a page (tab pages mode) can show up to ~90 % of a page behind the cursor.
                    const double back = st.tabPage ? st.tabSeconds * 1.3 + 1.0 : 1.0;
                    // The future part: two rows also show the whole next page (up to ~2.2 tabs ahead).
                    const double ahead = (st.tabPage && st.tabTwoRows ? st.tabSeconds * 2.3 : st.tabSeconds) + 1.0;
                    chart.TargetsBetween(v.songTime - back, v.songTime + ahead, tabLevels, &tabTargets);
                    chart.BeatsBetween(v.songTime - back, v.songTime + ahead, &tabBeatsRaw);
                    tabBeats.clear();  // always sent: the rhythm needs them even with the lines off
                    for (const Beat& b : tabBeatsRaw) tabBeats.push_back({b.time, b.measure, b.downbeat});
                    tabNotes.clear();
                    for (const Target* t : tabTargets) {
                        overlay::TabNote tn;
                        tn.time = t->time;
                        tn.chord = t->chord;
                        tn.ignore = t->ignore;
                        if (t->chord) std::copy(std::begin(t->frets), std::end(t->frets), tn.frets);
                        else if (t->string >= 0 && t->string < 6) tn.frets[t->string] = t->fret;
                        tn.name = t->chordName;
                        tn.sustain = t->sustain;
                        tabNotes.push_back(tn);
                    }
                }
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

        // ---- 1. guitar -> note events
        if (!tap.IsOpen() && now - lastTapTry > 1000) {
            lastTapTry = now;
            if (tap.Open()) Log("guitar input connected (GuitarTap, %u Hz)", tap.SampleRate());
        }
        events.clear();
        chordResults.clear();
        samples.clear();
        tap.ReadNew(samples);
        totalSamples += (long long)samples.size();
        debugAudio.Push(samples);
        pending.insert(pending.end(), samples.begin(), samples.end());
        // The chord to check: the one we're waiting for, or the next one on the highway (early hits).
        const std::vector<int>& expectChord = frozen ? (waitFor.chord ? waitFor.midi : kNoChord) : upcomingChord;
        size_t used = 0;
        for (; used + NoteTracker::kBlock <= pending.size(); used += NoteTracker::kBlock) {
            NoteEvent ev;
            if (tracker.Process(&pending[used], &ev)) events.push_back(ev);
            ChordResult cr;
            if (chordDet.Process(&pending[used], expectChord, &cr)) chordResults.push_back(cr);
        }
        pending.erase(pending.begin(), pending.begin() + used);

        // ---- 2. keys and the menu
        if (menuKey.Pressed(cfg.menuKey)) {
            overlay::ToggleMenu();
            game::PostUiEvent(cfg.menuSound.c_str());
            Log("menu %s", overlay::MenuOpen() ? "opened" : "closed");
        }
        const bool skip = skipKey.Pressed(cfg.skipKey) | overlay::TakeSkipRequest();

        const overlay::Settings newSt = overlay::GetSettings();
        if (!(newSt == st)) {
            if (newSt.enabled != st.enabled) {
                Log("Note-by-Note %s", newSt.enabled ? "ON" : "OFF");
                overlay::Toast(newSt.enabled ? "Note-by-Note ON" : "Note-by-Note OFF");
                if (!newSt.enabled && frozen) {
                    debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);
                    releaseWait();
                }
                if (newSt.enabled) lastT = -1;  // re-sync to the current position
            }
            if (!newSt.waitChords && frozen && waitFor.chord) {  // chord waits switched off while waiting at one
                cursor = waitFor.time;
                releaseWait();
            }
            st = newSt;
            SaveSettings(st);
            Log("settings: enabled=%d lead=%dms early=%dms octaves=%d banner=%d chords=%d (saved)", st.enabled, st.leadMs,
                st.earlyMs, st.acceptOctaves, st.showBanner, st.waitChords);
        }
        const double leadS = st.leadMs / 1000.0, earlyS = st.earlyMs / 1000.0;

        // ---- dev: unload when the file NoteByNote.unload appears next to the DLL
        if (now - lastUnloadCheck > 500) {
            lastUnloadCheck = now;
            if (DeleteFileW((DllDir() + L"NoteByNote.unload").c_str())) break;
        }

        // ---- 3. game state
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
        if (now - lastHeartbeat > 5000) {  // what the mod sees, every 5 s (diagnostics)
            lastHeartbeat = now;
            crashfix::Tick();
            double ht = -1;
            const bool tOk = game::GetSongTime(&ht);
            game::GetPhraseLevels(&levels);
            Log("status: menu=%s key=%s chart=%s enabled=%d t=%s%.3f cursor=%.3f frozen=%d hold=%d tap=%d samples=%lld levels=[%s]",
                menuOk ? menu.c_str() : "?", lastKey.c_str(), chartOk ? chart.arrangement.c_str() : "-", st.enabled,
                tOk ? "" : "(n/a)", ht, cursor, frozen, menuHold, tap.IsOpen(), totalSamples, Join(levels).c_str());
        }
        startup::Tick(st.skipPopups, menuOk, menu, overlay::GameWindow(), now);  // Ubisoft popups at game start
        if (!menuOk) continue;
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
            // Pause menu, song end, other screens: the game is in charge. If we were holding the song,
            // just forget it (the game's own pause stops/restarts the music and resets its clock flag).
            if (frozen || menuHold) {
                Log("left the song screen while holding the song; releasing");
                if (frozen) debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);
                frozen = menuHold = false;
            }
            game::ResetSongCache();
            lastT = -1;
            announced = false;
            clockTick = 0;
            upcomingChord.clear();
            songScreenTick = 0;
            continue;
        }
        if (!songScreenTick) songScreenTick = now;

        // ---- 4. the chart of the arrangement being played, read from game memory (again whenever the
        //         game loads another song/arrangement; retried while the song is still loading)
        const uintptr_t data = game::SongDataAddress();
        if (data != chartData) chartOk = false;
        if (!chartOk && data && now >= nextChartTry) {
            nextChartTry = now + 500;
            if (game::ReadSongChart(&chart)) {
                chartOk = true;
                chartData = data;
                lastT = -1;
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

        // Report: the song clock must run with the music (1 s with the song not held).
        if (chartOk && !clockChecked) {
            double ct;
            if (frozen || menuHold || !game::GetSongTime(&ct)) {
                clockTick = 0;
            } else if (!clockTick) {
                clockTick = now;
                clockT = ct;
            } else if (now - clockTick >= 1000) {
                const double moved = ct - clockT;
                if (moved == 0 && ++clockStill < 20) {
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
        if (overlay::MenuOpen()) {
            if (!frozen && !menuHold && now >= nextFreezeTry) {
                nextFreezeTry = now + 500;
                if (game::Freeze()) { menuHold = true; Log("menu: song held"); }
            }
        } else if (menuHold) {
            game::Unfreeze();
            menuHold = false;
            Log("menu: song resumed");
        }

        // Right after the song screen opens (song start, Riff Repeater, unpausing) the clock still
        // reports the old time for a moment: acting on it froze the song too early. Let it settle.
        if (now - songScreenTick < 400) continue;

        double t;
        if (!chartOk || !st.enabled || !game::GetSongTime(&t)) { upcomingChord.clear(); continue; }
        if (!game::GetPhraseLevels(&levels)) levels.clear();

        // ---- 6. keep the cursor in sync with the song position
        // (song start, Riff Repeater loops, the rewind after the game's pause, the mode switched on...)
        if (!frozen && !menuHold && (lastT < 0 || t < lastT - 0.25 || t > lastT + 1.0)) {
            if (lastT >= 0) Log("song position jumped %.2f -> %.2f s", lastT, t);
            else Log("song position %.3f s: following from here", t);
            cursor = t - 0.05;
        }
        lastT = t;

        // ---- 7. waiting: the player's notes, or a skip
        if (frozen) {
            // Testing without a guitar: after TestAutoPassMs the wait passes as if the note was played.
            const bool autoPass = !skip && cfg.testAutoPassMs > 0 && now - frozenTick >= (DWORD)cfg.testAutoPassMs;
            if (skip || autoPass) {
                Log("%s %.3f %s after waiting %.2f s", skip ? "SKIP" : "AUTO-PASS (test)", waitFor.time,
                    Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
                if (skip) debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);  // skipped = maybe not detected
                overlay::Toast(skip ? "Skipped" : "Test: passed by itself", 1200);
                cursor = waitFor.time;
                releaseWait();
                continue;
            }
            bool hit = false;
            hint::Neck neck;  // for the "how to fix it" advice
            neck.strings = chart.bass ? 4 : 6;
            std::copy(std::begin(chart.open), std::end(chart.open), neck.open);
            neck.capo = chart.capo;
            neck.bassUnsure = chart.bassUnsure;
            // A wrong note gets advice only when it was picked (an attack), and not in the first
            // moment of the wait (that is still the previous note ringing).
            const bool adviseNow = now - frozenTick > 150;
            for (const auto& ev : events) {
                if (Matches(st, chart, waitFor, ev.midi)) { hit = true; break; }
                Log("  heard %s (%+.0f cents, %.1f dB, aper %.2f%s), waiting for %s", MidiName(ev.midi).c_str(), ev.cents, ev.levelDb,
                    ev.aperiodicity, ev.attack ? ", attack" : "", Describe(chart, waitFor).c_str());
                if (ev.attack && adviseNow && !waitFor.chord && !waitFor.midi.empty()) {
                    waitHint = hint::ForNote(neck, waitFor.string, waitFor.fret, waitFor.midi[0], ev.midi);
                    if (!waitHint.empty()) Log("  advice: %s", hint::Text(waitHint).c_str());
                }
            }
            for (const auto& cr : chordResults) {  // only produced while waiting for a chord
                Log("  %s", cr.Describe().c_str());
                hit = hit || cr.match;
                if (!cr.match && !cr.quiet && !cr.heard.empty() && adviseNow) {
                    std::vector<int> heardMidi;
                    for (const auto& h : cr.heard) heardMidi.push_back(h.first);
                    hint::Line l = hint::ForChord(neck, waitFor.frets, waitFor.notes, heardMidi, cr.extra, cr.hits, cr.needed);
                    if (!l.empty()) {
                        waitHint = l;
                        Log("  advice: %s", hint::Text(waitHint).c_str());
                    }
                }
            }
            if (hit) {
                Log("HIT  %.3f %s after waiting %.2f s", waitFor.time, Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
                // Keep the audio of long waits, and of every chord for now (to tune chord detection offline).
                if (waitFor.chord || now - frozenTick > 3000) debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);
                cursor = waitFor.time;
                releaseWait();
            }
            continue;
        }
        if (menuHold) continue;

        // Next note on the highway (using the current level of each phrase), skipping ignored notes
        // (and chords if the player turned chord waits off).
        const Target* next = chart.NextTarget(cursor, levels);
        while (next && !Waitable(st, *next) && next->time <= t + earlyS) {
            if (next->chord) Log("pass %.3f %s (%s)", next->time, Describe(chart, *next).c_str(), next->ignore ? "ignored" : "chord waits off");
            cursor = next->time;
            next = chart.NextTarget(cursor, levels);
        }
        if (!next) { upcomingChord.clear(); continue; }  // end of the chart

        if (Waitable(st, *next) && t >= next->time - earlyS) {
            bool hit = false;
            if (next->chord) {
                for (const auto& cr : chordResults) hit = hit || cr.match;
            } else {
                for (const auto& ev : events) hit = hit || Matches(st, chart, *next, ev.midi);
            }
            if (hit) {  // played on time (or a little early): no need to stop
                Log("hit  %.3f %s on time (%+.0f ms, level %d)", next->time, Describe(chart, *next).c_str(),
                    (t - next->time) * 1000.0, next->level);
                cursor = next->time;
                next = chart.NextTarget(cursor, levels);
            }
        }
        upcomingChord = (next && next->chord && Waitable(st, *next)) ? next->midi : kNoChord;
        if (!next || !Waitable(st, *next)) continue;
        // A strum is checked 90 and 180 ms after its attack: while one is being checked, give it a
        // moment before stopping the song (so a chord played right on time doesn't stop it).
        if (next->chord && chordDet.Pending() && t < next->time + 0.2) continue;

        // ---- 8. reached the next note without it being played -> wait for it
        if (t >= next->time - leadS && now >= nextFreezeTry) {
            nextFreezeTry = now + 500;  // if freezing fails (e.g. the song is still loading), retry in 0.5 s
            if (game::Freeze()) {
                frozen = true;
                waitFor = *next;
                waitHint.clear();
                frozenTick = now;
                waitAudioStart = debugAudio.Pos() - 2 * 48000;
                Log("WAIT %.3f (phrase iteration %d, level %d): play %s", next->time, next->pi, next->level,
                    Describe(chart, *next).c_str());
            }
        }
    }

    // Unload (dev): release the song if we're holding it, remove the overlay, then free the DLL.
    if (frozen || menuHold) game::Unfreeze();
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
