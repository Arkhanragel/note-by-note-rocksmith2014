// stringid.h: which string was a note played on? The pitch alone can't say (an E4 exists on four
// strings), but the sound can. A real string is a little stiff, so its overtones are slightly sharp:
//
//     f_k = k * f0 * sqrt(1 + B k^2)
//
// B (the "inharmonicity") grows with the string's thickness and gets 4x bigger for every octave up
// the neck (B ~ 1/L^2, and fret 12 halves the length L). So the same pitch on a thicker string,
// higher up, has a bigger B. After a short calibration (each open string plucked a few times) the
// B of every spot on the neck can be predicted, and a played note matched to the spot that fits.
//
// Port of tools/detector/string_id.py (partials, fit_inharmonic; the "inharm" physics model). Tested
// there on real recordings: ~97 % right on the ~92 % of notes it is sure about, with 0.2 s of
// sound, on a CLEAN signal. A compressed / overdriven / preset signal makes the overtones exactly
// harmonic, so the calibration refuses it (see CheckCalibration). Guitar only.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace nbn::stringid {

constexpr int kSr = 48000;
constexpr double kStartAfter = 0.03;  // the analysis starts this long after the pick attack (seconds)
constexpr double kLength = 0.2;       // and takes this much sound (0.15-0.3 s tested: all ~97-98 % right)
constexpr double kMinLength = 0.15;   // less than this (the next note came too soon): no answer
constexpr double kMaxDist = 0.15;     // sure only if the best spot's log10 B is this close to the measure
constexpr double kMinMargin = 0.05;   // and the second best is this much further away

// One note's measure.
struct Measure {
    bool ok = false;      // false = not enough overtones to measure B
    double f0 = 0;        // fitted fundamental (Hz)
    double logB = 0;      // log10 of B
    int partials = 0;     // overtones found
};

// x: n samples of one note (from kStartAfter after its attack); f0Guess: the tracker's frequency.
Measure Analyze(const double* x, int n, double f0Guess, int sr = kSr);

// The open strings' B, measured once per guitar (and again after new strings).
struct Calibration {
    bool has[6] = {};
    double logB[6] = {};
    int midi[6] = {};     // the open string's pitch when it was measured (B scales with the tuning)

    bool Complete() const;
    std::string ToString() const;                       // "40:-3.780 45:-4.010 ..." (ini value)
    static Calibration FromString(const std::string& s);
};

// "" if the calibration looks like a clean guitar signal, or why not (for the menu): the wound
// strings must be clearly stiffer than a processed signal makes them look.
std::string CheckCalibration(const Calibration& c);

// The open string's log10 B for the song's tuning: B ~ 1/T and the tension T ~ f^2, so a string
// tuned down a semitone has a little more B than when it was calibrated.
double OpenLogB(const Calibration& c, int string, int openMidi);

struct Guess {
    int string = -1;      // -1 = no answer
    int fret = -1;        // sounding fret from the nut
    double dist = 0;      // |measured - predicted| log10 B of the best spot
    double margin = 0;    // how much further the second best spot is
    bool sure = false;    // dist <= kMaxDist && margin >= kMinMargin
};

// candidates: (string, sounding fret from the nut) where the heard pitch can be played; open: the
// song's open strings (MIDI, 0 = thickest).
Guess Identify(const Calibration& c, const int open[6], double logB, const std::vector<std::pair<int, int>>& candidates);

}  // namespace nbn::stringid
