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
// Log: NoteByNote.log next to the DLL.
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "chart.h"
#include "detector.h"
#include "game.h"
#include "log.h"
#include "overlay.h"
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
                "; Seconds of music the tab shows ahead (2..8)\n"
                "TabSeconds=4\n"
                "; Tab position (the menu has sliders): X from the screen centre, Y from the top, in\n"
                "; 1080p pixels (scaled with the screen height)\n"
                "TabX=-810\n"
                "TabY=385\n",
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
    c.initial.tabSeconds = std::max(2, std::min(8, (int)GetPrivateProfileIntW(L"NoteByNote", L"TabSeconds", 4, ini.c_str())));
    c.initial.tabX = (int)GetPrivateProfileIntW(L"NoteByNote", L"TabX", -810, ini.c_str());
    c.initial.tabY = (int)GetPrivateProfileIntW(L"NoteByNote", L"TabY", 385, ini.c_str());
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
    put(L"TabSeconds", st.tabSeconds);
    put(L"TabX", st.tabX);
    put(L"TabY", st.tabY);
}

bool SameSettings(const overlay::Settings& a, const overlay::Settings& b) {
    return a.enabled == b.enabled && a.leadMs == b.leadMs && a.earlyMs == b.earlyMs &&
           a.acceptOctaves == b.acceptOctaves && a.showBanner == b.showBanner && a.waitChords == b.waitChords &&
           a.showClock == b.showClock && a.showTab == b.showTab && a.tabSeconds == b.tabSeconds && a.tabX == b.tabX &&
           a.tabY == b.tabY;
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
    void Save(const std::wstring& dir, double songTime, long long fromPos, unsigned sr) {
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
    const Config cfg = LoadConfig();
    overlay::Settings st = cfg.initial;  // the live settings (the menu can change them)
    Log("config: enabled=%d menuKey=0x%X skipKey=0x%X lead=%dms early=%dms octaves=%d banner=%d chords=%d", st.enabled,
        cfg.menuKey, cfg.skipKey, st.leadMs, st.earlyMs, st.acceptOctaves, st.showBanner, st.waitChords);
    if (!game::Init()) return 0;
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
    std::vector<int> tabLevels;
    DWORD nextTabRefresh = 0;

    std::string menu, lastMenu, lastKey;
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
    DWORD frozenTick = 0, lastTapTry = 0, nextFreezeTry = 0, lastHeartbeat = 0, lastUnloadCheck = 0, nextChartTry = 0,
          songScreenTick = 0;
    long long totalSamples = 0;
    DebugAudio debugAudio;
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
            // The clock works even with the mode off or without a chart (it's just the song time).
            if (!inSong || !game::GetSongTime(&v.songTime)) v.songTime = -1;
            if (inSong && !game::GetSongLength(&v.songLength)) v.songLength = 0;
            // The scrolling tab: the notes the highway shows from 1 s ago to the end of the tab (+1 s
            // margin, the list is only refreshed every 50 ms). Works with the mode off too.
            if (inSong && chartOk && st.showTab && v.songTime >= 0) {
                if (now >= nextTabRefresh) {
                    nextTabRefresh = now + 50;
                    if (!game::GetPhraseLevels(&tabLevels)) tabLevels.clear();
                    chart.TargetsBetween(v.songTime - 1.0, v.songTime + st.tabSeconds + 1.0, tabLevels, &tabTargets);
                    tabNotes.clear();
                    for (const Target* t : tabTargets) {
                        overlay::TabNote tn;
                        tn.time = t->time;
                        tn.chord = t->chord;
                        tn.ignore = t->ignore;
                        if (t->chord) std::copy(std::begin(t->frets), std::end(t->frets), tn.frets);
                        else if (t->string >= 0 && t->string < 6) tn.frets[t->string] = t->fret;
                        tn.name = t->chordName;
                        tabNotes.push_back(tn);
                    }
                }
                v.tab = tabNotes;
            } else {
                tabNotes.clear();
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
        if (!SameSettings(newSt, st)) {
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
        const bool menuOk = game::GetMenu(&menu);
        if (now - lastHeartbeat > 5000) {  // what the mod sees, every 5 s (diagnostics)
            lastHeartbeat = now;
            double ht = -1;
            const bool tOk = game::GetSongTime(&ht);
            game::GetPhraseLevels(&levels);
            Log("status: menu=%s key=%s chart=%s enabled=%d t=%s%.3f cursor=%.3f frozen=%d hold=%d tap=%d samples=%lld levels=[%s]",
                menuOk ? menu.c_str() : "?", lastKey.c_str(), chartOk ? chart.arrangement.c_str() : "-", st.enabled,
                tOk ? "" : "(n/a)", ht, cursor, frozen, menuHold, tap.IsOpen(), totalSamples, Join(levels).c_str());
        }
        if (!menuOk) continue;
        if (menu != lastMenu) { Log("screen: %s", menu.c_str()); lastMenu = menu; }
        inSong = menu.size() >= cfg.menuSuffix.size() &&
                 menu.compare(menu.size() - cfg.menuSuffix.size(), std::string::npos, cfg.menuSuffix) == 0;
        std::string key;
        if (game::GetSongKey(&key)) lastKey = key;  // only valid while browsing songs

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
            if (skip) {
                Log("SKIP %.3f %s after waiting %.2f s", waitFor.time, Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
                debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);  // skipped = maybe not detected
                overlay::Toast("Skipped", 1200);
                cursor = waitFor.time;
                releaseWait();
                continue;
            }
            bool hit = false;
            for (const auto& ev : events) {
                if (Matches(st, chart, waitFor, ev.midi)) { hit = true; break; }
                Log("  heard %s (%+.0f cents, %.1f dB, aper %.2f%s), waiting for %s", MidiName(ev.midi).c_str(), ev.cents, ev.levelDb,
                    ev.aperiodicity, ev.attack ? ", attack" : "", Describe(chart, waitFor).c_str());
            }
            for (const auto& cr : chordResults) {  // only produced while waiting for a chord
                Log("  %s", cr.Describe().c_str());
                hit = hit || cr.match;
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
                frozenTick = now;
                waitAudioStart = debugAudio.Pos() - 2 * 48000;
                Log("WAIT %.3f (phrase iteration %d, level %d): play %s", next->time, next->pi, next->level,
                    Describe(chart, *next).c_str());
            }
        }
    }

    // Unload (dev): release the song if we're holding it, remove the overlay, then free the DLL.
    if (frozen || menuHold) game::Unfreeze();
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
