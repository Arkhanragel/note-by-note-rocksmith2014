// game.cpp: see game.h. Addresses are RVAs (relative to the exe's load address), per game build
// (kBuilds) or found by byte pattern (signatures.h).
#include "game.h"

#include <windows.h>
#include <imagehlp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "chart.h"
#include "log.h"
#include "music.h"
#include "picking.h"
#include "report.h"
#include "signatures.h"

namespace nbn::game {

namespace {

// ------------------------------------------------------------------ game builds
// What differs between builds of Rocksmith2014.exe, one row per build in kBuilds (like RSMods'
// VersioningStruct, picked by the same exe checksum):
//   - Addresses: the RVAs of code and globals (these always move between builds);
//   - Layout: offsets inside the game's objects (song object, clock, Dynamic Difficulty, clock
//     provider, menu chains). Probably the same in every build (RSMods finds the same for its own
//     chains), but if a tester's report shows one moved, the fix is one value in that build's row.
// The chart's inner layout (levels, notes, chords, beats) is the song-file (SNG) format itself, so
// it stays shared (constants below). The report (report.h) checks all of it while playing.
struct Addresses {
    uintptr_t root = 0;                     // root pointer of the song/menu structures
    uintptr_t previewName = 0;              // -> "Play_<SongKey>_Preview"
    uintptr_t providerVtable = 0;           // vtable of the song clock provider (+Layout::providerSong = song object)
    uintptr_t postEventChar = 0;            // Wwise PostEvent(const char*, obj, flags, cb, cookie, nExt, ext, pid)
    uintptr_t executeActionOnEventId = 0;   // Wwise ExecuteActionOnEvent(eventId, action, obj, ms, curve, pid)
    uintptr_t getEventIdFromPlayingId = 0;  // Wwise GetEventIDFromPlayingID(playingId)
};

// Names as in signatures.h. Code = a function we call; required = the mod can't work without it
// (PostEvent only plays the menu sound).
struct Field {
    const char* name;
    uintptr_t Addresses::*member;
    bool code, required;
};
constexpr Field kFields[] = {
    {"Root", &Addresses::root, false, true},
    {"PreviewName", &Addresses::previewName, false, true},
    {"ProviderVtable", &Addresses::providerVtable, false, true},
    {"PostEventChar", &Addresses::postEventChar, true, false},
    {"ExecuteActionOnEventId", &Addresses::executeActionOnEventId, true, true},
    {"GetEventIDFromPlayingID", &Addresses::getEventIdFromPlayingId, true, true},
};

// Offsets inside the game's objects (bytes). A chain is followed from a global: read the pointer,
// add the offset, repeat.
struct Layout {
    uint32_t menuChain[3];      // [root] -> ... -> the current screen's name ("LearnASong_Game")
    uint32_t preMenuChain[2];   // before the first dialog: a short name stored in place
    uint32_t previewChain[2];   // [previewName] -> ... -> "Play_<SongKey>_Preview"
    uint32_t songObj;           // [root]+songObj -> the song object
    uint32_t songClock;         // float in the song object: THE song clock (seconds)
    uint32_t songData;          // song object -> the loaded arrangement (SNG layout, see below)
    uint32_t songDd;            // song object -> Dynamic Difficulty state
    uint32_t ddEntries;         //   DD state: vector of entries, one per phrase iteration
    uint32_t ddEntrySize;       //   bytes per entry
    uint32_t ddLevel;           //   entry+ddLevel: current level (int, negative = none)
    uint32_t providerSong;      // clock provider -> back to the song object
    uint32_t providerPlayingId; // clock provider: the Wwise playing ID the clock follows
    uint32_t providerStopped;   // clock provider, byte: != 0 -> the clock does not advance
    uint32_t songGameObject;    // the Wwise game object the game plays the song on
    uint32_t greyChain[5];      // [root] -> ... -> float: notes before this song time are greyed out on
                                // the highway (after resuming from the pause screen the game replays a
                                // few seconds with those notes greyed). RSMods' ptr_greyOutNoteTimer
    int32_t loopStart;          // from that float: Riff Repeater's loop, floats: start, the grey time the
    int32_t loopGrey;           //   game really uses there, end (twice: end, end). Outside Riff Repeater
    int32_t loopEnd;            //   they hold 0, the grey time, the song's end, the song's end
};

// Learn & Play, all verified (docs/TECHNICAL.md: "Object layouts", "Pausing the song..."). Other
// builds start from these until their report says otherwise.
constexpr Layout kLayoutLearnAndPlay = {
    {0x28, 0x8C, 0x0}, {0x28, 0x8C}, {0xBC, 0x0},
    0xB0, 0x3B4, 0x78, 0x7C,
    0x18, 64, 4,
    0x0C, 0xCC, 0xDA,
    0x1234,
    {0x68, 0x10, 0x2C, 0x28, 0x3DC},  // checked 2026-09-29: resumed at 133.36 s, grey time stayed 135.962
    -0x1C, -0x18, -0x14,  // checked 2026-10-02: loops 104.011-106.889 and 41.806-47.602 (phrases 18, 7)
};

struct Build {
    const char* name = nullptr;
    DWORD checksum = 0;   // PE checksum computed over the file (MapFileAndCheckSum; RSMods uses the same)
    DWORD timestamp = 0;  // link time in its PE header (0 = unknown). A copy patched on disk keeps this
                          // and the header's CheckSum field, while the computed checksum changes.
    bool verified = false;  // addresses checked by hand on this build; otherwise only compared in the report
    Addresses addr{};
    Layout layout{};
};

constexpr Build kBuilds[] = {
    // Ours: every address verified by disassembly (docs/TECHNICAL.md, "Pausing the song without a pause screen").
    {"Learn & Play (December 2024)", 0x0176EC34, 0x67497D00, true,
     {0x00F6062C, 0x00F60514, 0x00DA0E70, 0x00AC4870, 0x00AC4900, 0x00ABFE80},
     kLayoutLearnAndPlay},
    // The older build most players have (RSMods' "RemasteredSeptember2022"). From RSMods' tables
    // (absolute there, exe base 0x400000), NOT verified; the provider vtable isn't known. Their
    // ExecuteActionOnEvent(id) was 0x30 past the real start on our build, maybe here too.
    {"Remastered (September 2022)", 0x00B13D7C, 0, false,
     {0x00F5F62C, 0x00F5F514, 0, 0x00AC51B0, 0x00AC5240, 0x00AC0850},
     kLayoutLearnAndPlay},  // RSMods: "the offsets stay the same"; the report checks each one
};

// The loaded arrangement ([song + Layout::songData]) has the SNG file's layout (from the disassembly
// of rva 0x3F1B40 / 0x3F20D0, test 24):
constexpr uintptr_t kSongDataBeats = 0x34;    //   vector<Beat>, 16 bytes (the SNG beat): +0 float time,
constexpr uintptr_t kBeatSize = 0x10;         //     +4 int16 measure, +6 int16 beat in the measure,
constexpr uint32_t kBeatFirstOfMeasure = 0x1; //     +8 int32 phrase iteration, +0xC int32 mask
constexpr uintptr_t kSongDataLevels = 0x40;   //   vector<Level> begin/end, Level = 0x64 bytes
constexpr uintptr_t kLevelSize = 0x64;
constexpr uintptr_t kLevelNotes = 0x30;       //   Level: vector<Note> begin/end, Note = 0x1C8 bytes
constexpr uintptr_t kNoteSize = 0x1C8;

// More of the song data, verified 2026-09-27 by rebuilding a whole chart from memory and comparing
// it with the one exported from the song file (tools/memchart.py: identical, 2054 lines):
constexpr uintptr_t kSongDataPis = 0x64;      //   vector<PhraseIteration>, 0x18 bytes: phraseId, start, end
constexpr uintptr_t kPiSize = 0x18;
constexpr uintptr_t kSongDataChords = 0x94;   //   vector<Chord>, 0x48 bytes (the SNG chord template):
constexpr uintptr_t kChordSize = 0x48;        //     +0x4 frets[6] (int8, -1 = not played), +0x10 MIDI notes[6] (int32)
constexpr uintptr_t kChordFrets = 0x4;        //     +0xA fingers[6] (1 = index .. 4 = little, 0 = thumb, -1), +0x28 name
constexpr uintptr_t kChordFingers = 0xA;
constexpr uintptr_t kChordMidi = 0x10;
constexpr uintptr_t kChordName = 0x28;
constexpr size_t kChordNameSize = 32;
constexpr uintptr_t kSongDataTuning = 0x110;  //   vector<int16>: semitones per string vs E standard
constexpr uintptr_t kSongDataCapo = 0x11C;    //   int8, -1 = no capo
constexpr uintptr_t kSongDataLength = 0x148;  //   float SongLength, seconds
// The sections ("intro", "verse"...), found 2026-10-02 by listing every vector of the song data (Seven Nation
// Army remix lead: 23 x 0x58 bytes, the song file's Section): +0 name (32 chars), +0x20 number, +0x24 start,
// +0x28 end (floats, s). The game splits them at every phrase (music::NameSections joins them again).
constexpr uintptr_t kSongDataSections = 0xF4, kSectionSize = 0x58;
// Note (0x1C8 bytes, same field order as the SNG note). Levels are stored in difficulty order.
// +0x12/+0x13: the hand's anchor (the fret under the index finger, and how many frets the hand
// covers), checked 2026-09-29 against Ode to Joy rhythm (notes on frets 2-5: anchor 2, width 4).
constexpr uintptr_t kNoteAnchorFret = 0x12, kNoteAnchorWidth = 0x13;
constexpr uintptr_t kNoteMask = 0x0, kNoteTime = 0xC, kNoteString = 0x10, kNoteFret = 0x11, kNoteChordId = 0x14,
                    kNotePi = 0x20;
// Sustain (float seconds, 0 = short note). In the file it follows vibrato (0x37, packed); in memory
// it is at 0x3C (found 2026-09-27: Sanctuary lead, the note at 20.129 s holds 1.198 s, as in the
// song file). Checked when read: a value that isn't a sane duration counts as 0.
constexpr uintptr_t kNoteSustain = 0x3C;
constexpr uint32_t kMaskChord = 0x2, kMaskIgnore = 0x40000;
// The technique's details (technique.h): in memory these sit 6 bytes later than in the song file
// (+0x2E/+0x2F there), found 2026-09-29 by dumping Ode to Joy's slide and bend notes: slide-to fret
// (int8, -1 = none), unpitched-slide-to fret, and the largest bend (float, steps; the bend's curve
// follows from +0x44 as (time, steps) pairs). Checked when read, like the sustain.
constexpr uintptr_t kNoteSlideTo = 0x34, kNoteSlideUnpitchTo = 0x35, kNoteMaxBend = 0x40;
// The pick direction (int8: 0 = down, 1 = up, -1 = chords), found 2026-10-02: the only byte near the
// technique fields that is 0 on every single note and -1 on every chord (Seven Nation Army remix lead),
// like the song file's PickDirection (+0x32 there).
constexpr uintptr_t kNotePick = 0x36;
// Held chord shapes, per level (the song file's FingerPrint: hand shapes, and arpeggios): 0x14 bytes,
// +0 chord template, +4 start, +8 end, +0xC first note, +0x10 last note (floats, s). The level keeps the
// file's vectors in order from +0 (anchors, anchor extensions, hand shapes, arpeggios, notes at +0x30).
// Checked 2026-10-03 against the song files (counts per level; see the log line "held chord shapes").
constexpr uintptr_t kLevelHandShapes = 0x18, kLevelArpeggios = 0x24, kShapeSize = 0x14;
// A chord's techniques per string: the chord-notes table, found 2026-09-29 (Ode to Joy rhythm: 16
// entries = 16 x 0x948 bytes, the song file's layout): +0x0 mask[6] uint32, +0x18 bend curves[6]
// (32 x (float time, float steps, 4 bytes) + int32 count, 0x184 bytes each), +0x930 slide-to[6] int8,
// +0x936 unpitched-slide-to[6] int8, +0x93C vibrato[6] int16. The chord note points into it (+0x18,
// -1 = none).
constexpr uintptr_t kSongDataChordNotes = 0xAC, kChordNotesSize = 0x948, kNoteChordNotesId = 0x18;
constexpr uintptr_t kCnMask = 0x0, kCnBends = 0x18, kCnBendSize = 0x184, kCnBendCount = 0x180, kCnSlideTo = 0x930,
                    kCnSlideUnpitchTo = 0x936;

constexpr int kActionPause = 1, kActionResume = 2, kCurveLinear = 4;

using PostEventChar_t = uint32_t(__cdecl*)(const char*, uintptr_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
using ExecuteActionOnEvent_t = int(__cdecl*)(uint32_t, int, uintptr_t, int32_t, int, uint32_t);
using GetEventIDFromPlayingID_t = uint32_t(__cdecl*)(uint32_t);

uintptr_t g_base = 0;
size_t g_imageSize = 0;
Addresses g_addr;  // the addresses in use (from kBuilds, or found by pattern)
Layout g_lay = kLayoutLearnAndPlay;  // the offsets in use (the build's row; unknown build: L&P's)
bool g_ready = false;
uintptr_t g_provider = 0;
uint32_t g_frozenPid = 0, g_frozenEvent = 0;
// Wwise counts the pauses of a playback: paused twice, it needs two resumes. The hold watchdog pauses
// again when the clock says the song went on (right after a song start or the game's own resume the
// clock jumps ahead by itself, so it does that with the first pause still in place); with one resume the
// music stayed paused and the song stood still with nothing holding it. So: the pauses not yet resumed
// on g_frozenPid, all taken back by Unfreeze.
int g_pauses = 0;
// Checks for the report: the song clock while frozen, and after resuming.
double g_freezeClock = 0, g_resumeClock = 0;
DWORD g_freezeTick = 0, g_resumeTick = 0;

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

bool ReadBytes(uintptr_t a, void* dst, size_t n) {
    __try { std::memcpy(dst, (const void*)a, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Reads a std::vector (begin, end pointers) of fixed-size elements into a byte buffer.
// maxCount is a sanity limit against garbage while the game is loading.
bool ReadVector(uintptr_t vecAddr, size_t elemSize, size_t maxCount, std::vector<uint8_t>* out, uint32_t* begin = nullptr) {
    uint32_t b, e;
    if (!ReadU32(vecAddr, &b) || !ReadU32(vecAddr + 4, &e) || e < b || (e - b) % elemSize || (e - b) / elemSize > maxCount)
        return false;
    if (begin) *begin = b;
    out->resize(e - b);
    return e == b || ReadBytes(b, out->data(), e - b);
}

// A field of type T at byte offset off of a buffer read with ReadVector.
template <typename T>
T At(const std::vector<uint8_t>& v, size_t off) {
    T x;
    std::memcpy(&x, v.data() + off, sizeof(T));
    return x;
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
    if (!ReadChain(g_base + g_addr.root, &g_lay.songObj, 1, &a) || !ReadU32(a, &obj)) return 0;
    return obj;
}

bool IsProvider(uintptr_t p, uintptr_t song) {
    uint32_t vt, back;
    return ReadU32(p, &vt) && ReadU32(p + g_lay.providerSong, &back) && vt == g_base + g_addr.providerVtable && back == song;
}

// The provider isn't referenced from the song object, so scan the heap for an object whose vtable
// is the provider's and whose +providerSong points back to the song object (<1 ms in practice).
uintptr_t FindProvider() {
    const uintptr_t song = SongObject();
    if (!song) return 0;
    if (g_provider && IsProvider(g_provider, song)) return g_provider;
    const uintptr_t vt = g_base + g_addr.providerVtable;
    MEMORY_BASIC_INFORMATION mbi{};
    g_provider = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi));
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & PAGE_GUARD) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            continue;
        const uint32_t* p = (const uint32_t*)mbi.BaseAddress;
        const size_t n = mbi.RegionSize / 4, back = g_lay.providerSong / 4;  // in 4-byte words
        __try {
            for (size_t i = 0; i + back < n; ++i)
                if (p[i] == vt && p[i + back] == song) { g_provider = (uintptr_t)&p[i]; break; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_provider) break;
    }
    Log("clock provider: 0x%08X", (unsigned)g_provider);
    return g_provider;
}

// ------------------------------------------------------------------ finding addresses by pattern
// The byte patterns of signatures.h are searched in the exe's executable sections in memory (after
// the protection has decrypted them). This lets the mod work on game builds nobody verified by
// hand, as long as their code is the same around these addresses.

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> any;  // 1 = this byte can be anything
};

Pattern ParsePattern(const char* s) {
    Pattern p;
    while (*s) {
        if (*s == ' ') { ++s; continue; }
        if (s[0] == '?') {
            p.bytes.push_back(0);
            p.any.push_back(1);
        } else {
            p.bytes.push_back((uint8_t)strtoul(std::string(s, 2).c_str(), nullptr, 16));
            p.any.push_back(0);
        }
        s += 2;
    }
    return p;
}

// Matches of a pattern in [lo, hi) (a readable region). Stops once the total passes 1.
// Kept free of C++ objects so __try can guard it (a page can disappear while we read).
int FindIn(const uint8_t* pat, const uint8_t* any, size_t len, uintptr_t lo, uintptr_t hi, int already,
           uintptr_t* first) {
    int n = 0;
    if (hi - lo < len) return 0;
    __try {
        const uint8_t* p = (const uint8_t*)lo;
        const uint8_t* last = (const uint8_t*)(hi - len);
        while (p <= last) {
            p = (const uint8_t*)std::memchr(p, pat[0], (size_t)(last - p) + 1);  // patterns start with a fixed byte
            if (!p) break;
            size_t i = 1;
            while (i < len && (any[i] || p[i] == pat[i])) ++i;
            if (i == len) {
                if (already + n == 0) *first = (uintptr_t)p;
                if (already + ++n > 1) break;
            }
            ++p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

// Matches of a pattern in the exe's executable sections: 0, 1, or 2 (= "more than one").
int FindPattern(const Pattern& pat, uintptr_t* first) {
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && n < 2; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t a = g_base + sec[s].VirtualAddress;
        const uintptr_t end = a + sec[s].Misc.VirtualSize;
        while (a < end && n < 2) {  // region by region: skip pages that can't be read
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
            const uintptr_t regionEnd = std::min<uintptr_t>(end, (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize);
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD))
                n += FindIn(pat.bytes.data(), pat.any.data(), pat.bytes.size(), a, regionEnd, n, first);
            a = regionEnd;
        }
    }
    return n;
}

struct Found {
    uintptr_t rva = 0;  // 0 = not found
    bool conflict = false;
    std::string detail;  // what each pattern gave, for the report
};

// Finds one address with every pattern of that name; they must agree.
Found FindByPattern(const char* name, bool code) {
    Found f;
    int agree = 0, total = 0;
    for (const auto& sig : sig::kSignatures) {
        if (std::strcmp(sig.name, name) != 0) continue;
        ++total;
        uintptr_t match = 0;
        const int n = FindPattern(ParsePattern(sig.pattern), &match);
        uintptr_t rva = 0;
        if (n == 1) {
            uint32_t v;
            if (sig.kind == sig::Kind::Func) rva = match - g_base;
            else if (ReadU32(match + sig.capture, &v) && v > g_base && v < g_base + g_imageSize) rva = v - g_base;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), total > 1 ? ", %s" : "%s",
                      n == 0 ? "no match" : n > 1 ? "several matches" : rva ? "" : "bad value");
        if (rva) std::snprintf(buf, sizeof(buf), total > 1 ? ", 0x%06X" : "0x%06X", (unsigned)rva);
        f.detail += buf;
        if (!rva) continue;
        if (f.rva && rva != f.rva) f.conflict = true;
        if (!f.rva) f.rva = rva;
        ++agree;
    }
    // A function must also look like one (and it's where the code starts, not a guess).
    if (f.rva && code && !LooksLikeFunction(f.rva)) {
        f.detail += " (not a function start)";
        f.rva = 0;
    }
    if (f.conflict) f.rva = 0;
    char buf[48];
    std::snprintf(buf, sizeof(buf), " [%d of %d patterns]", agree, total);
    f.detail += buf;
    return f;
}

// All addresses by pattern. With `report`, writes one report line per address, comparing with the
// build's table (verified or RSMods' guess). True if every required address was found.
bool FindAllByPattern(Addresses* out, const Build* build, bool report) {
    bool all = true;
    for (const auto& field : kFields) {
        const Found f = FindByPattern(field.name, field.code);
        out->*field.member = f.rva;
        if (!f.rva && field.required) all = false;
        if (!report) continue;
        std::string cmp;
        if (build && build->addr.*field.member) {
            const uintptr_t t = build->addr.*field.member;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "; %s 0x%06X: %s", build->verified ? "verified" : "RSMods",
                          (unsigned)t, f.rva == t ? "same" : "DIFFERENT");
            cmp = buf;
        }
        report::Line("  %-24s %s%s%s", field.name, f.rva ? "found" : field.required ? "NOT FOUND" : "not found (optional)",
                     (" (" + f.detail + ")").c_str(), cmp.c_str());
    }
    return all;
}

double ClockNow() {
    double t = -1;
    GetSongTime(&t);
    return t;
}

}  // namespace

bool Init(bool allowUnverified, bool patternsOnly) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    DWORD headerSum = 0, checksum = 0;
    MapFileAndCheckSumA(exe, &headerSum, &checksum);
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    const DWORD timestamp = nt->FileHeader.TimeDateStamp;
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExA(exe, GetFileExInfoStandard, &fa);
    // Only the file name goes into the log and the report (testers send them): the folder path can
    // hold the player's user name, and the version is told by the numbers, not by where it is.
    const char* slash = std::strrchr(exe, '\\');
    const char* exeName = slash ? slash + 1 : exe;
    Log("game: %s base=0x%08X checksum=0x%08X header=0x%08X time=0x%08X size=%lu", exeName, (unsigned)g_base, checksum,
        headerSum, timestamp, fa.nFileSizeLow);
    report::Line("Game: %s", exeName);
    report::Line("Exe: checksum 0x%08X, header checksum 0x%08X, link time 0x%08X, %lu bytes, loaded at 0x%08X",
                 checksum, headerSum, timestamp, fa.nFileSizeLow, (unsigned)g_base);

    // Which build: by checksum, or the same build patched on disk (header values still match).
    const Build* build = nullptr;
    bool patched = false;
    for (const auto& b : kBuilds)
        if (checksum == b.checksum) build = &b;
    if (!build)
        for (const auto& b : kBuilds)
            if (b.timestamp && headerSum == b.checksum && timestamp == b.timestamp) { build = &b; patched = true; }
    // Dev test: behave as on an unknown build (only the patterns), on a build we know.
    if (patternsOnly) report::Line("TestPatternsOnly=1: the verified addresses are ignored.");
    const bool verified = build && build->verified && !patternsOnly;
    report::Line("Build: %s%s%s", build ? build->name : "unknown", patched ? ", patched on disk" : "",
                 verified ? " (supported)" : build ? " (not verified yet)" : "");
    Log("build: %s%s", build ? build->name : "unknown", patched ? " (modified on disk)" : "");

    // The offsets inside the game's objects: this build's row (an unknown build tries L&P's). The
    // report lists them, so a tester's report says which values its checks were made with.
    g_lay = build ? build->layout : kLayoutLearnAndPlay;
    report::Line("Layout%s: song +0x%X, clock +0x%X, chart +0x%X, DD +0x%X (+0x%X, %u B, level +0x%X), provider +0x%X/+0x%X/+0x%X, "
                 "menu +0x%X+0x%X, preview +0x%X",
                 build ? "" : " (L&P's, unknown build)", g_lay.songObj, g_lay.songClock, g_lay.songData, g_lay.songDd,
                 g_lay.ddEntries, g_lay.ddEntrySize, g_lay.ddLevel, g_lay.providerSong, g_lay.providerPlayingId,
                 g_lay.providerStopped, g_lay.menuChain[0], g_lay.menuChain[1], g_lay.previewChain[0]);

    // The code is decrypted in memory at startup: wait for it. On a verified build our functions
    // start to look like functions; on another build, wait until the patterns are all found.
    Addresses found;
    bool ok = false;
    const DWORD t0 = GetTickCount();
    if (verified) {
        const Addresses& a = build->addr;
        for (int i = 0; i < 1200 && !ok; ++i) {
            ok = LooksLikeFunction(a.postEventChar) && LooksLikeFunction(a.executeActionOnEventId) &&
                 LooksLikeFunction(a.getEventIdFromPlayingId);
            if (!ok) Sleep(100);
        }
    } else {
        for (int i = 0; i < 120 && !ok; ++i) {
            ok = FindAllByPattern(&found, build, false);
            if (!ok) Sleep(1000);
        }
    }
    Log("game code %s after %lu ms", ok ? "ready" : "NOT found", GetTickCount() - t0);

    // Always report what the patterns find: on a verified build this checks the patterns themselves
    // (they must give the verified addresses), which is what makes them worth trying elsewhere.
    report::Line("Addresses found by pattern:");
    const bool all = FindAllByPattern(&found, build, true);

    if (verified) {
        g_addr = build->addr;
        if (!ok) {
            report::Line("The game's code was not found in memory: Note-by-Note stays off.");
            return false;
        }
        report::Line("Using the verified addresses of this build.");
    } else if (!all) {
        report::Line("Some addresses were not found: Note-by-Note can't work with this game version yet, it stays off.");
        Log("UNSUPPORTED game version: addresses not found. Note-by-Note stays disabled.");
        return false;
    } else if (!allowUnverified) {
        report::Line("Every address was found, but this game version isn't verified yet, so Note-by-Note stays off.");
        report::Line("To test it: set TestUnverifiedGame=1 in NoteByNote.ini and start the game again.");
        Log("unverified game version: stays disabled (TestUnverifiedGame=0)");
        return false;
    } else {
        g_addr = found;
        report::Line("TEST MODE (TestUnverifiedGame=1): using the addresses found by pattern.");
        Log("TEST MODE: unverified game version, using the addresses found by pattern");
    }
    g_ready = true;
    report::Line("");
    report::Line("Checks while playing:");
    return true;
}

void Tick() {
    // One second after a resume, the song clock must be running again.
    if (g_resumeTick && GetTickCount() - g_resumeTick >= 1000) {
        const double t = ClockNow();
        const double moved = t - g_resumeClock;
        report::Limited("resume", 3, "  Resume: the song clock moved %.2f s in the first second: %s", moved,
                        (t >= 0 && moved > 0.5 && moved < 1.6) ? "OK" : "FAILED (or the game was paused)");
        g_resumeTick = 0;
    }
}

bool GetMenu(std::string* menu) {
    uintptr_t a;
    return g_ready && ReadChain(g_base + g_addr.root, g_lay.menuChain, 3, &a) && ReadText(a, menu);
}

bool GetPreMenu(std::string* name) {
    uintptr_t a;
    return g_ready && ReadChain(g_base + g_addr.root, g_lay.preMenuChain, 2, &a) && ReadText(a, name);
}

bool GetSongKey(std::string* key) {
    uintptr_t a;
    std::string name;
    if (!g_ready || !ReadChain(g_base + g_addr.previewName, g_lay.previewChain, 2, &a) || !ReadText(a, &name)) return false;
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
    if (!g_ready || !song || !ReadFloat(song + g_lay.songClock, &f)) return false;
    *t = f;
    return true;
}

namespace {
// Riff Repeater's loop, read next to the grey timer at `a`. false = no loop.
bool ReadLoop(uintptr_t a, double* start, double* end) {
    float s, e, e2;
    if (!ReadFloat(a + g_lay.loopStart, &s) || !ReadFloat(a + g_lay.loopEnd, &e) || !ReadFloat(a + g_lay.loopEnd + 4, &e2))
        return false;
    // Normal play keeps 0 .. the song's end there, and after leaving Riff Repeater (the pause point or
    // the old loop's start) .. the song's end: a loop ends before the song's end. (Both end copies must
    // agree: a wrong offset on another build reads something else, and a false loop would stop every
    // wait outside it; it did ignore the practice bar after Riff Repeater, start 51.17, end = song end.)
    double len = 0;
    if (!(s > 0.001f && e > s + 0.2f && e == e2) || !GetSongLength(&len) || e > len - 0.5) return false;
    *start = s;
    *end = e;
    return true;
}
}  // namespace

bool GetGreyTime(double* t) {
    uintptr_t a;
    float f;
    double s, e;
    if (!g_ready || !ReadChain(g_base + g_addr.root, g_lay.greyChain, 5, &a)) return false;
    // In Riff Repeater the timer above keeps the pause point after the loop starts over (the whole loop
    // then looked greyed out); the game's own value 0x18 before it goes back to the loop's start.
    const uintptr_t at = ReadLoop(a, &s, &e) ? a + g_lay.loopGrey : a;
    if (!ReadFloat(at, &f) || !(f >= 0 && f < 36000)) return false;
    *t = f;
    return true;
}

bool GetLoop(double* start, double* end) {
    uintptr_t a;
    return g_ready && ReadChain(g_base + g_addr.root, g_lay.greyChain, 5, &a) && ReadLoop(a, start, end);
}

bool GetPhraseLevels(std::vector<int>* levels) {
    const uintptr_t song = SongObject();
    uint32_t dd, b, e;
    const Layout& L = g_lay;
    if (!g_ready || !song || !ReadU32(song + L.songDd, &dd) || !ReadU32(dd + L.ddEntries, &b) || !ReadU32(dd + L.ddEntries + 4, &e))
        return false;
    if (!L.ddEntrySize || e < b || (e - b) % L.ddEntrySize || (e - b) / L.ddEntrySize > 10000) return false;
    levels->clear();
    for (uint32_t p = b; p < e; p += L.ddEntrySize) {
        uint32_t lv;
        if (!ReadU32(p + L.ddLevel, &lv)) return false;
        levels->push_back((int)lv);
    }
    return true;
}

bool GetLevelNoteCounts(std::vector<int>* counts) {
    const uintptr_t song = SongObject();
    uint32_t data, b, e;
    if (!g_ready || !song || !ReadU32(song + g_lay.songData, &data) || !ReadU32(data + kSongDataLevels, &b) ||
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

bool GetSongLength(double* len) {
    const uintptr_t data = SongDataAddress();
    float f;
    if (!data || !ReadFloat(data + kSongDataLength, &f) || !(f > 0 && f < 36000)) return false;
    *len = f;
    return true;
}

uintptr_t SongDataAddress() {
    const uintptr_t song = SongObject();
    uint32_t data;
    return (g_ready && song && ReadU32(song + g_lay.songData, &data)) ? data : 0;
}

namespace {

const int kGuitarOpen[6] = {40, 45, 50, 55, 59, 64};  // E2 A2 D3 G3 B3 E4 (MIDI)

// With a capo, an open string sounds at the capo fret (same rule as Rocksmith2014.NET's toMidiNote).
int SoundingFret(int fret, int capo) { return (fret == 0 && capo > 0) ? capo : fret; }

// Guitar or bass: the chord templates store MIDI notes, computed with -12 for bass. This is every
// template note minus what a guitar in this tuning plays there: {-12} = bass, {0} = guitar, {} = no
// chords (can't tell).
std::set<int> ChordOctaveOffsets(const std::vector<uint8_t>& chords, const int tuning[6], int capo) {
    std::set<int> offsets;
    for (size_t i = 0; i < chords.size(); i += kChordSize)
        for (int s = 0; s < 6; ++s) {
            const int8_t fret = At<int8_t>(chords, i + kChordFrets + s);
            if (fret >= 0)
                offsets.insert(At<int32_t>(chords, i + kChordMidi + s * 4) - (kGuitarOpen[s] + tuning[s] + SoundingFret(fret, capo)));
        }
    return offsets;
}

// The beat grid (optional: without it the tab just has no bar lines). Kept only if the times go up,
// so garbage never draws lines.
std::vector<Beat> ReadBeats(uintptr_t data) {
    std::vector<uint8_t> raw;
    std::vector<Beat> beats;
    if (!ReadVector(data + kSongDataBeats, kBeatSize, 100000, &raw)) return beats;
    for (size_t i = 0; i < raw.size(); i += kBeatSize) {
        Beat b;
        b.time = At<float>(raw, i);
        b.measure = At<int16_t>(raw, i + 4);
        b.downbeat = (At<uint32_t>(raw, i + 0xC) & kBeatFirstOfMeasure) != 0;
        if (!(b.time >= 0 && b.time < 3600) || (!beats.empty() && b.time < beats.back().time)) {
            Log("beats: not a beat grid at #%zu (time %.3f), ignored", i / kBeatSize, b.time);
            beats.clear();
            break;
        }
        beats.push_back(b);
    }
    return beats;
}

// Turns the in-memory notes of one difficulty level into Targets.
struct NoteDecoder {
    const std::vector<uint8_t>& chords;  // the chord templates
    const std::vector<uint8_t>& chordNotes;  // each chord note's per-string techniques (may be empty)
    int open[6];                         // MIDI of each open string (bass: an octave down), no capo
    int capo;

    // The note at byte n of `notes`; false = not a usable note (its string is out of range).
    bool Decode(const std::vector<uint8_t>& notes, size_t n, int level, Target* out) const {
        Target t;
        const uint32_t mask = At<uint32_t>(notes, n + kNoteMask);
        const int chordId = At<int32_t>(notes, n + kNoteChordId);
        t.time = At<float>(notes, n + kNoteTime);
        t.level = level;
        t.pi = At<int32_t>(notes, n + kNotePi);
        t.ignore = (mask & kMaskIgnore) != 0;
        const float sus = At<float>(notes, n + kNoteSustain);
        const int anchor = At<int8_t>(notes, n + kNoteAnchorFret), width = At<int8_t>(notes, n + kNoteAnchorWidth);
        if (anchor >= 1 && anchor <= 24 && width >= 1 && width <= 8) {
            t.anchorFret = anchor;
            t.anchorWidth = width;
        }
        t.sustain = (sus > 0 && sus < 60) ? sus : 0;
        const int pick = At<int8_t>(notes, n + kNotePick);
        t.songPick = (pick == 0 || pick == 1) ? pick : -1;
        if (chordId >= 0 && (mask & kMaskChord) && (size_t)chordId * kChordSize < chords.size()) {
            t.chord = true;
            const size_t ch = (size_t)chordId * kChordSize;
            for (int s = 0; s < 6; ++s) {
                t.frets[s] = At<int8_t>(chords, ch + kChordFrets + s);
                const int finger = At<int8_t>(chords, ch + kChordFingers + s);
                t.fingers[s] = (t.frets[s] > 0 && finger >= 0 && finger <= 4) ? finger : -1;
                if (t.frets[s] < 0) continue;
                t.notes[s] = At<int32_t>(chords, ch + kChordMidi + s * 4);
                t.midi.push_back(t.notes[s]);
            }
            const char* name = (const char*)&chords[ch + kChordName];
            t.chordName.assign(name, strnlen(name, kChordNameSize));
            const int cn = At<int32_t>(notes, n + kNoteChordNotesId);
            if (cn >= 0 && (size_t)(cn + 1) * kChordNotesSize <= chordNotes.size()) {
                const size_t e = (size_t)cn * kChordNotesSize;
                for (int s = 0; s < 6; ++s) {
                    technique::Technique& st = t.strings[s];
                    st.mask = At<uint32_t>(chordNotes, e + kCnMask + s * 4);
                    const int to = At<int8_t>(chordNotes, e + kCnSlideTo + s), un = At<int8_t>(chordNotes, e + kCnSlideUnpitchTo + s);
                    st.slideTo = (to >= 0 && to <= 24) ? to : -1;
                    st.slideUnpitchTo = (un >= 0 && un <= 24) ? un : -1;
                    const size_t b = e + kCnBends + s * kCnBendSize;
                    const int used = std::min(32, std::max(0, At<int32_t>(chordNotes, b + kCnBendCount)));
                    for (int k = 0; k < used; ++k) {
                        const float step = At<float>(chordNotes, b + k * 12 + 4);
                        if (step > st.bend && step <= 3) st.bend = step;
                    }
                }
            }
            t.tech = technique::ForChord(mask, t.strings, t.frets, &t.techFret);
        } else {
            t.string = At<int8_t>(notes, n + kNoteString);
            t.fret = At<int8_t>(notes, n + kNoteFret);
            if (t.string < 0 || t.string > 5) return false;
            t.midi.push_back(open[t.string] + SoundingFret(t.fret, capo));
            t.tech.mask = mask;
            t.techFret = t.fret;
            // One finger per fret inside the hand's box (index on the anchor fret).
            if (t.fret > 0 && t.anchorFret > 0) {
                const int f = t.fret - t.anchorFret + 1;
                if (f >= 1 && f <= 4) t.fingers[t.string] = f;
            }
            const int slideTo = At<int8_t>(notes, n + kNoteSlideTo), unpitch = At<int8_t>(notes, n + kNoteSlideUnpitchTo);
            const float bend = At<float>(notes, n + kNoteMaxBend);
            t.tech.slideTo = (slideTo >= 0 && slideTo <= 24) ? slideTo : -1;
            t.tech.slideUnpitchTo = (unpitch >= 0 && unpitch <= 24) ? unpitch : -1;
            t.tech.bend = (bend > 0 && bend <= 3) ? bend : 0;
        }
        *out = std::move(t);
        return true;
    }
};

// Notes played together that the song stores one by one: a double stop (two strings at once, maybe
// sliding together) is often two single notes with the same time, not a chord shape. The mod waited
// for the first and skipped the second (same time = already passed), and the banner showed one note.
// They become one "play these strings together" target (no chord name): both dots and techniques on
// the banner, and the chord detector waits for both strings. `all` is in level, then time order.
std::vector<Target> MergeTogether(std::vector<Target> all) {
    auto asChord = [](Target& t) {
        if (t.chord) return;
        t.chord = true;
        t.frets[t.string] = t.fret;
        t.notes[t.string] = t.midi.empty() ? -1 : t.midi[0];
        t.strings[t.string] = t.tech;
        t.string = t.fret = -1;
    };
    std::vector<Target> out;
    out.reserve(all.size());
    for (auto& t : all) {
        Target* last = out.empty() ? nullptr : &out.back();
        const bool together = last && !t.chord && t.string >= 0 && t.string < 6 && last->level == t.level &&
                              last->pi == t.pi && std::fabs(last->time - t.time) < 0.002 &&
                              (last->chord ? last->chordName.empty() && last->frets[t.string] < 0 : last->string != t.string);
        if (!together) {
            out.push_back(std::move(t));
            continue;
        }
        asChord(*last);
        last->frets[t.string] = t.fret;
        last->notes[t.string] = t.midi.empty() ? -1 : t.midi[0];
        last->strings[t.string] = t.tech;
        last->fingers[t.string] = t.fingers[t.string];
        if (!t.midi.empty()) last->midi.push_back(t.midi[0]);
        std::sort(last->midi.begin(), last->midi.end());
        last->sustain = std::max(last->sustain, t.sustain);
        last->ignore = last->ignore && t.ignore;
        last->tech = technique::ForChord(0, last->strings, last->frets, &last->techFret);
    }
    return out;
}

// What was read, in the log (and a few beats, to check the grid by eye).
struct HeldShape {
    int chord = -1;
    double start = 0, end = 0;
};

// A level's held chord shapes (hand shapes and arpeggios, in time order); empty if they don't look right.
std::vector<HeldShape> ReadShapes(uintptr_t level, size_t chordCount) {
    std::vector<HeldShape> out;
    for (const uintptr_t off : {kLevelHandShapes, kLevelArpeggios}) {
        std::vector<uint8_t> raw;
        if (!ReadVector(level + off, kShapeSize, 20000, &raw)) return {};
        for (size_t i = 0; i < raw.size(); i += kShapeSize) {
            const int chord = At<int32_t>(raw, i);
            const float a = At<float>(raw, i + 4), b = At<float>(raw, i + 8);
            if (chord < -1 || chord >= (int)chordCount || !(a >= 0) || !(b >= a) || b > 7200) return {};
            if (chord >= 0) out.push_back({chord, a, b});
        }
    }
    std::sort(out.begin(), out.end(), [](const HeldShape& x, const HeldShape& y) { return x.start < y.start; });
    return out;
}

// A single note inside a held shape, on a string and fret of its chord (2 strings or more): the shape
// goes with it (the banner shows it, the tab names it), and the chord's finger for this string replaces
// the one guessed from the hand's position. Returns true if it's in one.
bool ApplyShape(Target* t, const std::vector<HeldShape>& shapes, const std::vector<uint8_t>& chords) {
    if (t->chord || t->string < 0 || t->string > 5) return false;
    for (const HeldShape& h : shapes) {
        if (h.start > t->time + 0.001) break;
        if (t->time >= h.end) continue;
        const size_t ch = (size_t)h.chord * kChordSize;
        int frets[6], played = 0;
        for (int s = 0; s < 6; ++s) {
            frets[s] = At<int8_t>(chords, ch + kChordFrets + s);
            played += frets[s] >= 0;
        }
        if (played < 2 || frets[t->string] != t->fret) continue;
        for (int s = 0; s < 6; ++s) {
            t->shapeFrets[s] = frets[s];
            const int f = At<int8_t>(chords, ch + kChordFingers + s);
            t->shapeFingers[s] = (frets[s] > 0 && f >= 0 && f <= 4) ? f : -1;
        }
        const char* name = (const char*)&chords[ch + kChordName];
        t->shapeName.assign(name, strnlen(name, kChordNameSize));
        while (!t->shapeName.empty() && t->shapeName.back() == ' ') t->shapeName.pop_back();
        t->shapeStart = h.start;
        t->shapeEnd = h.end;
        if (t->shapeFingers[t->string] >= 0) t->fingers[t->string] = t->shapeFingers[t->string];
        return true;
    }
    return false;
}

// The song's sections (empty if they don't look right: then the mod just doesn't name them).
std::vector<music::Section> ReadSections(uintptr_t data) {
    std::vector<uint8_t> raw;
    if (!ReadVector(data + kSongDataSections, kSectionSize, 1000, &raw)) return {};
    std::vector<music::SectionPiece> pieces;
    for (size_t i = 0; i < raw.size(); i += kSectionSize) {
        char name[33] = {};
        std::memcpy(name, raw.data() + i, 32);
        bool ok = name[0] != 0;
        for (const char* p = name; *p && ok; ++p) ok = *p > 32 && *p < 127;
        const float a = At<float>(raw, i + 0x24), b = At<float>(raw, i + 0x28);
        if (!ok || !(a >= 0) || !(b >= a) || b > 7200) return {};
        pieces.push_back({name, At<int32_t>(raw, i + 0x20), a, b});
    }
    return music::NameSections(pieces);
}

void LogChart(const Chart& chart, const int tuning[6], size_t chordShapes, const std::vector<Target>& all) {
    size_t sustained = 0;
    double maxSustain = 0;
    for (const auto& t : all)
        if (t.sustain > 0) { ++sustained; maxSustain = std::max(maxSustain, t.sustain); }
    Log("chart from memory: %s, tuning %d %d %d %d %d %d, capo %d, %zu chord shapes, %d levels, %zu phrase iterations, %zu notes",
        chart.arrangement.c_str(), tuning[0], tuning[1], tuning[2], tuning[3], tuning[4], tuning[5], chart.capo,
        chordShapes, chart.Levels(), chart.pis.size(), all.size());
    Log("  %zu beats (last bar %d), %zu notes held (longest %.2f s)", chart.beats.size(),
        chart.beats.empty() ? 0 : chart.beats.back().measure, sustained, maxSustain);
    for (size_t i = 0; i < chart.beats.size() && i < 6; ++i)
        Log("  beat %zu: %.3f s, bar %d%s", i, chart.beats[i].time, chart.beats[i].measure,
            chart.beats[i].downbeat ? ", first of the bar" : "");
    std::string secs;
    for (const auto& s : chart.sections) {
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %.1f", secs.empty() ? "" : ", ", s.name.c_str(), s.start);
        secs += b;
    }
    Log("  sections: %s", secs.empty() ? "(none)" : secs.c_str());
}

}  // namespace

bool ReadSongChart(Chart* chart) {
    const uintptr_t data = SongDataAddress();
    if (!data) return false;
    std::vector<uint8_t> tuningRaw, chords, chordNotes, levels, pis, notes;
    uint32_t levelsBegin = 0;
    int8_t capoRaw;
    if (!ReadVector(data + kSongDataTuning, 2, 8, &tuningRaw) || !ReadBytes(data + kSongDataCapo, &capoRaw, 1) ||
        !ReadVector(data + kSongDataChords, kChordSize, 10000, &chords) ||
        !ReadVector(data + kSongDataLevels, kLevelSize, 100, &levels, &levelsBegin) ||
        !ReadVector(data + kSongDataPis, kPiSize, 10000, &pis))
        return false;
    if (levels.empty() || pis.empty()) return false;  // still loading
    // Optional: without it chords just have no per-string techniques.
    if (!ReadVector(data + kSongDataChordNotes, kChordNotesSize, 10000, &chordNotes)) chordNotes.clear();
    int tuning[6] = {0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < tuningRaw.size() / 2 && i < 6; ++i) tuning[i] = At<int16_t>(tuningRaw, i * 2);
    const int capo = capoRaw > 0 ? capoRaw : 0;

    Chart c;
    const std::set<int> offsets = ChordOctaveOffsets(chords, tuning, capo);
    c.bass = offsets.size() == 1 && *offsets.begin() == -12;
    NoteDecoder dec{chords, chordNotes, {}, capo};
    for (int s = 0; s < 6; ++s) {
        c.open[s] = kGuitarOpen[s] + tuning[s] - (c.bass ? 12 : 0);
        dec.open[s] = c.open[s];
    }
    c.capo = capo;
    for (size_t i = 0; i < pis.size(); i += kPiSize)
        c.pis.push_back({At<int32_t>(pis, i), At<float>(pis, i + 4), At<float>(pis, i + 8)});
    c.beats = ReadBeats(data);
    c.sections = ReadSections(data);

    std::vector<Target> all;
    int maxString = 0;
    std::string shapeCounts;
    int inShape = 0;
    for (size_t lv = 0; lv * kLevelSize < levels.size(); ++lv) {
        if (!ReadVector(levelsBegin + lv * kLevelSize + kLevelNotes, kNoteSize, 100000, &notes)) return false;
        c.levelCounts.push_back((int)(notes.size() / kNoteSize));
        const std::vector<HeldShape> shapes = ReadShapes(levelsBegin + lv * kLevelSize, chords.size() / kChordSize);
        shapeCounts += (shapeCounts.empty() ? "" : " ") + std::to_string(shapes.size());
        for (size_t n = 0; n < notes.size(); n += kNoteSize) {
            Target t;
            if (!dec.Decode(notes, n, (int)lv, &t)) continue;
            inShape += ApplyShape(&t, shapes, chords);
            if (!t.chord) maxString = std::max(maxString, t.string);
            all.push_back(std::move(t));
        }
    }
    c.bassUnsure = offsets.empty() && maxString <= 3;
    c.arrangement = c.bass ? "bass" : (c.bassUnsure ? "guitar or bass" : "guitar");
    all = MergeTogether(std::move(all));
    c.Index(all);
    const picking::Result picks = picking::Assign(&c);
    Log("pick strokes: %s (the song marks %d of %d picked notes \"up\", %d agree with the rhythm)",
        picks.fromSong ? "the song's own" : "suggested from the rhythm", picks.songUps, picks.picked, picks.agree);
    Log("held chord shapes per level: %s; %d single notes inside one (all levels)", shapeCounts.c_str(), inShape);
    *chart = std::move(c);
    LogChart(*chart, tuning, chords.size() / kChordSize, all);
    return true;
}

// The search for the clock provider can take most of a second (seen: 0.7 s, in the first song after the
// game started): done at the first stop, the song ran that far past its first note before it was held.
// So the main loop asks for it ahead of time, while the song plays towards a note. Not found (the song
// is still loading): tried again 1 s later. The one found earlier is checked every time (three reads):
// the game makes a new provider when the song resumes from its pause menu or from Riff Repeater, and
// the first stop after that was just as late.
void PrepareFreeze() {
    static DWORD s_nextTry = 0;
    if (g_provider && IsProvider(g_provider, SongObject())) return;
    if (GetTickCount() < s_nextTry) return;
    if (!FindProvider()) s_nextTry = GetTickCount() + 1000;
}

bool Freeze() {
    const uintptr_t prov = FindProvider();
    uint32_t pid;
    if (!prov) report::Limited("provider", 1, "  Freeze: the song clock provider was NOT found (vtable 0x%06X)", (unsigned)g_addr.providerVtable);
    if (!prov || !ReadU32(prov + g_lay.providerPlayingId, &pid)) return false;
    const uint32_t ev = ((GetEventIDFromPlayingID_t)(g_base + g_addr.getEventIdFromPlayingId))(pid);
    if (!ev) {
        Log("freeze: the clock's playback is not active");
        report::Limited("noevent", 2, "  Freeze: not possible right now, no music playing (playing ID %u; normal around the game's pause screen)", pid);
        return false;
    }
    const int r = ((ExecuteActionOnEvent_t)(g_base + g_addr.executeActionOnEventId))(ev, kActionPause, g_lay.songGameObject, 0, kCurveLinear, pid);
    WriteByte(prov + g_lay.providerStopped, 1);
    g_pauses = (pid == g_frozenPid && g_frozenEvent) ? g_pauses + 1 : 1;
    g_frozenPid = pid;
    g_frozenEvent = ev;
    if (r != 1) Log("freeze: pause returned %d", r);
    g_freezeClock = ClockNow();
    g_freezeTick = GetTickCount();
    g_resumeTick = 0;
    report::Limited("pause", 3, "  Freeze: Wwise pause returned %d (1 = OK), song clock at %.3f s", r, g_freezeClock);
    return true;
}

bool Unfreeze() {
    const uintptr_t prov = FindProvider();
    if (g_frozenEvent) {
        for (int i = 0; i < std::max(1, g_pauses); ++i)  // one resume per pause (see g_pauses)
            ((ExecuteActionOnEvent_t)(g_base + g_addr.executeActionOnEventId))(g_frozenEvent, kActionResume, g_lay.songGameObject, 0,
                                                                          kCurveLinear, g_frozenPid);
        if (g_pauses > 1) Log("resume: the music had been paused %d times, resumed as many", g_pauses);
    }
    g_pauses = 0;
    // Report: while frozen the song clock must not have moved, and it must run again afterwards
    // (checked in Tick). Held less than half a second says too little.
    if (g_freezeTick && GetTickCount() - g_freezeTick >= 500) {
        const double t = ClockNow(), moved = t - g_freezeClock;
        report::Limited("freeze", 3, "  Freeze: held %.1f s, the song clock moved %.3f s: %s",
                        (GetTickCount() - g_freezeTick) / 1000.0, moved, (t >= 0 && std::fabs(moved) < 0.1) ? "OK" : "FAILED");
        g_resumeClock = t;
        g_resumeTick = GetTickCount();
    }
    g_freezeTick = 0;
    if (prov) WriteByte(prov + g_lay.providerStopped, 0);
    g_frozenPid = g_frozenEvent = 0;
    return true;
}

void PostUiEvent(const char* name) {
    if (g_ready && g_addr.postEventChar && name && *name)  // optional: not found = no menu sound
        ((PostEventChar_t)(g_base + g_addr.postEventChar))(name, g_lay.songGameObject, 0, nullptr, nullptr, 0, nullptr, 0);
}

void ResetSongCache() {
    g_provider = 0;
    g_frozenPid = g_frozenEvent = 0;
    g_pauses = 0;
    g_freezeTick = g_resumeTick = 0;
}

}  // namespace nbn::game
