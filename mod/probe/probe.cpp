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
#include <timeapi.h>
#include <tlhelp32.h>
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

// Diagnostic for test 2 (MH_ERROR_MEMORY_PROTECT). Hooking needs the code page to be writable for
// a moment (VirtualProtect). RSMods' notes say the game is protected by VMProtect, which can intercept
// ntdll!NtProtectVirtualMemory (the kernel call behind VirtualProtect) and refuse changes to the
// game's code. Here we check: what kind of memory the target is, whether a plain VirtualProtect
// fails and with which error, and whether ntdll's NtProtectVirtualMemory still starts with its normal
// "mov eax, <syscall number>" (B8 xx xx xx xx) or was patched.
static void DiagnoseProtect(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)a, &mbi, sizeof(mbi));
    Log("diag: page 0x%08X region=0x%08X size=0x%X state=0x%X protect=0x%X type=0x%X (0x1000000=IMAGE 0x40000=MAPPED 0x20000=PRIVATE)",
        (unsigned)a, (unsigned)(uintptr_t)mbi.BaseAddress, (unsigned)mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type);
    DWORD old = 0;
    BOOL ok = VirtualProtect((void*)a, 16, PAGE_EXECUTE_READWRITE, &old);
    DWORD err = ok ? 0 : GetLastError();
    Log("diag: VirtualProtect(RWX) -> %s, error=%lu, old=0x%X", ok ? "OK" : "FAILED", err, old);
    if (ok) VirtualProtect((void*)a, 16, old, &old);  // put it back
    LogBytes("ntdll!NtProtectVirtualMemory", (uintptr_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtProtectVirtualMemory"));
}

static bool InstallHooks() {
    DiagnoseProtect(g_base + addr::kSetRtpcChar);
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

static char g_lastSongKey[96] = "";  // last key seen while browsing songs (the pointer is empty in-game)

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
    if (!GetSongKey(key, sizeof(key))) strcpy_s(key, g_lastSongKey);
    if (!key[0]) { Log(">>> effect: no song key yet (select a song in the song list first)"); return; }
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

// ------------------------------------------------------------------------------------ pause / resume music
// Test 3 showed that the song timer (and so the highway) follows the MUSIC, and that time-stretch
// bottoms out around 25% speed, which is not a freeze. So: pause the music itself.
// ExecuteActionOnEvent applies an action (Stop/Pause/Resume...) to everything that a previously
// posted event started. "Play_<songkey>" is the event that started the song's music.
//
// Test 4: ExecuteActionOnEvent("Play_<key>", Pause, <all objects>) returned AK_Fail (2). This older
// Wwise apparently needs the exact game object and/or playing ID. So we FIND the song's playback:
//   - Every PostEvent returns a "playing ID", and Wwise hands those out counting up from 1.
//   - GetEventIDFromPlayingID(pid) tells which event started a playback (0 = not playing).
//   - An event's ID is the 32-bit FNV-1 hash of its lowercase name (that's how Wwise turns names
//     into IDs), so we compute the ID of "play_<key>" and scan playing IDs until one matches.
//   - GetGameObjectFromPlayingID(pid) then gives the game object it plays on.
// Then several ways to pause are tried in order and logged, and the first that succeeds is reused.
namespace addr {
constexpr uintptr_t kExecuteActionOnEventChar = 0x00AC49E0;
constexpr uintptr_t kExecuteActionOnEventId = 0x00AC4930;
constexpr uintptr_t kGetEventIDFromPlayingID = 0x00ABFE80;
constexpr uintptr_t kGetGameObjectFromPlayingID = 0x00ABFEA0;
constexpr uintptr_t kGetPlayingIDsFromGameObject = 0x00ABFEC0;
}  // namespace addr
using ExecuteActionOnEventChar_t = AKRESULT(__cdecl*)(const char*, int /*AkActionOnEventType*/, AkGameObjectID,
                                                       AkTimeMs, int /*curve*/, AkPlayingID);
using ExecuteActionOnEventId_t = AKRESULT(__cdecl*)(AkUniqueID, int, AkGameObjectID, AkTimeMs, int, AkPlayingID);
using GetEventIDFromPlayingID_t = AkUniqueID(__cdecl*)(AkPlayingID);
using GetGameObjectFromPlayingID_t = AkGameObjectID(__cdecl*)(AkPlayingID);
using GetPlayingIDsFromGameObject_t = AKRESULT(__cdecl*)(AkGameObjectID, AkUInt32*, AkPlayingID*);
constexpr int kActionPause = 1, kActionResume = 2;

static bool FnOk(uintptr_t rva, const char* name, bool verbose);  // defined below (safety check by name)

static AkUniqueID WwiseHash(const std::string& name) {  // FNV-1, 32 bit, lowercase (Wwise's GetIDFromString)
    uint32_t h = 2166136261u;
    for (char c : name) { h *= 16777619u; h ^= (uint8_t)tolower((unsigned char)c); }
    return h;
}

static AkPlayingID g_songPid = 0;          // playing ID of the song's music (0 = not found yet)
static AkGameObjectID g_songObj = 0;
static std::string g_songPidKey;           // song the cached pid belongs to

static bool FindSongPlayback(const std::string& ev, AkUniqueID evId) {
    auto evOf = (GetEventIDFromPlayingID_t)(g_base + addr::kGetEventIDFromPlayingID);
    auto objOf = (GetGameObjectFromPlayingID_t)(g_base + addr::kGetGameObjectFromPlayingID);
    auto idsOf = (GetPlayingIDsFromGameObject_t)(g_base + addr::kGetPlayingIDsFromGameObject);

    if (g_songPid && g_songPidKey == ev && evOf(g_songPid) == evId) return true;  // cached and still playing

    // 1) Cheap check first: what's playing on the object RSMods calls the song object (0x1234)?
    AkPlayingID ids[64];
    AkUInt32 n = 64;
    AKRESULT r = idsOf(kSongGameObject, &n, ids);
    Log(">>> find: GetPlayingIDsFromGameObject(0x1234) -> result=%d n=%u", r, n);
    for (AkUInt32 i = 0; i < n && i < 64; ++i) {
        AkUniqueID e = evOf(ids[i]);
        Log(">>> find:   pid=%u event=0x%08X%s", ids[i], e, e == evId ? "  <== the song" : "");
        if (e == evId) { g_songPid = ids[i]; g_songObj = kSongGameObject; g_songPidKey = ev; return true; }
    }

    // 2) Scan playing IDs, newest first. The counter only goes up, so the song is likely a recent
    //    ID, but everything since the game started (UI sounds etc.) has used IDs too.
    DWORD t0 = GetTickCount();
    for (AkPlayingID pid = 3000000; pid >= 1; --pid) {
        if (evOf(pid) == evId) {
            g_songPid = pid;
            g_songObj = objOf(pid);
            g_songPidKey = ev;
            Log(">>> find: scan found pid=%u on game object 0x%X (%lu ms)", pid, (unsigned)g_songObj, GetTickCount() - t0);
            return true;
        }
    }
    Log(">>> find: scan found nothing up to pid 3000000 (%lu ms)", GetTickCount() - t0);
    return false;
}

static void PauseMusic(bool pause) {
    if (!FnOk(addr::kExecuteActionOnEventChar, "ExecuteActionOnEvent(char*)", false)) return;
    char key[96];
    if (!GetSongKey(key, sizeof(key))) strcpy_s(key, g_lastSongKey);
    if (!key[0]) { Log(">>> pause: no song key yet (select a song in the song list first)"); return; }
    std::string ev = std::string("Play_") + key;
    AkUniqueID evId = WwiseHash(ev);
    int action = pause ? kActionPause : kActionResume;
    const char* what = pause ? "PAUSE" : "RESUME";
    float t = -1;
    GetSongTime(&t);
    Log(">>> %s %s (event id 0x%08X) at song t=%.3f", what, ev.c_str(), evId, t);

    auto execChar = (ExecuteActionOnEventChar_t)(g_base + addr::kExecuteActionOnEventChar);

    // Test 6: every call through this address returned AK_Fail (2), even with the right object and
    // playing ID. Its first bytes (push ebp; mov ebp,esp; sub esp,38h; mov ecx,[ebp+8]; ...) match the
    // BY-ID variant of SetRTPCValue (it builds a 0x38-byte queued message), and it doesn't call the
    // name->ID hash first like the by-NAME wrappers do (e.g. SetRTPCValue(char*) at 0xAC2400).
    // Hypothesis: RSMods' "char*" address is really ExecuteActionOnEvent(AkUniqueID). So pass the
    // numeric event ID (the FNV hash) through the same pointer:
    auto execIdHyp = (ExecuteActionOnEventId_t)(g_base + addr::kExecuteActionOnEventChar);
    if (FnOk(addr::kGetPlayingIDsFromGameObject, "GetPlayingIDsFromGameObject", false) &&
        FnOk(addr::kGetEventIDFromPlayingID, "GetEventIDFromPlayingID", false) &&
        FnOk(addr::kGetGameObjectFromPlayingID, "GetGameObjectFromPlayingID", false) &&
        FindSongPlayback(ev, evId)) {
        AKRESULT d = execIdHyp(evId, action, g_songObj, 0, 4, g_songPid);
        Log(">>>   D: id-hypothesis, obj 0x%X, pid %u -> %d", (unsigned)g_songObj, g_songPid, d);
        if (d == 1) return;
        d = execIdHyp(evId, action, g_songObj, 0, 4, 0);
        Log(">>>   E: id-hypothesis, obj 0x%X, any pid -> %d", (unsigned)g_songObj, d);
        if (d == 1) return;
    }
    AKRESULT e = execIdHyp(evId, action, AK_INVALID_GAME_OBJECT, 0, 4, 0);
    Log(">>>   F: id-hypothesis, all objects        -> %d", e);
    if (e == 1) return;

    // Strategy A: by name, on the song object 0x1234
    AKRESULT r = execChar(ev.c_str(), action, kSongGameObject, 0, 4, 0);
    Log(">>>   A: by name, obj 0x1234                -> %d", r);
    if (r == 1) return;

    // Strategy B/C: find the exact playback, then act on it. RSMods' address for the by-ID variant is
    // wrong for our build (test 5: mid-function bytes), so B and C use the by-NAME variant, which
    // takes the same game object and playing ID parameters.
    if (!FnOk(addr::kGetEventIDFromPlayingID, "GetEventIDFromPlayingID", false) ||
        !FnOk(addr::kGetGameObjectFromPlayingID, "GetGameObjectFromPlayingID", false) ||
        !FnOk(addr::kGetPlayingIDsFromGameObject, "GetPlayingIDsFromGameObject", false)) {
        Log(">>>   B/C skipped: a query function failed the safety check");
        return;
    }
    if (!FindSongPlayback(ev, evId)) return;
    r = execChar(ev.c_str(), action, g_songObj, 0, 4, g_songPid);
    Log(">>>   B: by name, obj 0x%X, pid %u        -> %d", (unsigned)g_songObj, g_songPid, r);
    if (r == 1) return;
    r = execChar(ev.c_str(), action, g_songObj, 0, 4, 0);
    Log(">>>   C: by name, obj 0x%X, any pid       -> %d", (unsigned)g_songObj, r);
}

// Checks every Wwise function we call, BY NAME. Test 5 showed that one address from RSMods is not a
// function start in our build, but the old log didn't say which one.
static bool FnOk(uintptr_t rva, const char* name, bool verbose) {
    bool ok = LooksLikeFunctionStart(g_base + rva);
    if (verbose || !ok) {
        LogBytes(name, g_base + rva);
        Log("check  %-28s rva=0x%06X : %s", name, (unsigned)rva, ok ? "OK" : "NOT A FUNCTION START");
    }
    return ok;
}

static void CheckWwiseFunctions() {
    FnOk(addr::kSetRtpcChar, "SetRTPCValue(char*)", true);
    FnOk(addr::kGetRtpcChar, "GetRTPCValue(char*)", true);
    FnOk(addr::kQueryAudioObjectIDsChar, "QueryAudioObjectIDs(char*)", true);
    FnOk(addr::kSetActorMixerEffect, "SetActorMixerEffect", true);
    FnOk(addr::kExecuteActionOnEventChar, "ExecuteActionOnEvent(char*)", true);
    FnOk(addr::kExecuteActionOnEventId, "ExecuteActionOnEvent(id)", true);
    FnOk(addr::kGetEventIDFromPlayingID, "GetEventIDFromPlayingID", true);
    FnOk(addr::kGetGameObjectFromPlayingID, "GetGameObjectFromPlayingID", true);
    FnOk(addr::kGetPlayingIDsFromGameObject, "GetPlayingIDsFromGameObject", true);
}

static std::wstring g_cmdPathDir;  // the run\ folder (commands.txt, logs)

// ------------------------------------------------------------------------------------ timer write tests
// Test 7: pausing the song's Wwise event succeeded (AK_Success) but changed nothing you could see or hear,
// and the timer kept running. New hypothesis: the song timer is the GAME's clock (it scales with
// Time_Stretch because the game applies the speed to its own clock). If the highway is drawn from
// this value, then writing it should move or freeze the highway:
//   hold <sec>   keep rewriting the timer to its current value for <sec> seconds (every ~1 ms)
//   jump <delta> add <delta> seconds to the timer once
// The timer is a normal heap variable (data, not code), so writing it needs no VirtualProtect.
static bool WriteFloat(uintptr_t a, float v) {
    __try { *(volatile float*)a = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool TimerAddress(uintptr_t* a) { return ReadChain(g_base + addr::kRoot, addr::kTimerChain, 3, a); }

static void HoldTimer(float seconds) {
    uintptr_t a;
    float t;
    if (!TimerAddress(&a) || !ReadFloat(a, &t)) { Log(">>> hold: timer not readable"); return; }
    Log(">>> hold: forcing timer to %.3f for %.1f s (address 0x%08X)", t, seconds, (unsigned)a);
    timeBeginPeriod(1);  // 1 ms Sleep resolution
    DWORD end = GetTickCount() + (DWORD)(seconds * 1000);
    unsigned writes = 0, drift = 0;
    while (GetTickCount() < end) {
        float now;
        if (ReadFloat(a, &now) && now != t) ++drift;  // the game moved it since our last write
        WriteFloat(a, t);
        ++writes;
        Sleep(1);
    }
    timeEndPeriod(1);
    float after = -1;
    ReadFloat(a, &after);
    Log(">>> hold: done. %u writes, the game changed the value %u times in between; timer now %.3f", writes, drift, after);
}

static void JumpTimer(float delta) {
    uintptr_t a;
    float t;
    if (!TimerAddress(&a) || !ReadFloat(a, &t)) { Log(">>> jump: timer not readable"); return; }
    WriteFloat(a, t + delta);
    Sleep(200);
    float after = -1;
    ReadFloat(a, &after);
    Log(">>> jump: %.3f -> %.3f (asked %+.1f s); 200 ms later the timer reads %.3f", t, t + delta, delta, after);
}

// ------------------------------------------------------------------------------------ "who writes this address?"
// Test 8: the timer we read is a COPY. The game rewrites it every ~6 ms, and our writes were undone
// within one tick. To find the real clock, we need the instruction that writes the copy. That's
// Cheat Engine's "find out what writes to this address", done here with the CPU's debug registers:
//   - DR0 = the address, DR7 = "break on WRITE of 4 bytes at DR0" (a hardware data breakpoint)
//   - it must be set on every thread of the game (debug registers are per thread)
//   - when a thread writes the address, the CPU raises EXCEPTION_SINGLE_STEP right AFTER the
//     writing instruction. Our vectored exception handler records EIP (and the registers) and lets the
//     game continue. Nothing in the game's code is modified.
// After <sec> seconds the breakpoints are removed. The log lists each writer with its RVA; we then
// disassemble around it in the memory dump.
struct WriterHit { uintptr_t eip; LONG count; CONTEXT ctx; };
static WriterHit g_hits[16];
static volatile LONG g_hitCount = 0;
static volatile LONG g_hitLock = 0;
static uintptr_t g_watchAddr = 0;

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t eip = ep->ContextRecord->Eip;
    while (InterlockedExchange(&g_hitLock, 1)) {}  // tiny spin lock (the handler runs on game threads)
    int i = 0;
    for (; i < g_hitCount; ++i) if (g_hits[i].eip == eip) break;
    if (i == g_hitCount && i < 16) { g_hits[i].eip = eip; g_hits[i].count = 0; g_hits[i].ctx = *ep->ContextRecord; ++g_hitCount; }
    if (i < 16) ++g_hits[i].count;
    InterlockedExchange(&g_hitLock, 0);
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (on=true) or clears DR0/DR7 on all game threads except ours.
static int SetWatchOnAllThreads(bool on, uintptr_t address, DWORD rwBits) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            if (on) {
                c.Dr0 = address;
                // DR7: bit0 = enable DR0 locally; bits16-17 = R/W (01 = write, 11 = read or write);
                // bits18-19 = LEN (11 = 4 bytes)
                c.Dr7 = (c.Dr7 & ~0xF0003u) | 1u | (rwBits << 16) | (3u << 18);
            } else {
                c.Dr0 = 0;
                c.Dr7 &= ~0xF0003u;
            }
            if (SetThreadContext(th, &c)) ++n;
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    return n;
}

static void WhoAccesses(float seconds, bool includeReads, uintptr_t a = 0) {
    if (!a && !TimerAddress(&a)) { Log(">>> who: timer not readable"); return; }
    g_watchAddr = a;
    g_hitCount = 0;
    PVOID veh = AddVectoredExceptionHandler(1, WatchHandler);
    int n = SetWatchOnAllThreads(true, a, includeReads ? 3u : 1u);
    Log(">>> who%s: watching 0x%08X on %d threads for %.1f s", includeReads ? "reads" : "writes", (unsigned)a, n, seconds);
    Sleep((DWORD)(seconds * 1000));
    SetWatchOnAllThreads(false, 0, 0);
    Sleep(50);
    RemoveVectoredExceptionHandler(veh);
    for (int i = 0; i < g_hitCount; ++i) {
        const WriterHit& h = g_hits[i];
        Log(">>> who: hit after EIP=0x%08X (rva 0x%06X) x%ld  EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X",
            (unsigned)h.eip, (unsigned)(h.eip - g_base), h.count, h.ctx.Eax, h.ctx.Ebx, h.ctx.Ecx, h.ctx.Edx,
            h.ctx.Esi, h.ctx.Edi, h.ctx.Ebp, h.ctx.Esp);
        LogBytes("  code before EIP", h.eip - 16);
    }
    if (g_hitCount == 0) Log(">>> who: no hits (is a song playing?)");
}

// ------------------------------------------------------------------------------------ the song object
// Test 9 (disassembly of rva 0x3DDA80, the function that writes our timer copy):
//   EDI = song object = [[base+0xF6062C]+0xB0]   (the "0xB0" step of the timer chain)
//   [EDI+0x3B4] = float -> copied into our timer        (candidate: the real song clock)
//   [EDI+0x3B0] = float from a call on a global object  (candidate: the audio/music clock)
//   [EDI+0x538] = pointer to the {flag, 0, time, remaining} record we were reading
static uintptr_t SongObject() {
    const uint32_t chain[] = {0xB0};
    uintptr_t a;
    if (!ReadChain(g_base + addr::kRoot, chain, 1, &a)) return 0;  // a = [root]+0xB0
    uintptr_t obj;
    __try { obj = *(uintptr_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return obj;
}

// "3b4" -> song object + 0x3B4. An argument starting with "0x" and 8+ hex digits is an absolute address.
static uintptr_t SongObjAddr(const char* arg) {
    uintptr_t v = (uintptr_t)strtoul(arg, nullptr, 16);
    if (strlen(arg) >= 8) return v;
    uintptr_t obj = SongObject();
    return obj ? obj + v : 0;
}

static void LogSongObject() {
    uintptr_t obj = SongObject();
    if (!obj) { Log(">>> songobj: not found"); return; }
    for (int i = 0; i < 5; ++i) {
        float a = -1, b = -1, t = -1;
        ReadFloat(obj + 0x3B0, &a);
        ReadFloat(obj + 0x3B4, &b);
        GetSongTime(&t);
        Log(">>> songobj 0x%08X: [+3B0]=%10.4f  [+3B4]=%10.4f  timer=%10.4f", (unsigned)obj, a, b, t);
        Sleep(200);
    }
}

// ------------------------------------------------------------------------------------ the clock provider
// Tests 10-11 (disassembly of rva 0x4BA80 / 0x4C550): the song clock [song+0x3B4] is copied from a
// "clock provider" object, whose time in state 1 is
//     GetSourcePlayPosition(playingID = [provider+0xCC], &ms, extrapolate=true) / 1000
// So the clock IS the position of one Wwise playback, whose playing ID is at provider+0xCC.
// Pausing THAT playback should freeze the clock and the highway.
//   pause2 <provider hex>   pause the playback at [provider+0xCC]
//   resume2 <provider hex>  resume it
static void PauseProviderPlayback(const char* arg, bool pause) {
    uintptr_t prov = (uintptr_t)strtoul(arg, nullptr, 16);
    AkPlayingID pid = 0;
    __try { pid = *(AkPlayingID*)(prov + 0xCC); } __except (EXCEPTION_EXECUTE_HANDLER) { Log(">>> pause2: provider unreadable"); return; }
    auto evOf = (GetEventIDFromPlayingID_t)(g_base + addr::kGetEventIDFromPlayingID);
    auto objOf = (GetGameObjectFromPlayingID_t)(g_base + addr::kGetGameObjectFromPlayingID);
    AkUniqueID ev = evOf(pid);
    AkGameObjectID obj = objOf(pid);
    float t = -1;
    GetSongTime(&t);
    Log(">>> %s2: provider 0x%08X playingID=%u event=0x%08X obj=0x%X  song t=%.3f",
        pause ? "pause" : "resume", (unsigned)prov, pid, ev, (unsigned)obj, t);
    if (!ev) { Log(">>>   that playing ID is not active"); return; }
    auto exec = (ExecuteActionOnEventId_t)(g_base + addr::kExecuteActionOnEventChar);  // really the by-ID variant (test 6)
    AKRESULT r = exec(ev, pause ? kActionPause : kActionResume, obj, 0, 4, pid);
    Log(">>>   ExecuteActionOnEvent(0x%08X, %s, obj 0x%X, pid %u) -> %d", ev, pause ? "Pause" : "Resume", (unsigned)obj, pid, r);
}

// ------------------------------------------------------------------------------------ the game's own music pause
// Test 12: pausing the clock's playback with "ExecuteActionOnEvent" (whatever rva 0xAC49E0 really is)
// returned success but paused nothing. So we looked for what the GAME does. The memory dump contains
// the event names "Pause_TMusic" / "Resume_TMusic", and the code that uses them (rva 0x3CF400, 0x4BF40)
// does this:
//     mgr = AudioManagerGetter()                 // rva 0x35F10, a service lookup with no arguments
//     PostEvent("Pause_TMusic", *(mgr + 0x38), 0, 0, 0, 0, 0, 0)   // rva 0xAC4870 = real PostEvent(char*)
// We do exactly the same. (The game's full pause also sets UI/"paused" flags; we only want the music.)
namespace addr {
constexpr uintptr_t kAudioManagerGetter = 0x035F10;
}
using AudioManagerGetter_t = void*(__cdecl*)();

static void TMusic(bool pause) {
    if (!FnOk(addr::kAudioManagerGetter, "AudioManagerGetter", false) || !FnOk(addr::kPostEventChar, "PostEvent(char*)", false)) return;
    void* mgr = ((AudioManagerGetter_t)(g_base + addr::kAudioManagerGetter))();
    if (!mgr) { Log(">>> tmusic: audio manager not found"); return; }
    AkGameObjectID obj = *(AkGameObjectID*)((uintptr_t)mgr + 0x38);
    const char* ev = pause ? "Pause_TMusic" : "Resume_TMusic";
    float t = -1;
    GetSongTime(&t);
    auto post = (PostEventChar_t)(g_base + addr::kPostEventChar);
    AkPlayingID r = post(ev, obj, 0, nullptr, nullptr, 0, nullptr, 0);
    Log(">>> tmusic: PostEvent(\"%s\", obj 0x%X) -> playingID %u (0 = failed)  mgr=0x%08X  song t=%.3f",
        ev, (unsigned)obj, r, (unsigned)(uintptr_t)mgr, t);
}

// pokeb <hexaddr> <value>: write one byte (data only; used for provider flags such as +0xDA).
// Test 14: the game's pause posts "Stop_TMusic" and sets provider+0xDA = 1. The provider's time
// function (rva 0x4C550) returns early when +0xDA != 0, so the song clock stops advancing.
static void PokeByte(const char* args) {
    char* end;
    uintptr_t a = (uintptr_t)strtoul(args, &end, 16);
    int v = atoi(end);
    uint8_t before = 0, after = 0;
    bool ok;
    __try { before = *(volatile uint8_t*)a; *(volatile uint8_t*)a = (uint8_t)v; after = *(volatile uint8_t*)a; ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    float t = -1;
    GetSongTime(&t);
    if (ok) Log(">>> pokeb 0x%08X: %u -> %u (reads %u)  song t=%.3f", (unsigned)a, before, v, after, t);
    else Log(">>> pokeb 0x%08X: not writable", (unsigned)a);
}

// ------------------------------------------------------------------------------------ FREEZE = clock flag + mute + seek
// Test 15: provider+0xDA = 1 freezes the song clock (highway, scoring...), but the music keeps
// playing, and on release the clock jumps ahead to the music's position.
// And RSMods lists rva 0xAC49E0 ALSO as SeekOnEvent(AkUniqueID, obj, AkTimeMs, bool). That's what it
// really is (its "ExecuteActionOnEvent" labels for our build are wrong), which explains tests 6-12.
// So the plan:
//   freeze:   remember T = song time; mute the song's game object; set provider+0xDA = 1
//   unfreeze: seek "Play_<key>" to T; unmute; set provider+0xDA = 0
namespace addr {
constexpr uintptr_t kSeekOnEventIdInt = 0x00AC49E0;               // SeekOnEvent(AkUniqueID, obj, AkTimeMs, bool)
constexpr uintptr_t kSetGameObjectOutputBusVolume = 0x00AC0E10;  // (obj, AkReal32 volume 0..1)
constexpr uintptr_t kProviderVtable = 0x00DA0E70;                // rva of the clock provider's vtable (0x01750E70 - base)
}
using SeekOnEventIdInt_t = AKRESULT(__cdecl*)(AkUniqueID, AkGameObjectID, AkTimeMs, bool);
using SetGameObjectOutputBusVolume_t = AKRESULT(__cdecl*)(AkGameObjectID, float);

// The provider is found through the song object: scan the song object's fields for a pointer to an
// object whose vtable is the provider's vtable and whose +0x0C points back to the song object.
static bool IsProvider(uintptr_t p, uintptr_t song) {
    __try { return *(uintptr_t*)p == g_base + addr::kProviderVtable && *(uintptr_t*)(p + 0x0C) == song; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Fallback (test 16: the song object has no direct pointer to the provider): scan all committed,
// writable, private memory for the provider's vtable pointer followed by the back-pointer at +0x0C.
static uintptr_t ScanForProvider(uintptr_t song) {
    const uintptr_t vt = g_base + addr::kProviderVtable;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
            (mbi.Protect & PAGE_GUARD))
            continue;
        const uintptr_t* p = (const uintptr_t*)mbi.BaseAddress;
        size_t n = mbi.RegionSize / 4;
        __try {
            for (size_t i = 0; i + 3 < n; ++i)
                if (p[i] == vt && p[i + 3] == song) return (uintptr_t)&p[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

static uintptr_t g_provider = 0;

static uintptr_t FindProvider() {
    uintptr_t song = SongObject();
    if (!song) return 0;
    if (g_provider && IsProvider(g_provider, song)) return g_provider;  // cached and still valid
    DWORD t0 = GetTickCount();
    g_provider = ScanForProvider(song);
    Log(">>> provider scan: 0x%08X (%lu ms)", (unsigned)g_provider, GetTickCount() - t0);
    return g_provider;
    for (uintptr_t off = 0; off < 0x1000; off += 4) {
        uintptr_t p = 0, vt = 0, back = 0;
        __try {
            p = *(uintptr_t*)(song + off);
            if (p < 0x10000) continue;
            vt = *(uintptr_t*)p;
            back = *(uintptr_t*)(p + 0x0C);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (vt == g_base + addr::kProviderVtable && back == song) return p;
    }
    return 0;
}

static bool SongEvent(std::string* ev, AkUniqueID* id) {
    char key[96];
    if (!GetSongKey(key, sizeof(key))) strcpy_s(key, g_lastSongKey);
    if (!key[0]) { Log(">>> no song key yet (select a song in the song list first)"); return false; }
    *ev = std::string("Play_") + key;
    *id = WwiseHash(*ev);
    return true;
}

static uintptr_t FindProvider();

static AKRESULT SeekSong(AkTimeMs ms) {
    std::string ev = "(from provider)"; AkUniqueID id = 0;
    // Preferred: the event of the playback the clock follows (provider+0xCC = its playing ID). This
    // doesn't need the song key, which a freshly injected probe doesn't know (test 17).
    uintptr_t prov = FindProvider();
    if (prov && FnOk(addr::kGetEventIDFromPlayingID, "GetEventIDFromPlayingID", false))
        id = ((GetEventIDFromPlayingID_t)(g_base + addr::kGetEventIDFromPlayingID))(*(AkPlayingID*)(prov + 0xCC));
    if (!id && !SongEvent(&ev, &id)) return -1;
    if (!FnOk(addr::kSeekOnEventIdInt, "SeekOnEvent(id,int)", false)) return -1;
    AKRESULT r = ((SeekOnEventIdInt_t)(g_base + addr::kSeekOnEventIdInt))(id, kSongGameObject, ms, false);
    Log(">>> seek %s (0x%08X) on obj 0x1234 to %d ms -> %d", ev.c_str(), id, ms, r);
    return r;
}

static AKRESULT SongVolume(float v) {
    if (!FnOk(addr::kSetGameObjectOutputBusVolume, "SetGameObjectOutputBusVolume", false)) return -1;
    AKRESULT r = ((SetGameObjectOutputBusVolume_t)(g_base + addr::kSetGameObjectOutputBusVolume))(kSongGameObject, v);
    Log(">>> SetGameObjectOutputBusVolume(obj 0x1234, %.2f) -> %d", v, r);
    return r;
}

// Test 18: the mute did nothing audible (SetGameObjectOutputBusVolume is probably mislabeled too).
// Disassembly around PostEvent/SeekOnEvent (each function queues a Wwise message; the type number
// identifies it) showed the REAL ExecuteActionOnEvent(AkUniqueID, action, obj, ms, curve, playingID)
// at rva 0xAC4900 (message type 0x20; the char*/wchar_t* wrappers at 0xAC4980/0xAC49B0 hash the
// name and call it). So we pause the exact playback the clock follows.
namespace addr {
constexpr uintptr_t kExecuteActionOnEventIdReal = 0x00AC4900;
}

static AKRESULT SongAction(int action) {
    uintptr_t prov = FindProvider();
    if (!prov || !FnOk(addr::kExecuteActionOnEventIdReal, "ExecuteActionOnEvent(id) real", false) ||
        !FnOk(addr::kGetEventIDFromPlayingID, "GetEventIDFromPlayingID", false)) return -1;
    AkPlayingID pid = *(AkPlayingID*)(prov + 0xCC);
    AkUniqueID ev = ((GetEventIDFromPlayingID_t)(g_base + addr::kGetEventIDFromPlayingID))(pid);
    AKRESULT r = ((ExecuteActionOnEventId_t)(g_base + addr::kExecuteActionOnEventIdReal))(ev, action, kSongGameObject, 0, 4, pid);
    Log(">>> ExecuteActionOnEvent(0x%08X, %s, obj 0x1234, pid %u) -> %d", ev, action == kActionPause ? "Pause" : "Resume", pid, r);
    return r;
}

static float g_freezeT = -1;

static void Freeze(bool on) {
    uintptr_t prov = FindProvider();
    if (!prov) { Log(">>> freeze: provider not found (is a song playing?)"); return; }
    if (on) {
        GetSongTime(&g_freezeT);
        SongAction(kActionPause);
        *(volatile uint8_t*)(prov + 0xDA) = 1;
        Log(">>> FREEZE at t=%.3f (provider 0x%08X)", g_freezeT, (unsigned)prov);
    } else {
        // Test 19: with the seek here, music and highway ended up out of sync. With a real pause
        // there's no drift to correct, and SeekOnEvent (music segment time) may not match the
        // source position the clock reads. So unfreeze = resume only.
        SongAction(kActionResume);
        *(volatile uint8_t*)(prov + 0xDA) = 0;
        Log(">>> UNFREEZE (back to t=%.3f)", g_freezeT);
        g_freezeT = -1;
    }
}

// ------------------------------------------------------------------------------------ find note arrays (difficulty levels)
// Test 21: the mod waited on a note that the highway wasn't showing, because Dynamic Difficulty shows
// only the notes of the current level of each phrase, and our charts had the max level. Each SNG level
// holds its own note array. For notegel1, level 18 starts 18.0, 19.0, 20.0; levels 1-6 start 18.0,
// 19.0, 22.0; level 0 starts 18.0, 22.0, 26.0. Notes are fixed-size records with a float time, so
// look for 3 floats t1, t2, t3 at a constant stride S (the record size) in the heap.
//   findnotes <t1> <t2> <t3>
static void FindNoteArrays(const char* args) {
    float t1 = 0, t2 = 0, t3 = 0;
    if (sscanf_s(args, "%f %f %f", &t1, &t2, &t3) != 3) { Log(">>> findnotes: need 3 times"); return; }
    uint32_t v1, v2, v3;
    memcpy(&v1, &t1, 4); memcpy(&v2, &t2, 4); memcpy(&v3, &t3, 4);
    int found = 0;
    DWORD t0 = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY)))
            continue;
        const uint8_t* base = (const uint8_t*)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        // Byte-aligned search and any stride 1..512: in the SNG file layout a note record is 67 bytes
        // (not a multiple of 4), so the times of an in-memory copy of the raw SNG are unaligned.
        // (The first 4-byte-aligned version only found beat grids and a lookup table.)
        __try {
            for (size_t i = 0; i + 4 <= size; ++i) {
                if (*(const uint32_t UNALIGNED*)(base + i) != v1) continue;
                for (size_t s = 8; s <= 512; ++s) {
                    if (i + 2 * s + 4 > size) break;
                    if (*(const uint32_t UNALIGNED*)(base + i + s) == v2 && *(const uint32_t UNALIGNED*)(base + i + 2 * s) == v3) {
                        if (found < 60)
                            Log(">>> notes %.2f,%.2f,%.2f at 0x%08X stride %u (region 0x%08X type 0x%X)", t1, t2, t3,
                                (unsigned)(uintptr_t)(base + i), (unsigned)s, (unsigned)(uintptr_t)base, mbi.Type);
                        ++found;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    Log(">>> findnotes %.2f %.2f %.2f: %d matches (%lu ms)", t1, t2, t3, found, GetTickCount() - t0);
}

// findptr <lo> <hi>: every aligned dword in memory whose value is in [lo, hi] (pointers into an object).
// Used to walk up from the note lists to the objects that own them (test 22).
static void FindPointers(const char* args) {
    char* end;
    uintptr_t lo = (uintptr_t)strtoul(args, &end, 16), hi = (uintptr_t)strtoul(end, nullptr, 16);
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            continue;
        const uint32_t* p = (const uint32_t*)mbi.BaseAddress;
        const size_t n = mbi.RegionSize / 4;
        __try {
            for (size_t i = 0; i < n; ++i)
                if (p[i] >= lo && p[i] <= hi) {
                    if (found < 80) Log(">>> ptr at 0x%08X -> 0x%08X (+0x%X)", (unsigned)(uintptr_t)&p[i], p[i], (unsigned)(p[i] - lo));
                    ++found;
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    Log(">>> findptr 0x%08X..0x%08X: %d pointers", (unsigned)lo, (unsigned)hi, found);
}

// ------------------------------------------------------------------------------------ which level is being read?
// Test 23: the level table is an array of 100-byte Level objects (same layout as the SNG file):
// Level L's notes vector {begin, end, cap} is at vec0 + L*0x64, and notes are 456-byte records with
// the time at +0x0C. watchnotes <vec0> <t> <L1> <L2> <L3> <L4> puts one hardware breakpoint
// (read/write) on the time of the first note at or after t in each of 4 levels, for 3 s. The level
// being drawn on the highway should be the one that gets read.
static uintptr_t g_multiAddr[4];
static volatile LONG g_multiCount[4];
static uintptr_t g_multiEip[4][4];

static LONG CALLBACK MultiWatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD dr6 = (DWORD)ep->ContextRecord->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i)
        if (dr6 & (1u << i)) {
            LONG n = InterlockedIncrement(&g_multiCount[i]);
            if (n <= 4) g_multiEip[i][n - 1] = ep->ContextRecord->Eip;
        }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool ReadDword(uintptr_t a, uint32_t* v) {
    __try { *v = *(const uint32_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void WatchNotes(const char* args) {
    unsigned vec0 = 0; float t = 0; int lv[4] = {-1, -1, -1, -1};
    if (sscanf_s(args, "%x %f %d %d %d %d", &vec0, &t, &lv[0], &lv[1], &lv[2], &lv[3]) < 3) { Log(">>> watchnotes: bad args"); return; }
    for (int k = 0; k < 4; ++k) {
        g_multiAddr[k] = 0; g_multiCount[k] = 0;
        if (lv[k] < 0) continue;
        uint32_t begin = 0, end = 0;
        if (!ReadDword(vec0 + lv[k] * 0x64, &begin) || !ReadDword(vec0 + lv[k] * 0x64 + 4, &end)) continue;
        const unsigned count = (end - begin) / 456;
        for (unsigned i = 0; i < count; ++i) {
            float nt = -1;
            ReadFloat(begin + i * 456 + 0xC, &nt);
            if (nt >= t) { g_multiAddr[k] = begin + i * 456 + 0xC; Log(">>> level %d: %u notes, watching note #%u (t=%.3f) time at 0x%08X", lv[k], count, i, nt, (unsigned)g_multiAddr[k]); break; }
        }
    }
    PVOID veh = AddVectoredExceptionHandler(1, MultiWatchHandler);
    // Set DR0..DR3 (read/write, 4 bytes) on all threads
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    std::vector<DWORD> tids;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId()) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    auto apply = [&](bool on) {
        for (DWORD tid : tids) {
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
            if (!th) continue;
            SuspendThread(th);
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                DWORD dr7 = 0;
                DWORD* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                for (int k = 0; k < 4; ++k) {
                    *drs[k] = on ? (DWORD)g_multiAddr[k] : 0;
                    if (on && g_multiAddr[k]) dr7 |= (1u << (2 * k)) | (3u << (16 + 4 * k)) | (3u << (18 + 4 * k));  // enable, R/W, 4 bytes
                }
                c.Dr7 = dr7;
                SetThreadContext(th, &c);
            }
            ResumeThread(th);
            CloseHandle(th);
        }
    };
    apply(true);
    Sleep(3000);
    apply(false);
    Sleep(50);
    RemoveVectoredExceptionHandler(veh);
    for (int k = 0; k < 4; ++k)
        if (g_multiAddr[k])
            Log(">>> level %d: %ld accesses in 3 s; code (rva): %06X %06X %06X %06X", lv[k], g_multiCount[k],
                g_multiEip[k][0] ? (unsigned)(g_multiEip[k][0] - g_base) : 0, g_multiEip[k][1] ? (unsigned)(g_multiEip[k][1] - g_base) : 0,
                g_multiEip[k][2] ? (unsigned)(g_multiEip[k][2] - g_base) : 0, g_multiEip[k][3] ? (unsigned)(g_multiEip[k][3] - g_base) : 0);
}

// ddstate: current Dynamic Difficulty level of every phrase iteration (test 24, from the disassembly
// of rva 0x3F20D0: song+0x78 = song data (levels vector +0x40, phrase iterations vector +0x64),
// song+0x7C = DD state (vector +0x18 of 64-byte entries, level at +4)).
static void DdState() {
    uintptr_t song = SongObject();
    uint32_t sng = 0, dd = 0, lvB = 0, lvE = 0, piB = 0, piE = 0, ddB = 0, ddE = 0;
    if (!song || !ReadDword(song + 0x78, &sng) || !ReadDword(song + 0x7C, &dd) || !ReadDword(sng + 0x40, &lvB) ||
        !ReadDword(sng + 0x44, &lvE) || !ReadDword(sng + 0x64, &piB) || !ReadDword(sng + 0x68, &piE) ||
        !ReadDword(dd + 0x18, &ddB) || !ReadDword(dd + 0x1C, &ddE)) { Log(">>> ddstate: not readable"); return; }
    const unsigned nLv = (lvE - lvB) / 0x64, nPi = (piE - piB) / 0x18, nDd = (ddE - ddB) / 64;
    Log(">>> ddstate: song 0x%08X sng 0x%08X dd 0x%08X: %u levels, %u phrase iterations, %u DD entries", (unsigned)song, sng, dd, nLv, nPi, nDd);
    for (unsigned i = 0; i < nPi && i < nDd; ++i) {
        uint32_t phrase = 0, lvl = 0; float start = 0, end = 0;
        ReadDword(piB + i * 0x18, &phrase); ReadFloat(piB + i * 0x18 + 4, &start); ReadFloat(piB + i * 0x18 + 8, &end);
        ReadDword(ddB + i * 64 + 4, &lvl);
        Log(">>>   PI %2u phrase %u %7.3f-%7.3f  current level %d", i, phrase, start, end, (int)lvl);
    }
}

// peek <hexaddr> <count>: log <count> dwords starting at an absolute address, as hex, int and float.
static void Peek(const char* args) {
    char* end;
    uintptr_t a = (uintptr_t)strtoul(args, &end, 16);
    int n = atoi(end);
    if (n <= 0 || n > 256) n = 16;
    for (int i = 0; i < n; ++i) {
        uint32_t v = 0;
        bool ok;
        __try { v = *(uint32_t*)(a + 4 * i); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok) { Log(">>> peek 0x%08X: unreadable", (unsigned)(a + 4 * i)); break; }
        float f;
        memcpy(&f, &v, 4);
        Log(">>> peek 0x%08X (+0x%03X): %08X  int=%-11d float=%g", (unsigned)(a + 4 * i), 4 * i, v, (int)v, f);
    }
}

// ------------------------------------------------------------------------------------ call tracing with hardware breakpoints
// Test 13: posting "Pause_TMusic" like the game's provider does changed nothing. We need to SEE what
// the game calls when the player presses Esc. Code hooks are blocked (VMProtect), but the CPU's debug
// registers can also trigger on EXECUTION of an address (DR7 R/W bits = 00). The CPU raises
// EXCEPTION_SINGLE_STEP BEFORE the instruction runs. Our handler copies the arguments from the
// stack ([esp] = return address, [esp+4] = 1st arg...), sets the Resume Flag (EFLAGS.RF) so the same
// breakpoint doesn't fire again immediately, and lets the game continue. Up to 4 addresses.
struct TraceEntry { DWORD tick; int fn; uintptr_t ret; uint32_t arg[6]; char name[48]; };
static TraceEntry g_trace[32768];
static volatile LONG g_traceN = 0;
static uintptr_t g_traceAddr[4];
static const char* kTraceNames[4] = {"PostEvent(char*)", "PostEvent(id)", "ExecActionOnEvent(id)", "SetRTPCValue(char*)"};

static void CopyName(char* dst, size_t cap, const char* src) {
    __try {
        size_t i = 0;
        for (; i < cap - 1 && src[i] >= 32 && src[i] < 127; ++i) dst[i] = src[i];
        dst[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = 0; }
}

static LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    int fn = -1;
    for (int i = 0; i < 4; ++i) if (g_traceAddr[i] && c->Eip == g_traceAddr[i]) fn = i;
    if (fn < 0) return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&g_traceN) - 1;
    if (n < 32768) {
        TraceEntry& e = g_trace[n];
        e.tick = GetTickCount();
        e.fn = fn;
        __try {
            const uint32_t* sp = (const uint32_t*)c->Esp;
            e.ret = sp[0];
            for (int i = 0; i < 6; ++i) e.arg[i] = sp[1 + i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        e.name[0] = 0;
        if (fn == 0 || fn == 3) CopyName(e.name, sizeof(e.name), (const char*)e.arg[0]);
    }
    c->Dr6 = 0;
    c->EFlags |= 0x10000;  // RF: don't re-trigger on this instruction when we continue
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int SetExecBreakpoints(bool on) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            if (on) {
                c.Dr0 = g_traceAddr[0]; c.Dr1 = g_traceAddr[1]; c.Dr2 = g_traceAddr[2]; c.Dr3 = g_traceAddr[3];
                c.Dr7 = 0x55;  // L0..L3 enabled; R/W=00 (execute) and LEN=00 for all four (bits 16-31 = 0)
            } else {
                c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
                c.Dr7 = 0;
            }
            if (SetThreadContext(th, &c)) ++n;
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    return n;
}

static void TraceAudioCalls(float seconds, bool withRtpc) {
    g_traceAddr[0] = g_base + addr::kPostEventChar;
    g_traceAddr[1] = g_base + addr::kPostEventId;
    g_traceAddr[2] = g_base + addr::kExecuteActionOnEventChar;  // really the by-ID variant (test 6)
    // SetRTPCValue filled the whole buffer in test 13 ("MusicRamping" every frame), so it's optional.
    // Breakpoint 3 at address 0 never fires.
    g_traceAddr[3] = withRtpc ? g_base + addr::kSetRtpcChar : 0;
    g_traceN = 0;
    PVOID veh = AddVectoredExceptionHandler(1, TraceHandler);
    int n = SetExecBreakpoints(true);
    Log(">>> trace: recording audio calls on %d threads for %.0f s. Pause and resume the game now.", n, seconds);
    DWORD t0 = GetTickCount();
    Sleep((DWORD)(seconds * 1000));
    SetExecBreakpoints(false);
    Sleep(50);
    RemoveVectoredExceptionHandler(veh);
    LONG total = g_traceN;
    Log(">>> trace: %ld calls recorded", total);
    // Print in time order. Repeats of the same RTPC name/value are collapsed to keep the log readable.
    std::unordered_map<std::string, int> rtpcCount;
    for (LONG i = 0; i < total && i < 32768; ++i) {
        const TraceEntry& e = g_trace[i];
        float f;
        if (e.fn == 3) {
            memcpy(&f, &e.arg[1], 4);
            char key[80];
            sprintf_s(key, "%s=%.2f", e.name, f);
            if (rtpcCount[key]++ > 0) continue;
            Log("TRACE %7.3fs %-22s \"%s\" value=%.3f obj=0x%X  (caller rva 0x%06X)", (e.tick - t0) / 1000.0,
                kTraceNames[e.fn], e.name, f, e.arg[2], (unsigned)(e.ret - g_base));
        } else if (e.fn == 0) {
            Log("TRACE %7.3fs %-22s \"%s\" obj=0x%X flags=0x%X  (caller rva 0x%06X)", (e.tick - t0) / 1000.0,
                kTraceNames[e.fn], e.name, e.arg[1], e.arg[2], (unsigned)(e.ret - g_base));
        } else {
            Log("TRACE %7.3fs %-22s id=0x%08X a2=%u a3=0x%X a4=%u a5=%u a6=%u  (caller rva 0x%06X)", (e.tick - t0) / 1000.0,
                kTraceNames[e.fn], e.arg[0], e.arg[1], e.arg[2], e.arg[3], e.arg[4], e.arg[5], (unsigned)(e.ret - g_base));
        }
    }
}

// ------------------------------------------------------------------------------------ command file
static std::wstring g_cmdPath;

// Returns the command in commands.txt (first line, trimmed) and deletes the file, or "" if there's none.
static std::string ReadCommand() {
    FILE* f = _wfopen(g_cmdPath.c_str(), L"r");
    if (!f) return "";
    char line[256] = "";
    fgets(line, sizeof(line), f);
    fclose(f);
    DeleteFileW(g_cmdPath.c_str());
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF) s = s.substr(3);  // strip a UTF-8 BOM (PowerShell adds one)
    return s;
}

// ------------------------------------------------------------------------------------ main loop
static bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Test keys are Ctrl + a number key. Plain F-keys clashed: F10 = the Windows menu bar (froze the
// rendering), F12 = the Steam screenshot key, and F11 caused a white flash in the game.
static bool Pressed(int vk, bool& wasDown) {  // true on the key-down edge of Ctrl+vk only
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

static DWORD WINAPI MainThread(LPVOID) {
    // Log file next to the DLL (in our project folder, so the game folder stays untouched).
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring logPath(path);
    // One log per injected copy (NoteByNoteProbe_<tick>.log), so an older probe that is still loaded
    // can't write into the same file.
    logPath = logPath.substr(0, logPath.find_last_of(L'.')) + L".log";
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
    // Hooks are NOT installed (test 2): VMProtect blocks VirtualProtect on the game's code (ACCESS_DENIED),
    // and MinHook's attempt froze the game for ~4 s. Everything below only READS memory or CALLS Wwise
    // functions, and neither needs code patching. InstallHooks() stays in the file for later.
    g_setRtpcOk = LooksLikeFunctionStart(g_base + addr::kSetRtpcChar);
    g_getRtpcOk = LooksLikeFunctionStart(g_base + addr::kGetRtpcChar);
    g_cmdPathDir = logPath.substr(0, logPath.find_last_of(L"\\/") + 1);
    g_cmdPath = g_cmdPathDir + L"commands.txt";
    DeleteFileW(g_cmdPath.c_str());  // ignore leftovers from an earlier run
    Log("Commands: write one line to run\\commands.txt: mark [text] | speed <percent> | fx on | fx off | "
        "pause | resume | check | unload");
    CheckWwiseFunctions();

    int mark = 0;

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
            strcpy_s(g_lastSongKey, key);
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

        // Commands come from a text file instead of keys. Every key we tried clashed with something:
        // F10 = Windows menu bar, F11 = white flash, F12 = Steam screenshot, Ctrl+number = the game's
        // own shortcuts. With a file, Claude (or a script) sends commands while the player just plays.
        std::string cmd = ReadCommand();
        if (cmd.empty()) continue;
        Log("CMD    %s", cmd.c_str());
        if (cmd.rfind("mark", 0) == 0) Log("==================== MARK %d (t=%.3f) %s ====================", ++mark, lastT, cmd.c_str() + 4);
        else if (cmd.rfind("speed ", 0) == 0) SetSpeed((float)atof(cmd.c_str() + 6));
        else if (cmd == "fx on") SetTimeStretchEffect(true);
        else if (cmd == "fx off") SetTimeStretchEffect(false);
        else if (cmd == "pause") PauseMusic(true);
        else if (cmd == "resume") PauseMusic(false);
        else if (cmd == "check") CheckWwiseFunctions();
        else if (cmd == "whowrites") WhoAccesses(2.0f, false);
        else if (cmd == "whoreads") WhoAccesses(2.0f, true);
        else if (cmd.rfind("whowrites ", 0) == 0) WhoAccesses(2.0f, false, SongObjAddr(cmd.c_str() + 10));
        else if (cmd.rfind("whoreads ", 0) == 0) WhoAccesses(2.0f, true, SongObjAddr(cmd.c_str() + 9));
        else if (cmd == "songobj") LogSongObject();
        else if (cmd.rfind("peek ", 0) == 0) Peek(cmd.c_str() + 5);
        else if (cmd.rfind("findnotes ", 0) == 0) FindNoteArrays(cmd.c_str() + 10);
        else if (cmd.rfind("findptr ", 0) == 0) FindPointers(cmd.c_str() + 8);
        else if (cmd.rfind("watchnotes ", 0) == 0) WatchNotes(cmd.c_str() + 11);
        else if (cmd == "ddstate") DdState();
        else if (cmd.rfind("trace ", 0) == 0) TraceAudioCalls((float)atof(cmd.c_str() + 6), cmd.find("nortpc") == std::string::npos);
        else if (cmd.rfind("pokeb ", 0) == 0) PokeByte(cmd.c_str() + 6);
        else if (cmd == "findprov") Log(">>> provider = 0x%08X", (unsigned)FindProvider());
        else if (cmd.rfind("seekrel ", 0) == 0) { float t = -1; GetSongTime(&t); SeekSong((AkTimeMs)((t + (float)atof(cmd.c_str() + 8)) * 1000)); }
        else if (cmd.rfind("seek ", 0) == 0) SeekSong(atoi(cmd.c_str() + 5));
        else if (cmd.rfind("objvol ", 0) == 0) SongVolume((float)atof(cmd.c_str() + 7));
        else if (cmd == "freeze") Freeze(true);
        else if (cmd == "unfreeze") Freeze(false);
        else if (cmd == "tmusic pause") TMusic(true);
        else if (cmd == "tmusic resume") TMusic(false);
        else if (cmd.rfind("pause2 ", 0) == 0) PauseProviderPlayback(cmd.c_str() + 7, true);
        else if (cmd.rfind("resume2 ", 0) == 0) PauseProviderPlayback(cmd.c_str() + 8, false);
        else if (cmd.rfind("hold ", 0) == 0) HoldTimer((float)atof(cmd.c_str() + 5));
        else if (cmd.rfind("jump ", 0) == 0) JumpTimer((float)atof(cmd.c_str() + 5));
        else if (cmd == "unload") break;
        else Log("       unknown command");
    }

    // "unload": so a rebuilt probe can be injected without restarting the game.
    MH_Uninitialize();  // harmless if hooks were never installed
    Sleep(300);
    Log("Probe unloaded.");
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
