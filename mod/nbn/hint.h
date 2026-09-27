// hint.h: "what went wrong" advice while the song waits. When the player plays something other than
// the note or chord the song is waiting for, the banner shows how to fix it in guitar terms:
//
//   single note:  "You played G - move UP 2 frets (to fret 7)"
//                 "You played C - that's the ORANGE string: use the BLUE string, fret 5"
//                 "You played E - right note, one octave too high: fret 2 on the BLUE string"
//   chord:        "GREEN string: move DOWN 1 fret (to fret 3)"
//                 "Not sounding: BLUE string (fret 2) - press it firmly and strum it"
//
// A guitar note doesn't say which string it was played on (the same pitch exists on several
// strings), so the advice guesses the most likely slip: the same fret on a neighbouring string, or a
// few frets off on the right string. Pure functions, no game state (tested by nbn_hint_test).
#pragma once
#include <string>
#include <vector>

namespace nbn::hint {

// Colour of a piece of text: 0..5 = that string's highway colour (0 = thickest), or one of these.
enum Color : int { kWhite = -1, kGrey = -2 };

struct Seg {
    std::string text;
    int color = kWhite;
};
using Line = std::vector<Seg>;  // empty = no advice

// The instrument, as the chart says.
struct Neck {
    int strings = 6;                        // 4 for bass
    int open[6] = {40, 45, 50, 55, 59, 64}; // MIDI of each open string with the song's tuning
    int capo = 0;                           // capo fret (0 = none): chart fret 0 then sounds here
    bool bassUnsure = false;                // chart might be bass (sounds an octave lower): allow both
};

// Single note: the song waits for fret `fret` on `string` (MIDI `want`) and the player played `heard`.
Line ForNote(const Neck& neck, int string, int fret, int want, int heard);

// Chord: frets/notes per string (-1 = not played), what the chord detector heard (MIDI, strongest
// first), the wrong pitch classes it found (0 = C), and how many of the chord's notes it heard vs.
// needed.
Line ForChord(const Neck& neck, const int frets[6], const int notes[6], const std::vector<int>& heard,
              const std::vector<int>& extraPcs, int hits, int needed);

// Plain text of a line (for the log).
std::string Text(const Line& line);

}  // namespace nbn::hint
