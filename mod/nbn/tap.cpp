// tap.cpp: see tap.h.
#include "tap.h"

#include <windows.h>

#include "../common/GuitarTapShared.h"

namespace nbn {

bool TapReader::Open() {
    if (shared_) return true;
    // Mapped read/WRITE even though we only read: the atomic 64-bit read below uses
    // InterlockedCompareExchange64 (lock cmpxchg8b), which needs write access. With a read-only view it
    // raised an access violation that silently killed the mod's thread (first in-game test).
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kGuitarTapName);
    if (!map) return false;  // RS_ASIO hasn't created it yet (or this isn't our RS_ASIO build)
    auto* p = (GuitarTapShared*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GuitarTapShared));
    CloseHandle(map);  // the view keeps the mapping alive
    if (!p) return false;
    if (p->h.magic != kGuitarTapMagic || p->h.capacity != kGuitarTapCapacity) {
        UnmapViewOfFile(p);
        return false;
    }
    shared_ = p;
    readPos_ = -1;
    return true;
}

unsigned TapReader::SampleRate() const { return shared_ ? shared_->h.sampleRate : 0; }

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
