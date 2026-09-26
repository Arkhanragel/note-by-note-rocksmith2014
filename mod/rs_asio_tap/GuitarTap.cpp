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
static float s_scratch[8192];  // conversion buffer (ASIO buffers are far smaller than this)

static bool Init() {
    if (s_shared) return true;
    if (s_initFailed) return false;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    (DWORD)sizeof(nbn::GuitarTapShared), nbn::kGuitarTapName);
    if (!map) { s_initFailed = true; return false; }
    // The mapping stays open for the lifetime of the process (the handle is deliberately not closed).
    s_shared = (nbn::GuitarTapShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nbn::GuitarTapShared));
    if (!s_shared) { s_initFailed = true; return false; }
    s_shared->h.capacity = nbn::kGuitarTapCapacity;
    s_shared->h.version = nbn::kGuitarTapVersion;
    s_shared->h.writerPid = GetCurrentProcessId();
    s_shared->h.magic = nbn::kGuitarTapMagic;  // set last: readers check it
    rslog::info_ts() << "GuitarTap: shared memory ready (" << sizeof(nbn::GuitarTapShared) << " bytes)" << std::endl;
    return true;
}

void Write(const void* owner, const void* asioBuffer, ASIOSampleType asioType, unsigned asioSampleSize,
           unsigned numFrames, unsigned sampleRate) {
    if (!asioBuffer || !numFrames || numFrames > 8192 || !Init()) return;

    // Only one input client feeds the tap. Another may take over if the owner has gone quiet.
    const DWORD now = GetTickCount();
    if (s_owner != owner) {
        if (s_owner && now - s_shared->h.lastWriteTick < 1000) return;
        s_owner = owner;
        rslog::info_ts() << "GuitarTap: new writer, " << sampleRate << " Hz, " << numFrames << " frames/buffer" << std::endl;
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
