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
    int maxK = 0;         // the highest one found (B needs one at 6 or above)
    double levelDb = -120;  // RMS level of the analysed sound (dBFS)
    bool unstretched = false;  // overtones high enough, but not sharp at all (B <= 0): no single string
                               // sounds like that (a harmonic, several strings ringing, a pitch that moves)
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

constexpr int kHandMargin = 3;  // hand tie-break: the nearer spot must be this much nearer (frets; a string = 3)
constexpr int kWeakTopK = 8;    // a measure whose highest overtone is below this is "weak" (noisier B)...
constexpr int kWeakReach = 6;   // ...and only trusted this near the hand (in game: 4 weak answers 1 fret from
                                // the hand looked right, one at 17 = string 5 fret 20 with the hand at G 9, wrong)

struct Guess {
    int string = -1;      // -1 = no answer
    int fret = -1;        // sounding fret from the nut
    double dist = 0;      // |measured - predicted| log10 B of the chosen spot
    double margin = 0;    // how much further the next best spot is (by sound)
    bool sure = false;    // dist <= kMaxDist, and the sound (margin >= kMinMargin) or the hand picked it
    bool byHand = false;  // the sound fitted several spots; the one near the hand was taken
};

// candidates: (string, sounding fret from the nut) where the heard pitch can be played; open: the
// song's open strings (MIDI, 0 = thickest). handString / handFret (sounding): where the fretting
// hand is, i.e. the note the song waits for (-1 = unknown).
//
// Some spots predict almost the same B (e.g. string 3 (G) fret 10 and string 4 (D) fret 15). When
// the sound fits several, the hand decides: a slip is a fret or a string away, not six frets
// (in game, 6 of 9 "not sure" notes were such pairs, all with the hand next to one of them). An
// open string is "near" any fret (brushing the next string is a common slip), so it only costs its
// string distance; when that leaves two spots about as near, there's no answer.
// weak: the measure only had low overtones (Measure::maxK < kWeakTopK); then the answer must be
// within kWeakReach of the hand (no hand = no answer).
Guess Identify(const Calibration& c, const int open[6], double logB, const std::vector<std::pair<int, int>>& candidates,
               int handString = -1, int handFret = -1, bool weak = false);

}  // namespace nbn::stringid
