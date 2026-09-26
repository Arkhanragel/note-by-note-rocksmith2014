// game.cpp: see game.h. Addresses are RVAs (relative to the exe's load address).
#include "game.h"

#include <windows.h>
#include <imagehlp.h>

#include <cstdint>

#include "log.h"

namespace nbn::game {

namespace {

// ------------------------------------------------------------------ verified addresses (RVAs)
constexpr DWORD kExpectedChecksum = 0x0176EC34;  // PE checksum of the supported exe

constexpr uintptr_t kRoot = 0x00F6062C;               // root pointer of the song/menu structures
constexpr uint32_t kMenuChain[] = {0x28, 0x8C, 0x0};  // -> menu name string
constexpr uintptr_t kPreviewName = 0x00F60514;        // -> "Play_<SongKey>_Preview"
constexpr uint32_t kPreviewChain[] = {0xBC, 0x0};
constexpr uint32_t kSongObjChain[] = {0xB0};          // [root]+0xB0 -> song object
constexpr uintptr_t kSongClockOffset = 0x3B4;         // float in the song object: THE song clock

// Song data and Dynamic Difficulty (from the disassembly of rva 0x3F1B40 / 0x3F20D0, test 24):
constexpr uintptr_t kSongData = 0x78;         // song object -> loaded arrangement (same layout as the SNG file)
constexpr uintptr_t kSongDataLevels = 0x40;   //   vector<Level> begin/end, Level = 0x64 bytes
constexpr uintptr_t kLevelSize = 0x64;
constexpr uintptr_t kLevelNotes = 0x30;       //   Level: vector<Note> begin/end, Note = 0x1C8 bytes
constexpr uintptr_t kNoteSize = 0x1C8;
constexpr uintptr_t kSongDd = 0x7C;           // song object -> Dynamic Difficulty state
constexpr uintptr_t kDdEntries = 0x18;        //   vector of 64-byte entries, one per phrase iteration
constexpr uintptr_t kDdEntrySize = 64;
constexpr uintptr_t kDdLevel = 4;             //   entry+4: current level (int, negative = none)

constexpr uintptr_t kProviderVtable = 0x00DA0E70;  // clock provider: vtable, +0x0C = song object
constexpr uintptr_t kProviderPlayingId = 0xCC;     // Wwise playing ID the clock follows
constexpr uintptr_t kProviderStopped = 0xDA;       // byte: !=0 -> clock does not advance

// Wwise (statically linked). Identified by their queued-message type (see BITACORA.md).
constexpr uintptr_t kPostEventChar = 0x00AC4870;           // PostEvent(const char*, obj, flags, cb, cookie, nExt, ext, pid)
constexpr uintptr_t kExecuteActionOnEventId = 0x00AC4900;  // ExecuteActionOnEvent(eventId, action, obj, ms, curve, pid)
constexpr uintptr_t kGetEventIDFromPlayingID = 0x00ABFE80;

constexpr uintptr_t kSongGameObject = 0x1234;  // Wwise game object of the song (as the game uses it)
constexpr int kActionPause = 1, kActionResume = 2, kCurveLinear = 4;

using PostEventChar_t = uint32_t(__cdecl*)(const char*, uintptr_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
using ExecuteActionOnEvent_t = int(__cdecl*)(uint32_t, int, uintptr_t, int32_t, int, uint32_t);
using GetEventIDFromPlayingID_t = uint32_t(__cdecl*)(uint32_t);

uintptr_t g_base = 0;
bool g_ready = false;
uintptr_t g_provider = 0;
uint32_t g_frozenPid = 0, g_frozenEvent = 0;

// ------------------------------------------------------------------ safe memory access
// __try/__except catches access violations (e.g. while the game is loading and a pointer is null).
bool ReadChain(uintptr_t start, const uint32_t* offs, size_t n, uintptr_t* out) {
    __try {
        uintptr_t a = start;
        for (size_t i = 0; i < n; ++i) {
            a = *(uintptr_t*)a;
            if (!a) return false;
            a += offs[i];
        }
        *out = a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadU32(uintptr_t a, uint32_t* v) {
    __try { *v = *(volatile uint32_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadFloat(uintptr_t a, float* v) {
    __try { *v = *(volatile float*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteByte(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadText(uintptr_t a, std::string* s) {
    char buf[128];
    __try {
        const char* p = (const char*)a;
        size_t i = 0;
        for (; i < sizeof(buf) - 1 && p[i]; ++i) {
            if (p[i] < 32 || p[i] > 126) return false;
            buf[i] = p[i];
        }
        buf[i] = 0;
        if (!i) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    *s = buf;
    return true;
}

// A function start compiled by MSVC: 16-byte aligned, preceded by int3 padding or a ret,
// starting with push ebp; mov ebp, esp (55 8B EC). Protects against calling a wrong address.
bool LooksLikeFunction(uintptr_t rva) {
    __try {
        const uint8_t* p = (const uint8_t*)(g_base + rva);
        return (rva % 16 == 0) && (p[-1] == 0xCC || p[-1] == 0xC3) && p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t SongObject() {
    uintptr_t a;
    uint32_t obj;
    if (!ReadChain(g_base + kRoot, kSongObjChain, 1, &a) || !ReadU32(a, &obj)) return 0;
    return obj;
}

bool IsProvider(uintptr_t p, uintptr_t song) {
    uint32_t vt, back;
    return ReadU32(p, &vt) && ReadU32(p + 0x0C, &back) && vt == g_base + kProviderVtable && back == song;
}

// The provider isn't referenced from the song object, so scan the heap for an object whose vtable
// is the provider's and whose +0x0C points back to the song object (<1 ms in practice).
uintptr_t FindProvider() {
    const uintptr_t song = SongObject();
    if (!song) return 0;
    if (g_provider && IsProvider(g_provider, song)) return g_provider;
    const uintptr_t vt = g_base + kProviderVtable;
    MEMORY_BASIC_INFORMATION mbi{};
    g_provider = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi));
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & PAGE_GUARD) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            continue;
        const uint32_t* p = (const uint32_t*)mbi.BaseAddress;
        const size_t n = mbi.RegionSize / 4;
        __try {
            for (size_t i = 0; i + 3 < n; ++i)
                if (p[i] == vt && p[i + 3] == song) { g_provider = (uintptr_t)&p[i]; break; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_provider) break;
    }
    Log("clock provider: 0x%08X", (unsigned)g_provider);
    return g_provider;
}

}  // namespace

bool Init() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    DWORD headerSum = 0, checksum = 0;
    MapFileAndCheckSumA(exe, &headerSum, &checksum);
    Log("game: %s base=0x%08X checksum=0x%08X", exe, (unsigned)g_base, checksum);
    if (checksum != kExpectedChecksum) {
        Log("UNSUPPORTED game version (expected checksum 0x%08X). Note-by-Note stays disabled.", kExpectedChecksum);
        return false;
    }
    // The code is decrypted in memory at startup. Wait until our functions look like functions.
    for (int i = 0; i < 1200; ++i) {
        if (LooksLikeFunction(kPostEventChar) && LooksLikeFunction(kExecuteActionOnEventId) &&
            LooksLikeFunction(kGetEventIDFromPlayingID)) {
            g_ready = true;
            Log("game code verified after %d ms", i * 100);
            return true;
        }
        Sleep(100);
    }
    Log("game functions not found in memory. Note-by-Note stays disabled.");
    return false;
}

bool GetMenu(std::string* menu) {
    uintptr_t a;
    return g_ready && ReadChain(g_base + kRoot, kMenuChain, 3, &a) && ReadText(a, menu);
}

bool GetSongKey(std::string* key) {
    uintptr_t a;
    std::string name;
    if (!g_ready || !ReadChain(g_base + kPreviewName, kPreviewChain, 2, &a) || !ReadText(a, &name)) return false;
    if (name.rfind("Play_", 0) != 0) return false;
    size_t end = name.rfind("_Preview");
    if (end == std::string::npos) end = name.rfind("_Invalid");  // song previews disabled in the options
    if (end == std::string::npos || end <= 5) return false;
    *key = name.substr(5, end - 5);
    return true;
}

bool GetSongTime(double* t) {
    const uintptr_t song = SongObject();
    float f;
    if (!g_ready || !song || !ReadFloat(song + kSongClockOffset, &f)) return false;
    *t = f;
    return true;
}

bool GetPhraseLevels(std::vector<int>* levels) {
    const uintptr_t song = SongObject();
    uint32_t dd, b, e;
    if (!g_ready || !song || !ReadU32(song + kSongDd, &dd) || !ReadU32(dd + kDdEntries, &b) || !ReadU32(dd + kDdEntries + 4, &e))
        return false;
    if (e < b || (e - b) % kDdEntrySize || (e - b) / kDdEntrySize > 10000) return false;
    levels->clear();
    for (uint32_t p = b; p < e; p += kDdEntrySize) {
        uint32_t lv;
        if (!ReadU32(p + kDdLevel, &lv)) return false;
        levels->push_back((int)lv);
    }
    return true;
}

bool GetLevelNoteCounts(std::vector<int>* counts) {
    const uintptr_t song = SongObject();
    uint32_t data, b, e;
    if (!g_ready || !song || !ReadU32(song + kSongData, &data) || !ReadU32(data + kSongDataLevels, &b) ||
        !ReadU32(data + kSongDataLevels + 4, &e))
        return false;
    if (e < b || (e - b) % kLevelSize || (e - b) / kLevelSize > 100) return false;
    counts->clear();
    for (uint32_t lv = b; lv < e; lv += kLevelSize) {
        uint32_t nb, ne;
        if (!ReadU32(lv + kLevelNotes, &nb) || !ReadU32(lv + kLevelNotes + 4, &ne) || ne < nb) return false;
        counts->push_back((int)((ne - nb) / kNoteSize));
    }
    return true;
}

bool Freeze() {
    const uintptr_t prov = FindProvider();
    uint32_t pid;
    if (!prov || !ReadU32(prov + kProviderPlayingId, &pid)) return false;
    const uint32_t ev = ((GetEventIDFromPlayingID_t)(g_base + kGetEventIDFromPlayingID))(pid);
    if (!ev) { Log("freeze: the clock's playback is not active"); return false; }
    const int r = ((ExecuteActionOnEvent_t)(g_base + kExecuteActionOnEventId))(ev, kActionPause, kSongGameObject, 0, kCurveLinear, pid);
    WriteByte(prov + kProviderStopped, 1);
    g_frozenPid = pid;
    g_frozenEvent = ev;
    if (r != 1) Log("freeze: pause returned %d", r);
    return true;
}

bool Unfreeze() {
    const uintptr_t prov = FindProvider();
    if (g_frozenEvent)
        ((ExecuteActionOnEvent_t)(g_base + kExecuteActionOnEventId))(g_frozenEvent, kActionResume, kSongGameObject, 0,
                                                                      kCurveLinear, g_frozenPid);
    if (prov) WriteByte(prov + kProviderStopped, 0);
    g_frozenPid = g_frozenEvent = 0;
    return true;
}

void PostUiEvent(const char* name) {
    if (g_ready && name && *name)
        ((PostEventChar_t)(g_base + kPostEventChar))(name, kSongGameObject, 0, nullptr, nullptr, 0, nullptr, 0);
}

void ResetSongCache() {
    g_provider = 0;
    g_frozenPid = g_frozenEvent = 0;
}

}  // namespace nbn::game
