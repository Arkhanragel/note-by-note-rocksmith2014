// GuitarTapShared.h: the shared-memory "guitar tap" between our RS_ASIO build and the mod.
//
// WHY: RS_ASIO owns the ASIO audio interface (the Focusrite), so nothing else can open it while the
// game runs. Our RS_ASIO build copies the raw guitar signal (the guitar input's channel, before
// RS_ASIO's software volume) into this ring buffer on every ASIO buffer switch. The Note-by-Note mod
// reads the latest samples from it for pitch detection. Because it's a named Windows file mapping, the
// Python tools can read it too (handy for debugging while the game runs).
//
// LAYOUT: a header followed by a float ring buffer. The writer stores samples first, then publishes
// the new total count in writePos. A reader takes writePos and reads the samples just before it.
// The ring holds ~1.4 s, far more than any reader needs (the pitch window is 2048-4096 samples).
//
// ONE TAP PER GAME PROCESS (since 0.3.2): the mapping's name ends with the process id. With one fixed
// name (0.3.1 and before) a second game instance wrote into the same ring (both signals interleaved:
// no note could be found in it), and a game started while the previous one was still closing attached
// to the dying instance's tap and heard nothing for the whole session.
//
// WHICH INPUT (since 0.3.2): RS_ASIO can have several inputs enabled ([Asio.Input.0], [Asio.Input.1],
// [Asio.Input.Mic]). The tap carries ONE of them, chosen by GuitarTapTakesOver below; the header says
// which, so the mod's log shows it.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

namespace nbn {

// The mapping's name is this + "_<process id>" (GuitarTapNameFor). The bare name was the whole name
// up to 0.3.1: the mod still opens it when it finds an older RS_ASIO build next to it.
constexpr const wchar_t* kGuitarTapName = L"Local\\NoteByNote_GuitarInput";
constexpr uint32_t kGuitarTapMagic = 0x314E424E;  // "NBN1" in memory order
constexpr uint32_t kGuitarTapVersion = 1;
constexpr uint32_t kGuitarTapCapacity = 1u << 16;  // samples, a power of two (65536 = 1.37 s at 48 kHz)
constexpr uint32_t kGuitarTapMicInput = 100;       // GuitarTapHeader::input for [Asio.Input.Mic]

struct GuitarTapHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sampleRate;
    uint32_t capacity;
    volatile int64_t writePos;  // total samples ever written. Written with InterlockedExchange64 (atomic on x86)
    uint32_t writerPid;
    uint32_t blockFrames;       // frames per ASIO buffer (latency info)
    volatile uint32_t lastWriteTick;  // GetTickCount() of the last write (to detect a stopped stream)
    // The two below are 0 in a tap written by 0.3.1 or older (they were reserved): 0 = not known.
    volatile uint32_t input;    // 1 + N for [Asio.Input.N]; kGuitarTapMicInput for [Asio.Input.Mic]
    volatile uint32_t channel;  // 1 + the ASIO channel that input reads
    uint32_t reserved[5];
};

struct GuitarTapShared {
    GuitarTapHeader h;
    float samples[kGuitarTapCapacity];
};

// The name of the tap of the game process `pid`. `out` needs room for 64 characters.
inline void GuitarTapNameFor(uint32_t pid, wchar_t* out, size_t count) {
    swprintf(out, count, L"%ls_%u", kGuitarTapName, pid);
}

// ---- which input feeds the tap
// N of [Asio.Input.N] from RS_ASIO's name for an input, "{ASIO IN 2}" (the microphone input is the
// third: 2). 0 when the name isn't like that.
inline int GuitarTapInputIndex(const wchar_t* deviceId) {
    static const wchar_t kPrefix[] = L"{ASIO IN ";
    const size_t prefixLen = sizeof(kPrefix) / sizeof(kPrefix[0]) - 1;
    if (!deviceId || wcsncmp(deviceId, kPrefix, prefixLen) != 0) return 0;
    const long n = wcstol(deviceId + prefixLen, nullptr, 10);
    return n >= 0 && n < 64 ? (int)n : 0;
}

// The game listens to its first guitar input for a single player (seen on both game builds: with
// [Asio.Input.0] on an empty channel and the guitar on [Asio.Input.1], the game hears no guitar). So the
// tap follows the same rule: the enabled guitar input with the lowest number, and the microphone input
// only when there is nothing else. Lower value = preferred.
inline int GuitarTapPriority(int inputIndex, bool microphone) { return (microphone ? 1000 : 0) + inputIndex; }

// True if the input with `priority` should feed the tap from now on, instead of the current owner.
// It takes over at once when it is preferred (or when nothing feeds the tap yet); a less preferred
// input only when the owner has stopped writing (`ownerQuiet`: nothing for a second), and it hands the
// tap back as soon as the preferred one writes again.
// Before 0.3.2 the first input to write kept the tap, and RS_ASIO calls its inputs in the order of
// their memory addresses: with several inputs enabled the tap could carry an empty one.
inline bool GuitarTapTakesOver(bool hasOwner, int ownerPriority, bool ownerQuiet, int priority) {
    return !hasOwner || ownerQuiet || priority < ownerPriority;
}

}  // namespace nbn
