// main.cpp: Note-by-Note for Rocksmith 2014, the mod's entry point and "wait mode" logic.
//
// Loaded by our RS_ASIO build (it loads NoteByNote.dll from the game folder at startup), or during
// development by nbn_inject.exe. Everything runs on one background thread:
//
//   every ~1 ms:  read new guitar samples from the GuitarTap -> NoteTracker -> note events
//                 read the game state (menu, song key, song clock)
//                 WAIT MODE: if the clock reaches the next note and it hasn't been played -> freeze
//                            the song; when the player plays it -> unfreeze and move on
//
// Configuration: NoteByNote.ini next to the DLL (created with defaults on first run).
// Log: NoteByNote.log next to the DLL.
#include <windows.h>
#include <timeapi.h>

#include <cmath>
#include <string>
#include <vector>

#include "chart.h"
#include "detector.h"
#include "game.h"
#include "log.h"
#include "tap.h"

namespace nbn {
namespace {

HMODULE g_self = nullptr;

// ------------------------------------------------------------------ configuration
struct Config {
    bool enabledAtStart = true;
    int toggleKey = VK_F8;
    std::wstring arrangement = L"auto";  // auto | lead | rhythm | bass | lead2 | ...
    std::wstring chartsDir;              // absolute
    double leadS = 0.0;                  // freeze this long BEFORE the note time
    double earlyS = 0.30;                // a correct note up to this early counts without freezing
    bool acceptOctaves = false;          // accept the right note name in another octave
    std::string menuSuffix = "_Game";    // the mode only acts on screens whose name ends like this
    std::string toggleSound = "Nav_InGame_Options";
};

std::wstring DllDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

int ParseKey(const std::wstring& k) {
    if (k.size() >= 2 && (k[0] == L'F' || k[0] == L'f')) {
        int n = _wtoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (k.rfind(L"0x", 0) == 0) return (int)wcstol(k.c_str(), nullptr, 16);
    return VK_F8;
}

Config LoadConfig() {
    const std::wstring ini = DllDir() + L"NoteByNote.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First run: write a commented default file the player can edit.
        FILE* f = _wfopen(ini.c_str(), L"w");
        if (f) {
            std::fputs(
                "; Note-by-Note for Rocksmith 2014 - settings\n"
                "; The song waits at each single note until you play it.\n"
                "[NoteByNote]\n"
                "; 1 = the mode starts switched on, 0 = off (switch it with the toggle key while playing)\n"
                "Enabled=1\n"
                "; Key that switches the mode on/off during a song (F1..F12). Avoid F10 (Windows menu key)\n"
                "; and F12 (Steam screenshot).\n"
                "ToggleKey=F8\n"
                "; Which part you play: auto (lead, then rhythm, then bass), lead, rhythm, bass, lead2, ...\n"
                "Arrangement=auto\n"
                "; Folder with the charts made by the chart exporter (relative to this file)\n"
                "ChartsDir=NoteByNote_charts\n"
                "; Stop this many milliseconds BEFORE the note reaches the line (0 = exactly on it)\n"
                "LeadMs=0\n"
                "; A correct note played up to this many milliseconds early counts without stopping\n"
                "EarlyMs=300\n"
                "; 1 = the same note one octave higher/lower also counts\n"
                "AcceptOctaves=0\n",
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
    c.enabledAtStart = GetPrivateProfileIntW(L"NoteByNote", L"Enabled", 1, ini.c_str()) != 0;
    c.toggleKey = ParseKey(str(L"ToggleKey", L"F8"));
    c.arrangement = str(L"Arrangement", L"auto");
    std::wstring dir = str(L"ChartsDir", L"NoteByNote_charts");
    c.chartsDir = (dir.size() > 1 && dir[1] == L':') ? dir : DllDir() + dir;
    if (!c.chartsDir.empty() && c.chartsDir.back() != L'\\') c.chartsDir += L'\\';
    c.leadS = GetPrivateProfileIntW(L"NoteByNote", L"LeadMs", 0, ini.c_str()) / 1000.0;
    c.earlyS = GetPrivateProfileIntW(L"NoteByNote", L"EarlyMs", 300, ini.c_str()) / 1000.0;
    c.acceptOctaves = GetPrivateProfileIntW(L"NoteByNote", L"AcceptOctaves", 0, ini.c_str()) != 0;
    return c;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }  // ASCII paths/names only

std::wstring Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return std::wstring(s.begin(), s.end());
}

// Finds and loads the chart for a song key: <charts>/<songkey>/<arrangement>.nbn
bool LoadChartFor(const Config& cfg, const std::string& songKey, Chart* chart) {
    const std::wstring dir = cfg.chartsDir + Lower(songKey) + L"\\";
    std::vector<std::wstring> tries;
    if (cfg.arrangement != L"auto") tries.push_back(cfg.arrangement);
    for (const wchar_t* a : {L"lead", L"rhythm", L"bass", L"combo", L"lead2", L"rhythm2", L"bass2"}) tries.push_back(a);
    for (const auto& a : tries) {
        if (chart->Load(dir + a + L".nbn")) {
            Log("chart: %s / %s  \"%s\"  (%zu targets)", songKey.c_str(), Narrow(a).c_str(), chart->title.c_str(),
                chart->targets.size());
            return true;
        }
    }
    // Lessons have their own suffix (e.g. "lsn50"): take any chart in the folder.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.nbn").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        const std::wstring file = dir + fd.cFileName;
        FindClose(h);
        if (chart->Load(file)) {
            Log("chart: %s / %s (only chart available)", songKey.c_str(), Narrow(fd.cFileName).c_str());
            return true;
        }
    }
    Log("chart: none for song key \"%s\" in %s. Run the chart exporter for your songs.", songKey.c_str(),
        Narrow(cfg.chartsDir).c_str());
    return false;
}

// Friendly note description for the log (the future overlay will use the same words).
std::string Describe(const Chart& chart, const Target& t) {
    static const char* gtr[] = {"6th string (thickest)", "5th string", "4th string", "3rd string", "2nd string", "1st string (thinnest)"};
    static const char* bass[] = {"4th string (thickest)", "3rd string", "2nd string", "1st string (thinnest)"};
    if (t.chord) return "chord";
    const char* s = (t.string >= 0 && t.string < (chart.bass ? 4 : 6)) ? (chart.bass ? bass : gtr)[t.string] : "?";
    char buf[128];
    if (t.fret == 0) std::snprintf(buf, sizeof(buf), "%s, open (%s)", s, MidiName(t.midi[0]).c_str());
    else std::snprintf(buf, sizeof(buf), "%s, fret %d (%s)", s, t.fret, MidiName(t.midi[0]).c_str());
    return buf;
}

bool Matches(const Config& cfg, const Target& t, int midi) {
    if (t.chord || t.midi.empty()) return false;
    const int d = midi - t.midi[0];
    return d == 0 || (cfg.acceptOctaves && d % 12 == 0);
}

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// ------------------------------------------------------------------ main loop
DWORD WINAPI MainThread(LPVOID) {
    LogOpen(DllDir() + L"NoteByNote.log");
    Log("Note-by-Note starting");
    const Config cfg = LoadConfig();
    Log("config: enabled=%d toggleKey=0x%X arrangement=%s charts=%s lead=%.3fs early=%.3fs octaves=%d", cfg.enabledAtStart,
        cfg.toggleKey, Narrow(cfg.arrangement).c_str(), Narrow(cfg.chartsDir).c_str(), cfg.leadS, cfg.earlyS, cfg.acceptOctaves);
    if (!game::Init()) return 0;

    bool enabled = cfg.enabledAtStart, keyDown = false;
    TapReader tap;
    TrackerConfig guitarCfg, bassCfg;
    bassCfg.window = 4096;
    bassCfg.fmin = 35.0;
    NoteTracker tracker(guitarCfg);
    bool trackerIsBass = false;
    std::vector<float> samples, pending;
    std::vector<NoteEvent> events;

    std::string menu, lastMenu, lastKey, chartKey;
    Chart chart;
    bool chartOk = false, frozen = false;
    size_t idx = 0;
    double lastT = -1, frozenAt = 0;
    DWORD frozenTick = 0, lastTapTry = 0, nextFreezeTry = 0, lastHeartbeat = 0, lastUnloadCheck = 0;
    long long totalSamples = 0;

    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        const DWORD now = GetTickCount();

        // ---- 1. guitar -> note events
        if (!tap.IsOpen() && now - lastTapTry > 1000) {
            lastTapTry = now;
            if (tap.Open()) Log("guitar input connected (GuitarTap, %u Hz)", tap.SampleRate());
        }
        events.clear();
        samples.clear();
        tap.ReadNew(samples);
        totalSamples += (long long)samples.size();
        pending.insert(pending.end(), samples.begin(), samples.end());
        size_t used = 0;
        for (; used + NoteTracker::kBlock <= pending.size(); used += NoteTracker::kBlock) {
            NoteEvent ev;
            if (tracker.Process(&pending[used], &ev)) events.push_back(ev);
        }
        pending.erase(pending.begin(), pending.begin() + used);

        // ---- 2. on/off key (only while the game window is focused)
        const bool down = GameFocused() && (GetAsyncKeyState(cfg.toggleKey) & 0x8000);
        if (down && !keyDown) {
            enabled = !enabled;
            Log("Note-by-Note %s", enabled ? "ON" : "OFF");
            game::PostUiEvent(cfg.toggleSound.c_str());
            if (!enabled && frozen) { game::Unfreeze(); frozen = false; }
            if (enabled) lastT = -1;  // re-sync to the current position
        }
        keyDown = down;

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
            Log("status: menuOk=%d menu=%s key=%s chartOk=%d enabled=%d t=%s%.3f idx=%zu frozen=%d tapOpen=%d samples=%lld",
                menuOk, menu.c_str(), lastKey.c_str(), chartOk, enabled, tOk ? "" : "(n/a)", ht, idx, frozen,
                tap.IsOpen(), totalSamples);
        }
        if (!menuOk) continue;
        if (menu != lastMenu) { Log("screen: %s", menu.c_str()); lastMenu = menu; }
        const bool inSong = menu.size() >= cfg.menuSuffix.size() &&
                            menu.compare(menu.size() - cfg.menuSuffix.size(), std::string::npos, cfg.menuSuffix) == 0;
        std::string key;
        if (game::GetSongKey(&key)) lastKey = key;  // only valid while browsing songs

        if (!inSong) {
            // Pause menu, song end, other screens: the game is in charge. If we were holding the song,
            // just forget it (the game's own pause stops/restarts the music and resets its clock flag).
            if (frozen) { Log("left the song screen while waiting; releasing"); frozen = false; }
            game::ResetSongCache();
            lastT = -1;
            continue;
        }

        // ---- 4. chart for this song
        if (lastKey != chartKey) {
            chartKey = lastKey;
            chartOk = !chartKey.empty() && LoadChartFor(cfg, chartKey, &chart);
            if (chartOk && chart.bass != trackerIsBass) {
                trackerIsBass = chart.bass;
                tracker = NoteTracker(trackerIsBass ? bassCfg : guitarCfg);
            }
            lastT = -1;
        }
        double t;
        if (!chartOk || !enabled || !game::GetSongTime(&t)) continue;
        const auto& targets = chart.targets;

        // ---- 5. keep the note pointer in sync with the song position
        // (start of the song, Riff Repeater loops, rewinds after the game's pause, the mode switched on...)
        if (!frozen && (lastT < 0 || t < lastT - 0.25 || t > lastT + 1.0)) {
            idx = chart.FirstAtOrAfter(t - 0.05);
            if (lastT >= 0) Log("song position jumped %.2f -> %.2f s; next note #%zu", lastT, t, idx);
        }
        lastT = t;
        // chords aren't supported yet: skip them (never wait on them)
        while (idx < targets.size() && targets[idx].chord && targets[idx].time <= t + cfg.earlyS) ++idx;
        if (idx >= targets.size()) continue;  // end of the chart
        const Target& next = targets[idx];

        // ---- 6. the player's notes
        for (const auto& ev : events) {
            if (!Matches(cfg, next, ev.midi)) {
                if (frozen) Log("  heard %s, waiting for %s", MidiName(ev.midi).c_str(), Describe(chart, next).c_str());
                continue;
            }
            if (frozen) {
                game::Unfreeze();
                frozen = false;
                Log("HIT  #%zu %s after waiting %.2f s", idx, Describe(chart, next).c_str(), (now - frozenTick) / 1000.0);
                ++idx;
                break;
            }
            if (t >= next.time - cfg.earlyS) {  // on time (or a little early): no need to stop
                Log("hit  #%zu %s on time (%+.0f ms)", idx, Describe(chart, next).c_str(), (t - next.time) * 1000.0);
                ++idx;
                break;
            }
        }
        if (idx >= targets.size() || frozen) continue;

        // ---- 7. reached the next note without it being played -> wait for it
        const Target& due = targets[idx];
        if (!due.chord && t >= due.time - cfg.leadS && now >= nextFreezeTry) {
            nextFreezeTry = now + 500;  // if freezing fails (e.g. the song is still loading), retry in 0.5 s
            if (game::Freeze()) {
                frozen = true;
                frozenAt = t;
                frozenTick = now;
                Log("WAIT #%zu at %.3f s: play %s", idx, frozenAt, Describe(chart, due).c_str());
            }
        }
    }

    // Unload (dev): release the song if we're holding it, then free the DLL.
    if (frozen) game::Unfreeze();
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
