// tap.cpp: see tap.h.
#include "tap.h"

#include <windows.h>

#include "../common/GuitarTapShared.h"

namespace nbn {

namespace {

// Maps the tap called `name`, or returns null when it doesn't exist or isn't this process's.
GuitarTapShared* OpenTap(const wchar_t* name) {
    // Mapped read/WRITE even though we only read: the atomic 64-bit read below uses
    // InterlockedCompareExchange64 (lock cmpxchg8b), which needs write access. With a read-only view it
    // raised an access violation that silently killed the mod's thread (first in-game test).
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!map) return nullptr;  // RS_ASIO hasn't created it yet (or this isn't our RS_ASIO build)
    auto* p = (GuitarTapShared*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GuitarTapShared));
    CloseHandle(map);  // the view keeps the mapping alive
    if (!p) return nullptr;
    // writerPid: the tap must be written by THIS game. (Seen with the fixed name of 0.3.1: a game started
    // while the previous one was still closing attached to the old one's tap. The name went away with the
    // old process, our view kept its memory, and the mod heard nothing until the next start.)
    if (p->h.magic != kGuitarTapMagic || p->h.capacity != kGuitarTapCapacity || p->h.writerPid != GetCurrentProcessId()) {
        UnmapViewOfFile(p);
        return nullptr;
    }
    return p;
}

}  // namespace

bool TapReader::Open() {
    if (shared_) return true;
    wchar_t name[64];
    GuitarTapNameFor(GetCurrentProcessId(), name, 64);
    GuitarTapShared* p = OpenTap(name);
    // An RS_ASIO build from 0.3.1 or before (the setup installs the pair, but a file can be copied by
    // hand): its tap has the bare name. Still only accepted when this process writes it.
    if (!p) p = OpenTap(kGuitarTapName);
    if (!p) return false;
    shared_ = p;
    readPos_ = -1;
    return true;
}

unsigned TapReader::SampleRate() const { return shared_ ? shared_->h.sampleRate : 0; }

uint32_t TapReader::InputId() const { return shared_ ? shared_->h.input * 1000 + shared_->h.channel : 0; }

std::string TapReader::InputName() const {
    if (!shared_ || !shared_->h.input) return "";
    const uint32_t input = shared_->h.input, channel = shared_->h.channel;
    std::string s = input == kGuitarTapMicInput ? "[Asio.Input.Mic]" : "[Asio.Input." + std::to_string(input - 1) + "]";
    if (channel) s += ", channel " + std::to_string(channel - 1);
    return s;
}

void TapReader::ReadNew(std::vector<float>& out) {
    if (!shared_) return;
    // Atomic 64-bit read on 32-bit x86 (a compare-exchange that never changes the value).
    const int64_t writePos = InterlockedCompareExchange64((volatile LONG64*)&shared_->h.writePos, 0, 0);
    if (readPos_ < 0 || writePos < readPos_ || writePos - readPos_ > kGuitarTapCapacity / 2)
        readPos_ = writePos;  // first read, restart of the stream, or we fell behind: start fresh
    const uint32_t mask = kGuitarTapCapacity - 1;
    for (int64_t i = readPos_; i < writePos; ++i) out.push_back(shared_->samples[(uint32_t)i & mask]);
    readPos_ = writePos;
}

}  // namespace nbn
