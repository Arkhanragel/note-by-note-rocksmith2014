// detector.h: note detection for the Note-by-Note mod.
// A C++ port of tools/detector/pitch.py + tracker.py, with the same algorithms and parameters
// (tuned on real recordings; see BITACORA.md). Keep both versions in sync.
//
//   Yin()           monophonic pitch (YIN with an octave-error guard)
//   OnsetDetector   "a new pick attack happened" (energy jump over the recent minimum)
//   NoteTracker     turns 256-sample blocks into NoteEvents ("the player just played X")
#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace nbn {

struct PitchResult {
    bool ok = false;
    double freq = 0, midi = 0, aperiodicity = 1;
};

// x: window of W samples. Returns ok=false if the window isn't clearly pitched.
PitchResult Yin(const double* x, int W, int sr, double fmin, double fmax, double threshold);

class OnsetDetector {
public:
    OnsetDetector(int sr, double ratio = 2.0, double gateDb = -45.0, double spanMs = 21.0,
                  double lookbackMs = 60.0, double refractoryMs = 150.0);
    // window: the tracker's analysis window (the last `span` samples are used); nowMs: stream time.
    bool Process(const double* window, int W, double nowMs);

private:
    double ratio_, gate_, lookbackMs_, refractoryMs_;
    int span_;
    std::deque<std::pair<double, double>> hist_;  // (time ms, energy)
    double lastOnsetMs_ = -1e9;
};

struct NoteEvent {
    double time = 0;      // seconds of audio processed so far
    int midi = 0;
    double freq = 0, cents = 0, levelDb = 0, aperiodicity = 0;
    bool attack = false;  // caused by a fresh pick attack (false = legato / pitch change)
};

struct TrackerConfig {
    int sr = 48000;
    int window = 2048;        // 4096 for bass
    double fmin = 70.0;       // 35 for bass
    double fmax = 1400.0;
    double threshold = 0.15;  // YIN
    double gateDb = -45.0;    // absolute gate
    double relGateDb = 18.0;  // ignore frames this far below the peak since the last attack
    int stable = 3;           // blocks needed after an attack (3 x 5.3 ms)
    int stableLegato = 6;     // blocks needed for a change without an attack
    double onsetRatio = 2.0;
};

class NoteTracker {
public:
    explicit NoteTracker(const TrackerConfig& cfg);
    // Feed exactly one block of kBlock samples. Returns true and fills *ev when a note event happens.
    bool Process(const float* block, NoteEvent* ev);
    double Now() const { return (double)samples_ / cfg_.sr; }
    const TrackerConfig& Config() const { return cfg_; }

    static constexpr int kBlock = 256;

private:
    TrackerConfig cfg_;
    std::vector<double> buf_;  // sliding analysis window (oldest first)
    OnsetDetector onset_;
    long long samples_ = 0;
    int cur_ = -1, stable_ = 0, reported_ = -1;  // -1 = none
    bool onsetPending_ = false;
    double peakDb_ = -120;
    int lastEventMidi_ = -1;
    double lastEventTime_ = -1;
};

std::string MidiName(int midi);

}  // namespace nbn
