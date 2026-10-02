// tuning.h: is the guitar in tune with the song? The mod knows which note the song asked for and hears
// what was played, so it compares them, string by string:
//
//   a little off:     "string 5 (A) sounds a bit low: tune it up a little, to A"
//   a half step off:  "string 6 (E) sounds a whole step high: this song is in Drop D, tune it down to D"
//   all of them:      "Your guitar sounds a half step high: this song is in Eb standard, tune every
//                     string down a half step"
//
// One note says little (the player may have missed the fret, a pick attack starts sharp), so:
// - an amount between whole half steps (1.4 half steps low) can only be the tuning: a few notes that
//   agree are enough, even the same open string again and again;
// - each note's pitch is its STEADY pitch: the median of the pitch tracker's readings from ~50 ms to
//   400 ms after the note started (SteadyPitch);
// - a string is only called out of tune when several of its last notes agree. A whole half step off
//   also needs different notes, and no note on that string that came out right in between (a hand one
//   fret off for a while also gives "a half step off" on every note, but the player then corrects it).
// Pure code, no game state (tested by nbn_tuning_test).
#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "hint.h"

namespace nbn::tuning {

// The tracker's blocks (NoteTracker::kBlock = 256 samples at 48 kHz = 5.3 ms) after a note event
// (which comes ~16 ms after the pick) whose pitch counts: from ~50 ms to ~400 ms into the note.
constexpr int kFirstBlock = 7;
constexpr int kLastBlock = 72;
constexpr int kMinFrames = 8;  // fewer readings (a short note): not measured

// The steady pitch of one note.
class SteadyPitch {
public:
    // A note event (MIDI note).
    void Start(int midi);
    // One block's pitch from the tracker (fractional MIDI, < 0 = none). Returns true once per note,
    // when its steady pitch is known (*pitch, fractional MIDI).
    bool Frame(double pitch, double* out);
    // The note was cut short (another note started): its steady pitch so far, if there's enough of it.
    bool Stop(double* out) { return on_ && Finish(out); }
    bool On() const { return on_; }
    int Midi() const { return midi_; }

private:
    bool Finish(double* out);
    bool on_ = false;
    int midi_ = 0, block_ = 0, gap_ = 0;
    std::vector<double> got_;
};

struct Finding {
    int string = -1;     // the string out of tune (0 = thickest); -1 = none (with all: every string)
    bool all = false;    // the whole guitar is off the same way
    double offset = 0;   // how far off, in half steps (+ = high/sharp): the median of its notes
    int steps = 0;       // whole half steps off (round(offset)); 0 = only a little off
    bool operator==(const Finding&) const = default;
};

class Check {
public:
    static constexpr int kKeep = 8;          // notes kept per string
    static constexpr int kNeed = 4;          // notes that must agree
    // A little off: from 30 cents (0.3 of a half step). Measured on the user's guitar in tune (582 notes
    // of saved waits): the steady pitch of a note is +7 cents on average (fretting pulls it sharp), each
    // note's median up to +15, single notes -38..+35. From 20 cents, 13 of 582 checks said "out of
    // tune"; from 30, none, and a string 40 cents low is still found (after a few windows of notes).
    static constexpr double kLittle = 0.3;
    static constexpr double kNear = 0.8;     // notes this close to the right pitch count for "a little off"
    static constexpr double kAgree = 0.2;    // notes within this of their median agree
    static constexpr double kAgreeShare = 0.6;  // and this share of them must
    // Off by an amount at least this far from a whole number of half steps: only the tuning does that
    // (a wrong fret or string is a whole number of half steps; the user's in-tune notes, per note:
    // within 0.15).
    static constexpr double kBetween = 0.2;

    void Reset();
    // One note played: the string and note (MIDI) the song asked for, and the note's steady pitch.
    void Add(int string, int wanted, double heard);
    Finding Get() const;
    // How many notes of `string` are kept (for the log).
    int Count(int string) const { return string >= 0 && string < 6 ? (int)notes_[string].size() : 0; }

private:
    Finding ForString(int s, bool* inTune) const;
    std::deque<std::pair<int, double>> notes_[6];  // (wanted MIDI, offset in half steps), oldest first
};

// The song's tuning in words: "E standard", "Eb standard", "Drop D", "Drop C#", or the notes of the
// open strings ("D A D G A D"). open = MIDI of each open string (the chart's, bass: -12), strings = 6 or 4.
std::string TuningName(const int open[6], int strings);

// The advice as a line for the wrong-note panel (string names in their colours, like hint.h). open =
// the song's open strings (what to tune to); neck = for the string names. Empty = nothing to say.
hint::Line Advice(const Finding& f, const hint::Neck& neck);

// The same as plain text (the log, a toast).
std::string AdviceText(const Finding& f, const hint::Neck& neck);

}  // namespace nbn::tuning
