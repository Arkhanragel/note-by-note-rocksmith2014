// silence.h: a wait during which no sound at all arrives from the guitar.
//
// That is not a wrong note: the guitar isn't reaching the mod (unplugged, its volume down, a flat
// wireless battery, the wrong input in RS_ASIO.ini). Without a word the player sits in front of a song
// that just doesn't move (issue #2), so after a few seconds the banner says "I can't hear your guitar",
// and takes it back as soon as anything is heard.
//
// A pure rule, no game state (tested by nbn_tap_test).
#pragma once

namespace nbn {

struct SilenceWatch {
    // Below this an input only has its own noise (an open input sits around -85 dB; a quiet pluck is far
    // above). -50 dB: a little under the note detector's gate (-45 dB), so "silent" never overlaps a
    // sound the detector could have answered.
    static constexpr float kSilentPeak = 0.003f;
    // How long a wait must be silent before the message. Long enough for a player who is finding the note.
    static constexpr unsigned kSilentMs = 6000;

    enum class Event { kNone, kSilent, kHeardAgain };

    bool told = false;  // the message is up for this wait

    // A new wait starts.
    void Reset() { told = false; }

    // Call while waiting. `waitPeak` = the loudest sample (0..1) since the wait began, `waitedMs` = how
    // long it has lasted. kSilent: show the message (once per silence). kHeardAgain: take it away.
    Event Update(float waitPeak, unsigned waitedMs) {
        if (waitPeak >= kSilentPeak) {
            if (!told) return Event::kNone;
            told = false;
            return Event::kHeardAgain;
        }
        if (told || waitedMs < kSilentMs) return Event::kNone;
        told = true;
        return Event::kSilent;
    }
};

}  // namespace nbn
