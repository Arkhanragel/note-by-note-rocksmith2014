// GuitarTapShared.h: the shared-memory "guitar tap" between our RS_ASIO build and the mod.
//
// WHY: RS_ASIO owns the ASIO audio interface (the Focusrite), so nothing else can open it while the
// game runs. Our RS_ASIO build copies the raw guitar signal (input channel 0, before RS_ASIO's software
// volume) into this ring buffer on every ASIO buffer switch. The Note-by-Note mod reads the latest
// samples from it for pitch detection. Because it's a named Windows file mapping, the Python tools
// can read it too (handy for debugging while the game runs).
//
// LAYOUT: a header followed by a float ring buffer. The writer stores samples first, then publishes
// the new total count in writePos. A reader takes writePos and reads the samples just before it.
// The ring holds ~1.4 s, far more than any reader needs (the pitch window is 2048-4096 samples).
#pragma once
#include <stdint.h>

namespace nbn {

constexpr const wchar_t* kGuitarTapName = L"Local\\NoteByNote_GuitarInput";
constexpr uint32_t kGuitarTapMagic = 0x314E424E;  // "NBN1" in memory order
constexpr uint32_t kGuitarTapVersion = 1;
constexpr uint32_t kGuitarTapCapacity = 1u << 16;  // samples, a power of two (65536 = 1.37 s at 48 kHz)

struct GuitarTapHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sampleRate;
    uint32_t capacity;
    volatile int64_t writePos;  // total samples ever written. Written with InterlockedExchange64 (atomic on x86)
    uint32_t writerPid;
    uint32_t blockFrames;       // frames per ASIO buffer (latency info)
    volatile uint32_t lastWriteTick;  // GetTickCount() of the last write (to detect a stopped stream)
    uint32_t reserved[7];
};

struct GuitarTapShared {
    GuitarTapHeader h;
    float samples[kGuitarTapCapacity];
};

}  // namespace nbn
