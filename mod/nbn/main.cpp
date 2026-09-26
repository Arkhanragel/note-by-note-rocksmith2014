// main.cpp: Note-by-Note for Rocksmith 2014, the mod's entry point and "wait mode" logic.
//
// Loaded by our RS_ASIO build (it loads NoteByNote.dll from the game folder at startup), or during
// development by nbn_inject.exe. Everything runs on one background thread:
//
//   every ~1 ms:  read new guitar samples from the GuitarTap -> NoteTracker -> note events
//                 read the game state (screen, song key, song clock, Dynamic Difficulty levels)
//                 WAIT MODE: when the clock reaches the next note ON THE HIGHWAY and it hasn't been
//                            played -> freeze the song; when the player plays it -> unfreeze
//
// "The next note on the highway" = the next note of the chart, where each phrase iteration uses its
// CURRENT Dynamic Difficulty level, read from game memory (see game.h / chart.h).
//
// Configuration: NoteByNote.ini next to the DLL (created with defaults on first run).
// Log: NoteByNote.log next to the DLL.
#include <windows.h>
#include <timeapi.h>

#include <cmath>
#include <cstdint>
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
    std::wstring arrangement = L"auto";  // auto = the chart matching what the game loaded
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
                "; Which chart to use: auto (the one matching the part the game loaded), or lead, rhythm, bass...\n"
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

std::string Join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

// Picks the chart for the song being played: among charts/<songkey>/*.nbn, the one whose note count
// per difficulty level equals what the game loaded, so it's exactly the arrangement on screen.
// (A forced Arrangement= in the ini wins if it matches too.)
bool LoadChartFor(const Config& cfg, const std::string& songKey, const std::vector<int>& gameCounts, Chart* chart) {
    const std::wstring dir = cfg.chartsDir + Lower(songKey) + L"\\";
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.nbn").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("chart: no charts for song key \"%s\" in %s. Run the chart exporter for your songs.", songKey.c_str(),
            Narrow(cfg.chartsDir).c_str());
        return false;
    }
    do files.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    if (cfg.arrangement != L"auto")  // try the configured one first
        for (size_t i = 0; i < files.size(); ++i)
            if (_wcsicmp(files[i].c_str(), (cfg.arrangement + L".nbn").c_str()) == 0) std::swap(files[0], files[i]);

    for (const auto& f : files) {
        Chart c;
        if (!c.Load(dir + f)) continue;
        if (c.levelCounts == gameCounts) {
            *chart = std::move(c);
            Log("chart: %s / %s \"%s\" (%d levels, %zu phrase iterations) matches the game", songKey.c_str(),
                Narrow(f).c_str(), chart->title.c_str(), chart->Levels(), chart->pis.size());
            return true;
        }
    }
    Log("chart: none of the %zu charts for \"%s\" matches the arrangement in the game (game levels: %s). "
        "Re-run the chart exporter (the song may have been updated).", files.size(), songKey.c_str(), Join(gameCounts).c_str());
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

bool Waitable(const Target& t) { return !t.chord && !t.ignore; }  // chords aren't supported yet

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

    std::string menu, lastMenu, lastKey;
    Chart chart;
    std::string chartFor;             // song key the current chart was matched for ("" = none yet)
    bool chartOk = false, frozen = false;
    std::vector<int> levels, gameCounts;
    double cursor = 0;                // song time of the last note that was hit/passed
    double lastT = -1;
    Target waitFor;                   // the note we're frozen on
    DWORD frozenTick = 0, lastTapTry = 0, nextFreezeTry = 0, lastHeartbeat = 0, lastUnloadCheck = 0, nextChartTry = 0;
    long long totalSamples = 0;
    DebugAudio debugAudio;
    long long waitAudioStart = 0;
    const std::wstring debugDir = DllDir() + L"NoteByNote_debug\\";

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
        debugAudio.Push(samples);
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
            if (!enabled && frozen) { game::Unfreeze(); frozen = false; debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000); }
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
            game::GetPhraseLevels(&levels);
            Log("status: menu=%s key=%s chart=%s enabled=%d t=%s%.3f cursor=%.3f frozen=%d tap=%d samples=%lld levels=[%s]",
                menuOk ? menu.c_str() : "?", lastKey.c_str(), chartOk ? chart.arrangement.c_str() : "-", enabled,
                tOk ? "" : "(n/a)", ht, cursor, frozen, tap.IsOpen(), totalSamples, Join(levels).c_str());
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
            if (frozen) {
                Log("left the song screen while waiting; releasing");
                debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);
                frozen = false;
            }
            game::ResetSongCache();
            lastT = -1;
            if (lastKey != chartFor) chartOk = false;  // a different song was selected
            continue;
        }

        // ---- 4. chart for the arrangement the game loaded (retry until the song data is readable)
        if ((!chartOk || chartFor != lastKey) && !lastKey.empty() && now >= nextChartTry) {
            nextChartTry = now + 2000;
            if (game::GetLevelNoteCounts(&gameCounts) && !gameCounts.empty()) {
                chartFor = lastKey;
                chartOk = LoadChartFor(cfg, lastKey, gameCounts, &chart);
                if (!chartOk) nextChartTry = now + 30000;  // don't spam the log
                if (chartOk && chart.bass != trackerIsBass) {
                    trackerIsBass = chart.bass;
                    tracker = NoteTracker(trackerIsBass ? bassCfg : guitarCfg);
                }
                lastT = -1;
            }
        }
        double t;
        if (!chartOk || !enabled || !game::GetSongTime(&t)) continue;
        if (!game::GetPhraseLevels(&levels)) levels.clear();

        // ---- 5. keep the cursor in sync with the song position
        // (song start, Riff Repeater loops, the rewind after the game's pause, the mode switched on...)
        if (!frozen && (lastT < 0 || t < lastT - 0.25 || t > lastT + 1.0)) {
            if (lastT >= 0) Log("song position jumped %.2f -> %.2f s", lastT, t);
            cursor = t - 0.05;
        }
        lastT = t;

        // ---- 6. the player's notes
        if (frozen) {
            for (const auto& ev : events) {
                if (Matches(cfg, waitFor, ev.midi)) {
                    game::Unfreeze();
                    frozen = false;
                    cursor = waitFor.time;
                    Log("HIT  %.3f %s after waiting %.2f s", waitFor.time, Describe(chart, waitFor).c_str(), (now - frozenTick) / 1000.0);
                    if (now - frozenTick > 3000) debugAudio.Save(debugDir, waitFor.time, waitAudioStart, 48000);  // long waits only
                    break;
                }
                Log("  heard %s (%+.0f cents, %.1f dB, aper %.2f%s), waiting for %s", MidiName(ev.midi).c_str(), ev.cents, ev.levelDb,
                    ev.aperiodicity, ev.attack ? ", attack" : "", Describe(chart, waitFor).c_str());
            }
            continue;
        }

        // Next note on the highway (using the current level of each phrase), skipping chords/ignored
        // notes, which we don't wait on (yet).
        const Target* next = chart.NextTarget(cursor, levels);
        while (next && !Waitable(*next) && next->time <= t + cfg.earlyS) {
            cursor = next->time;
            next = chart.NextTarget(cursor, levels);
        }
        if (!next) continue;  // end of the chart

        for (const auto& ev : events) {
            if (Waitable(*next) && Matches(cfg, *next, ev.midi) && t >= next->time - cfg.earlyS) {
                // played on time (or a little early): no need to stop
                Log("hit  %.3f %s on time (%+.0f ms, level %d)", next->time, Describe(chart, *next).c_str(),
                    (t - next->time) * 1000.0, next->level);
                cursor = next->time;
                next = chart.NextTarget(cursor, levels);
                break;
            }
        }
        if (!next || !Waitable(*next)) continue;

        // ---- 7. reached the next note without it being played -> wait for it
        if (t >= next->time - cfg.leadS && now >= nextFreezeTry) {
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
