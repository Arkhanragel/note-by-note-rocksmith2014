// tap.h: reads the guitar signal that our RS_ASIO build publishes in shared memory
// (layout: mod/common/GuitarTapShared.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nbn {

struct GuitarTapShared;

class TapReader {
public:
    // Opens the shared memory if RS_ASIO has created it (it does so when the guitar input starts).
    // Only the tap of THIS game process is accepted: another instance's tap (a second game running, or
    // one that is still closing) carries someone else's audio, or none.
    bool Open();
    bool IsOpen() const { return shared_ != nullptr; }
    unsigned SampleRate() const;
    // Which RS_ASIO input the tap carries, for the log: "[Asio.Input.0], channel 0". "" when the tap
    // doesn't say (an RS_ASIO build older than 0.3.2).
    std::string InputName() const;
    // A number that changes when the tap moves to another input (0 = not known).
    uint32_t InputId() const;
    // Appends all samples written since the last call to `out`. If the reader fell more than half
    // the ring behind, it skips ahead (old audio is useless for detection anyway).
    void ReadNew(std::vector<float>& out);

private:
    GuitarTapShared* shared_ = nullptr;
    int64_t readPos_ = -1;
};

}  // namespace nbn
