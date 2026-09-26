// tap.h: reads the guitar signal that our RS_ASIO build publishes in shared memory
// (layout: mod/common/GuitarTapShared.h).
#pragma once
#include <cstdint>
#include <vector>

namespace nbn {

struct GuitarTapShared;

class TapReader {
public:
    // Opens the shared memory if RS_ASIO has created it (it does so when the guitar input starts).
    bool Open();
    bool IsOpen() const { return shared_ != nullptr; }
    unsigned SampleRate() const;
    // Appends all samples written since the last call to `out`. If the reader fell more than half
    // the ring behind, it skips ahead (old audio is useless for detection anyway).
    void ReadNew(std::vector<float>& out);

private:
    GuitarTapShared* shared_ = nullptr;
    int64_t readPos_ = -1;
};

}  // namespace nbn
