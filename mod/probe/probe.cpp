// NoteByNoteProbe.dll: Phase 3 probe. It runs INSIDE Rocksmith2014.exe (loaded by nbn_inject).
//
// It doesn't change gameplay unless you press the test keys. What it does:
//   * checks that the exe is the version our addresses are for (PE checksum)
//   * logs the current menu/screen name and the song timer, including the timer's effective
//     speed (seconds of song per real second: 1.00 = normal, 0.50 = half speed, 0 = frozen)
//   * logs every audio event the game sends to Wwise (PostEvent) and every audio parameter
//     change (SetRTPCValue, rate-limited). This is how we'll learn what "pause" does inside
//   * test keys (only while the game window is focused):
//       F5  = lower the song speed one step (100 -> 75 -> 50 -> 25 -> 10 -> 5 -> 1 %)
//       F6  = back to 100 %
//       F7  = log the current Time_Stretch value
//       F8  = write a numbered MARK line in the log ("I just paused", etc.)
//       F12 = switch off the probe's hooks (safety switch)
//
// BACKGROUND
// - Addresses: RSMods' research for the "LPDecember2024" build (see BITACORA.md). They're
//   relative to the exe's load address ("base"), which is why we add GetModuleHandle(NULL).
// - A "pointer chain" means: start at base+X, read the pointer stored there, add an
//   offset, read again... The game allocates its objects dynamically, so the only fixed
//   address is the first one.
// - Wwise is the audio engine (by Audiokinetic) that's compiled into the game. The game talks to
//   it with "events" (PostEvent("Play_Something")) and "RTPCs" (Real-Time Parameter Controls:
//   named numbers like volume or Time_Stretch that the sound design reacts to).
// - Hooking with MinHook: MinHook overwrites the first bytes of a game function with a jump
//   to our "detour" function. It also builds a "trampoline" (orig_*) that runs the overwritten
//   bytes and jumps back, so our detour can log and then call the original.

#include <windows.h>
#include <imagehlp.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

// ------------------------------------------------------------------------------------ addresses
namespace addr {
constexpr DWORD kExpectedChecksum = 0x0176EC34;  // PE checksum of the Dec 2024 Steam build

constexpr uintptr_t kRoot = 0x00F6062C;              // root pointer shared by the timer and the menu chains
constexpr uint32_t kTimerChain[] = {0xB0, 0x538, 0x8};  // -> float: song time in seconds
constexpr uint32_t kMenuChain[] = {0x28, 0x8C, 0x0};    // -> char[]: internal menu/screen name

constexpr uintptr_t kPostEventChar = 0x00AC4870;
constexpr uintptr_t kPostEventId = 0x00AC5400;
constexpr uintptr_t kSetRtpcChar = 0x00AC2400;
constexpr uintptr_t kSetRtpcId = 0x00AC0BB0;
constexpr uintptr_t kGetRtpcChar = 0x00AC0280;
}  // namespace addr

// ------------------------------------------------------------------------------------ Wwise types
// Wwise's own typedefs for a 32-bit build. The detours must match these EXACTLY (and __cdecl),
// or the stack gets corrupted and the game crashes.
using AkUInt32 = uint32_t;
using AkPlayingID = uint32_t;
using AkUniqueID = uint32_t;
using AkGameObjectID = uintptr_t;  // 32-bit here
using AkTimeMs = int32_t;
using AkRtpcValue = float;
using AKRESULT = int;
constexpr AkGameObjectID AK_INVALID_GAME_OBJECT = (AkGameObjectID)-1;  // "global" (all objects)
constexpr AkGameObjectID kSongGameObject = 0x1234;  // the object the game uses for the song (from RSMods)

using PostEventChar_t = AkPlayingID(__cdecl*)(const char*, AkGameObjectID, AkUInt32, void*, void*, AkUInt32, void*, AkPlayingID);
using PostEventId_t = AkPlayingID(__cdecl*)(AkUniqueID, AkGameObjectID, AkUInt32, void*, void*, AkUInt32, void*, AkPlayingID);
using SetRtpcChar_t = AKRESULT(__cdecl*)(const char*, AkRtpcValue, AkGameObjectID, AkTimeMs, int /*curve*/);
using SetRtpcId_t = AKRESULT(__cdecl*)(AkUniqueID, AkRtpcValue, AkGameObjectID, AkTimeMs, int /*curve*/);
using GetRtpcChar_t = AKRESULT(__cdecl*)(const char*, AkGameObjectID, AkRtpcValue*, int* /*RTPCValue_type*/);

// ------------------------------------------------------------------------------------ globals
static HMODULE g_self;
static uintptr_t g_base;
static FILE* g_log;
static std::mutex g_logMutex;
static DWORD g_t0;
static std::atomic<bool> g_hooksOn{false};
static bool g_setRtpcOk = false;  // SetRTPCValue(char*) passed the safety check (F5/F6 allowed)
static bool g_getRtpcOk = false;  // GetRTPCValue(char*) passed the safety check (F7 allowed)

static PostEventChar_t orig_PostEventChar;
static PostEventId_t orig_PostEventId;
static SetRtpcChar_t orig_SetRtpcChar;
static SetRtpcId_t orig_SetRtpcId;

// ------------------------------------------------------------------------------------ logging
// Thread-safe: game threads (through the hooks) and our own thread all write here.
static void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    fprintf(g_log, "[%8.3f] %s\n", (GetTickCount() - g_t0) / 1000.0, msg);
    fflush(g_log);  // flush every line, so the log survives a crash
}

// ------------------------------------------------------------------------------------ safe memory reads
// Following a pointer chain can hit unmapped memory (e.g. while a menu is loading). __try/__except
// is Windows "structured exception handling". It catches the access violation instead of crashing
// the game. (This function must not create C++ objects with destructors; that's an MSVC rule for __try.)
static bool ReadChain(uintptr_t start, const uint32_t* offsets, size_t count, uintptr_t* out) {
    __try {
        uintptr_t a = start;
        for (size_t i = 0; i < count; ++i) {
            a = *(uintptr_t*)a;
            if (!a) return false;
            a += offsets[i];
        }
        *out = a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadFloat(uintptr_t a, float* out) {
    __try { *out = *(float*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Copies a printable string of up to cap-1 chars. Returns false if it doesn't look like text.
static bool ReadText(uintptr_t a, char* buf, size_t cap) {
    __try {
        const char* s = (const char*)a;
        size_t i = 0;
        for (; i < cap - 1 && s[i]; ++i) {
            if (s[i] < 32 || s[i] > 126) return false;
            buf[i] = s[i];
        }
        buf[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GetSongTime(float* t) {
    uintptr_t a;
    return ReadChain(g_base + addr::kRoot, addr::kTimerChain, 3, &a) && ReadFloat(a, t);
}

static bool GetMenu(char* buf, size_t cap) {
    uintptr_t a;
    return ReadChain(g_base + addr::kRoot, addr::kMenuChain, 3, &a) && ReadText(a, buf, cap);
}

// ------------------------------------------------------------------------------------ hooks (detours)
static AkPlayingID __cdecl Hook_PostEventChar(const char* name, AkGameObjectID obj, AkUInt32 flags, void* cb,
                                              void* cookie, AkUInt32 nExt, void* ext, AkPlayingID pid) {
    AkPlayingID r = orig_PostEventChar(name, obj, flags, cb, cookie, nExt, ext, pid);
    char safe[128] = "?";
    if (name) ReadText((uintptr_t)name, safe, sizeof(safe));
    Log("EVENT  %-48s obj=0x%X flags=0x%X -> playingID=%u", safe, (unsigned)obj, flags, r);
    return r;
}

static AkPlayingID __cdecl Hook_PostEventId(AkUniqueID id, AkGameObjectID obj, AkUInt32 flags, void* cb,
                                            void* cookie, AkUInt32 nExt, void* ext, AkPlayingID pid) {
    AkPlayingID r = orig_PostEventId(id, obj, flags, cb, cookie, nExt, ext, pid);
    Log("EVENT  id=%u obj=0x%X flags=0x%X -> playingID=%u", id, (unsigned)obj, flags, r);
    return r;
}

// RTPCs can be set every frame (e.g. meters, crowd), so log them only when they change noticeably,
// at most about once per second per parameter. Time_Stretch is always logged when it changes.
struct RtpcSeen { float value; DWORD tick; };
static std::unordered_map<std::string, RtpcSeen> g_rtpcSeen;
static std::mutex g_rtpcMutex;

static void LogRtpc(const std::string& key, float v, AkGameObjectID obj) {
    DWORD now = GetTickCount();
    bool isStretch = key == "Time_Stretch";
    {
        std::lock_guard<std::mutex> lock(g_rtpcMutex);
        auto it = g_rtpcSeen.find(key);
        if (it != g_rtpcSeen.end()) {
            float d = std::fabs(v - it->second.value);
            bool changed = d > 0.05f * std::fmax(std::fabs(it->second.value), 1.0f);
            if (!changed || (!isStretch && now - it->second.tick < 1000)) return;
        }
        g_rtpcSeen[key] = {v, now};
    }
    Log("RTPC   %-48s = %10.3f  obj=0x%X", key.c_str(), v, (unsigned)obj);
}

static AKRESULT __cdecl Hook_SetRtpcChar(const char* name, AkRtpcValue v, AkGameObjectID obj, AkTimeMs ms, int curve) {
    char safe[96] = "?";
    if (name) ReadText((uintptr_t)name, safe, sizeof(safe));
    LogRtpc(safe, v, obj);
    return orig_SetRtpcChar(name, v, obj, ms, curve);
}

static AKRESULT __cdecl Hook_SetRtpcId(AkUniqueID id, AkRtpcValue v, AkGameObjectID obj, AkTimeMs ms, int curve) {
    LogRtpc("id:" + std::to_string(id), v, obj);
    return orig_SetRtpcId(id, v, obj, ms, curve);
}

// Safety check before hooking. The exe is encrypted on disk and decrypts itself when it starts, so
// we can only check the code here, in memory. MSVC puts functions at 16-byte-aligned addresses,
// padded with int3 (0xCC) bytes, or right after the previous function's "ret" (0xC3 / 0xC2 nn).
// Real function starts usually begin with push ebp (55), mov edi,edi (8B FF), sub esp (83 EC / 81 EC),
// push reg (50-57), push imm (6A / 68) or a mov (8B). If neither the padding nor the first byte
// looks right, we don't hook it, because patching the middle of an instruction would crash the game.
static bool LooksLikeFunctionStart(uintptr_t a) {
    __try {
        const uint8_t* p = (const uint8_t*)a;
        uint8_t before = p[-1], first = p[0];
        bool paddingOk = before == 0xCC || before == 0xC3 || before == 0x90 || p[-3] == 0xC2;
        bool prologueOk = first == 0x55 || first == 0x8B || first == 0x83 || first == 0x81 ||
                          (first >= 0x50 && first <= 0x57) || first == 0x6A || first == 0x68 || first == 0xA1;
        return (a % 16 == 0) && paddingOk && prologueOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogBytes(const char* name, uintptr_t a) {
    char hex[3 * 20 + 1] = "";
    __try {
        for (int i = -4; i < 16; ++i) sprintf_s(hex + 3 * (i + 4), 4, "%02X ", ((const uint8_t*)a)[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(hex, "<unreadable>");
    }
    Log("bytes %-22s [-4..+16]: %s", name, hex);
}

static bool InstallHooks() {
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return false; }
    struct { uintptr_t rva; void* detour; void** orig; const char* name; } hooks[] = {
        {addr::kPostEventChar, (void*)&Hook_PostEventChar, (void**)&orig_PostEventChar, "PostEvent(char*)"},
        {addr::kPostEventId, (void*)&Hook_PostEventId, (void**)&orig_PostEventId, "PostEvent(id)"},
        {addr::kSetRtpcChar, (void*)&Hook_SetRtpcChar, (void**)&orig_SetRtpcChar, "SetRTPCValue(char*)"},
        {addr::kSetRtpcId, (void*)&Hook_SetRtpcId, (void**)&orig_SetRtpcId, "SetRTPCValue(id)"},
    };
    bool ok = true;
    for (auto& h : hooks) {
        LogBytes(h.name, g_base + h.rva);
        if (!LooksLikeFunctionStart(g_base + h.rva)) {
            Log("hook %-22s SKIPPED: doesn't look like a function start (wrong address for this build?)", h.name);
            ok = false;
            continue;
        }
        if (h.rva == addr::kSetRtpcChar) g_setRtpcOk = true;
        MH_STATUS s = MH_CreateHook((void*)(g_base + h.rva), h.detour, h.orig);
        Log("hook %-22s @ 0x%08X : %s", h.name, (unsigned)(g_base + h.rva), MH_StatusToString(s));
        ok &= (s == MH_OK);
    }
    // Test 1 found that the hooks never fired, so now we log the result of enabling them and
    // re-read the bytes. After a successful enable, each function must start with E9 (JMP to our detour).
    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    Log("MH_EnableHook(all): %s", MH_StatusToString(en));
    for (auto& h : hooks) LogBytes(h.name, g_base + h.rva);
    g_hooksOn = ok && en == MH_OK;
    return g_hooksOn;
}

// ------------------------------------------------------------------------------------ time-stretch effect
// Test 1: setting Time_Stretch in normal Learn a Song did nothing. RSMods shows why: the time-stretch
// *effect* must be attached to the song's Wwise "actor-mixer" (the audio node that holds the song's
// music). Riff Repeater does that when you pick a speed under 100%. Outside RR we have to do it
// ourselves:
//   1. find the song key (e.g. "notegel1"): the game keeps the name of the preview audio event,
//      "Play_<songkey>_Preview", at a known pointer
//   2. ask Wwise which audio objects the "Play_<songkey>" event uses. The first one is the actor-mixer
//   3. SetActorMixerEffect(actorMixer, slot 2, Default_Time_Stretch). Slot 2 is where RSMods puts it
namespace addr {
constexpr uintptr_t kPreviewName = 0x00F60514;             // -> "Play_<songkey>_Preview"
constexpr uint32_t kPreviewChain[] = {0xBC, 0x0};
constexpr uintptr_t kQueryAudioObjectIDsChar = 0x00AC06B0;
constexpr uintptr_t kSetActorMixerEffect = 0x00AC1D40;
}  // namespace addr

struct AkObjectInfo { AkUniqueID objID; AkUniqueID parentID; int32_t depth; };
using QueryAudioObjectIDs_t = AKRESULT(__cdecl*)(const char*, AkUInt32*, AkObjectInfo*);
using SetActorMixerEffect_t = AKRESULT(__cdecl*)(AkUniqueID node, AkUInt32 fxIndex, AkUniqueID shareSet);
constexpr AkUniqueID kDefaultTimeStretch = 0xB3745FC2;  // Wwise ID of the "Default_Time_Stretch" effect ShareSet
constexpr AkUInt32 kTimeStretchSlot = 2;

static bool GetSongKey(char* key, size_t cap) {
    uintptr_t a;
    char name[128];
    if (!ReadChain(g_base + addr::kPreviewName, addr::kPreviewChain, 2, &a) || !ReadText(a, name, sizeof(name)))
        return false;
    std::string s(name);  // "Play_notegel1_Preview"
    if (s.rfind("Play_", 0) != 0) return false;
    size_t end = s.rfind("_Preview");
    if (end == std::string::npos) end = s.rfind("_Invalid");  // song previews switched off in the options
    if (end == std::string::npos || end <= 5) return false;
    strcpy_s(key, cap, s.substr(5, end - 5).c_str());
    return true;
}

static void SetTimeStretchEffect(bool on) {
    for (uintptr_t f : {addr::kQueryAudioObjectIDsChar, addr::kSetActorMixerEffect}) {
        if (!LooksLikeFunctionStart(g_base + f)) { LogBytes("Wwise query/effect", g_base + f); Log(">>> effect: function check failed"); return; }
    }
    char key[96];
    if (!GetSongKey(key, sizeof(key))) { Log(">>> effect: song key not found (enter a song first)"); return; }
    std::string ev = std::string("Play_") + key;

    auto query = (QueryAudioObjectIDs_t)(g_base + addr::kQueryAudioObjectIDsChar);
    AkUInt32 n = 0;
    AKRESULT r1 = query(ev.c_str(), &n, nullptr);  // first call: only asks how many objects
    if (n == 0 || n > 64) { Log(">>> effect: QueryAudioObjectIDs(%s) count -> result=%d n=%u", ev.c_str(), r1, n); return; }
    AkObjectInfo objs[64];
    AKRESULT r2 = query(ev.c_str(), &n, objs);
    for (AkUInt32 i = 0; i < n && i < 4; ++i)
        Log(">>> effect: object[%u] id=0x%08X parent=0x%08X depth=%d", i, objs[i].objID, objs[i].parentID, objs[i].depth);

    auto setFx = (SetActorMixerEffect_t)(g_base + addr::kSetActorMixerEffect);
    AKRESULT r3 = setFx(objs[0].objID, kTimeStretchSlot, on ? kDefaultTimeStretch : 0);
    Log(">>> effect %s on %s (node 0x%08X): query=%d/%d setFx=%d", on ? "ON" : "OFF", ev.c_str(), objs[0].objID, r1, r2, r3);
}

// ------------------------------------------------------------------------------------ speed experiments
// Time_Stretch semantics (from RSMods): the RTPC is "percent of normal duration", so
// RTPC = 10000 / speed%. 100% -> 100, 50% -> 200, 1% -> 10000.
// The game may clamp the RTPC range. Finding the lowest usable speed is one goal of this probe.
static void SetSpeed(float speedPercent) {
    if (!g_setRtpcOk) { Log(">>> SetSpeed ignored: SetRTPCValue failed the safety check"); return; }
    auto set = (SetRtpcChar_t)(g_base + addr::kSetRtpcChar);  // through the hook, so it's logged too
    float rtpc = 10000.0f / speedPercent;
    AKRESULT a = set("Time_Stretch", rtpc, kSongGameObject, 0, 4 /*linear*/);
    AKRESULT b = set("Time_Stretch", rtpc, AK_INVALID_GAME_OBJECT, 0, 4);
    Log(">>> SetSpeed %.0f%% (Time_Stretch=%.1f) results: songObj=%d global=%d", speedPercent, rtpc, a, b);
}

static void LogStretch() {
    if (!g_getRtpcOk) { Log(">>> GetRTPC ignored: GetRTPCValue failed the safety check"); return; }
    auto get = (GetRtpcChar_t)(g_base + addr::kGetRtpcChar);
    for (AkGameObjectID obj : {kSongGameObject, AK_INVALID_GAME_OBJECT}) {
        float v = -1;
        int type = 2;  // RTPCValue_GameObject: "value for this object, falling back to global"
        AKRESULT r = get("Time_Stretch", obj, &v, &type);
        Log(">>> GetRTPC Time_Stretch obj=0x%X -> result=%d value=%.3f type=%d", (unsigned)obj, r, v, type);
    }
}

// ------------------------------------------------------------------------------------ main loop
static bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool Pressed(int vk, bool& wasDown) {  // true on the key-down edge only
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

static DWORD WINAPI MainThread(LPVOID) {
    // Log file next to the DLL (in our project folder, so the game folder stays untouched).
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring logPath(path);
    logPath = logPath.substr(0, logPath.find_last_of(L"\\/") + 1) + L"NoteByNoteProbe.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    g_t0 = GetTickCount();

    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    DWORD headerSum = 0, checksum = 0;
    MapFileAndCheckSumA(exe, &headerSum, &checksum);
    Log("NoteByNoteProbe loaded. exe=%s base=0x%08X checksum=0x%08X", exe, (unsigned)g_base, checksum);

    if (checksum != addr::kExpectedChecksum) {
        // Wrong game version: our addresses would point at random code. Do nothing.
        Log("UNSUPPORTED exe version (expected 0x%08X). Probe disabled, nothing was changed.", addr::kExpectedChecksum);
        return 0;
    }
    InstallHooks();
    LogBytes("GetRTPCValue(char*)", g_base + addr::kGetRtpcChar);
    g_getRtpcOk = LooksLikeFunctionStart(g_base + addr::kGetRtpcChar);
    Log("Keys: F5 slower | F6 speed 100%% | F7 log Time_Stretch | F8 mark | F9 stretch effect ON | "
        "F10 effect OFF | F11 unload probe | F12 disable hooks");

    const float speeds[] = {100, 75, 50, 25, 10, 5, 1};
    int speedIdx = 0, mark = 0;
    bool k5 = false, k6 = false, k7 = false, k8 = false, k9 = false, k10 = false, k11 = false, k12 = false;

    char lastMenu[96] = "", lastKey[96] = "";
    float lastT = -1, rateT = -1;
    DWORD rateTick = GetTickCount();
    bool wasMoving = false;

    for (;;) {
        Sleep(50);

        // Menu / screen changes
        char menu[96];
        if (GetMenu(menu, sizeof(menu)) && strcmp(menu, lastMenu) != 0) {
            Log("MENU   %s", menu);
            strcpy_s(lastMenu, menu);
        }

        char key[96];
        if (GetSongKey(key, sizeof(key)) && strcmp(key, lastKey) != 0) {
            Log("SONG   key=%s", key);
            strcpy_s(lastKey, key);
        }

        // Song timer: log once per second while it's running, with its effective speed
        float t;
        if (GetSongTime(&t)) {
            DWORD now = GetTickCount();
            if (rateT < 0) { rateT = t; rateTick = now; }
            if (now - rateTick >= 1000) {
                float rate = (t - rateT) / ((now - rateTick) / 1000.0f);
                bool moving = std::fabs(rate) > 0.001f;
                if (moving || wasMoving)
                    Log("TIMER  t=%8.3f s   speed=%5.2fx%s", t, rate, moving ? "" : "   (STOPPED)");
                wasMoving = moving;
                rateT = t;
                rateTick = now;
            }
            lastT = t;
        }

        if (!GameFocused()) continue;
        if (Pressed(VK_F5, k5)) { if (speedIdx < 6) ++speedIdx; SetSpeed(speeds[speedIdx]); }
        if (Pressed(VK_F6, k6)) { speedIdx = 0; SetSpeed(100); }
        if (Pressed(VK_F7, k7)) LogStretch();
        if (Pressed(VK_F8, k8)) Log("==================== MARK %d (t=%.3f) ====================", ++mark, lastT);
        if (Pressed(VK_F9, k9)) SetTimeStretchEffect(true);
        if (Pressed(VK_F10, k10)) SetTimeStretchEffect(false);
        if (Pressed(VK_F12, k12) && g_hooksOn) {
            MH_DisableHook(MH_ALL_HOOKS);
            g_hooksOn = false;
            Log("Hooks disabled (F12).");
        }
        if (Pressed(VK_F11, k11)) break;
    }

    // F11: unload, so a rebuilt probe can be injected without restarting the game.
    // Remove the hooks first, then wait a moment so no game thread is still inside a detour.
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Sleep(300);
    Log("Probe unloaded (F11).");
    { std::lock_guard<std::mutex> lock(g_logMutex); fclose(g_log); g_log = nullptr; }
    FreeLibraryAndExitThread(g_self, 0);
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        // Don't do real work inside DllMain (the Windows loader lock is held). Start a thread instead.
        CloseHandle(CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
