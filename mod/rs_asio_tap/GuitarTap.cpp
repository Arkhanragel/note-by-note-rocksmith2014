// GuitarTap: added to RS_ASIO by the Note-by-Note project. See GuitarTap.h and GuitarTapShared.h.
//
// This runs on the ASIO driver's callback thread every few milliseconds, so it has to be quick
// and must never block: no locks, no allocation after the first call, no logging in the hot path.
#include "stdafx.h"
#include "GuitarTap.h"
#include "AudioProcessing.h"
#include "GuitarTapShared.h"

namespace GuitarTap {

static nbn::GuitarTapShared* s_shared = nullptr;
static bool s_initFailed = false;
static const void* s_owner = nullptr;
static int s_ownerPriority = 0;
static int s_forcedInput = -1;   // TapInput in NoteByNote.ini: only this input feeds the tap (-1 = automatic)
static float s_scratch[8192];  // conversion buffer (ASIO buffers are far smaller than this)

// The folder of the game's exe, with a trailing backslash (RS_ASIO.ini and NoteByNote.ini are there).
static std::wstring GameDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

static bool Init() {
    if (s_shared) return true;
    if (s_initFailed) return false;
    // One tap per game process: the name ends with the process id (why: GuitarTapShared.h).
    wchar_t name[64];
    nbn::GuitarTapNameFor(GetCurrentProcessId(), name, 64);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    (DWORD)sizeof(nbn::GuitarTapShared), name);
    if (!map) { s_initFailed = true; return false; }
    // The mapping stays open for the lifetime of the process (the handle is deliberately not closed).
    s_shared = (nbn::GuitarTapShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nbn::GuitarTapShared));
    if (!s_shared) { s_initFailed = true; return false; }
    s_shared->h.capacity = nbn::kGuitarTapCapacity;
    s_shared->h.version = nbn::kGuitarTapVersion;
    s_shared->h.writerPid = GetCurrentProcessId();
    s_shared->h.magic = nbn::kGuitarTapMagic;  // set last: readers check it
    s_forcedInput = (int)GetPrivateProfileIntW(L"NoteByNote", L"TapInput", -1, (GameDir() + L"NoteByNote.ini").c_str());
    rslog::info_ts() << "GuitarTap: shared memory ready (" << sizeof(nbn::GuitarTapShared) << " bytes)" << std::endl;
    if (s_forcedInput >= 0)
        rslog::info_ts() << "GuitarTap: NoteByNote.ini has TapInput=" << s_forcedInput << ": only that input is used" << std::endl;
    return true;
}

void Write(const void* owner, const wchar_t* deviceId, bool microphone, unsigned channel, const void* asioBuffer,
           ASIOSampleType asioType, unsigned asioSampleSize, unsigned numFrames, unsigned sampleRate) {
    if (!asioBuffer || !numFrames || numFrames > 8192 || !Init()) return;

    const DWORD now = GetTickCount();
    if (s_owner != owner) {
        // Another input than the one feeding the tap: does it take over? (the rule: GuitarTapTakesOver)
        int index = 0;
        if (!deviceId || swscanf(deviceId, L"{ASIO IN %d}", &index) != 1) index = 0;
        if (s_forcedInput >= 0 && index != s_forcedInput) return;
        const int priority = nbn::GuitarTapPriority(index, microphone);
        const bool ownerQuiet = now - s_shared->h.lastWriteTick >= 1000;
        if (!nbn::GuitarTapTakesOver(s_owner != nullptr, s_ownerPriority, ownerQuiet, priority)) return;
        s_owner = owner;
        s_ownerPriority = priority;
        s_shared->h.input = microphone ? nbn::kGuitarTapMicInput : (uint32_t)index + 1;
        s_shared->h.channel = channel + 1;
        // (A change of input is rare: the start of the stream, or an input that stopped. Logging is fine here.)
        rslog::info_ts() << "GuitarTap: listening to [Asio.Input." << (microphone ? std::string("Mic") : std::to_string(index))
                         << "], channel " << channel << ", " << sampleRate << " Hz, " << numFrames << " frames/buffer" << std::endl;
    }

    // Convert whatever format the driver uses (usually 24/32-bit int) to 32-bit float.
    AudioProcessing::CopyConvertFormat((const BYTE*)asioBuffer, asioType, (WORD)asioSampleSize, numFrames,
                                       (BYTE*)s_scratch, ASIOSTFloat32LSB, (WORD)sizeof(float));

    // Copy into the ring, then publish the new total. Readers never see a count ahead of the data.
    nbn::GuitarTapHeader& h = s_shared->h;
    const int64_t pos = h.writePos;
    const uint32_t mask = nbn::kGuitarTapCapacity - 1;
    for (unsigned i = 0; i < numFrames; ++i)
        s_shared->samples[(uint32_t)(pos + i) & mask] = s_scratch[i];
    h.sampleRate = sampleRate;
    h.blockFrames = numFrames;
    h.lastWriteTick = now;
    InterlockedExchange64(&h.writePos, pos + numFrames);  // full memory barrier + atomic 64-bit store
}

static DWORD WINAPI ModLoaderThread(LPVOID param) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW((HMODULE)param, path, MAX_PATH);
    std::wstring p(path);
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"NoteByNote.dll";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        rslog::info_ts() << "GuitarTap: NoteByNote.dll not found, mod not loaded" << std::endl;
        return 0;
    }
    HMODULE mod = LoadLibraryW(p.c_str());
    rslog::info_ts() << "GuitarTap: NoteByNote.dll " << (mod ? "loaded" : "FAILED to load") << std::endl;
    return 0;
}

void StartModLoader(HMODULE rsAsioModule) {
    HANDLE t = CreateThread(nullptr, 0, ModLoaderThread, rsAsioModule, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace GuitarTap
