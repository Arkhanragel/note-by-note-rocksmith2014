// hint.h: "what went wrong" advice while the song waits. When the player plays something other than
// the note or chord the song is waiting for, the banner shows how to fix it in guitar terms:
//
//   single note:  "You played A - move DOWN 2 frets, to fret 5 on string 4 (D)"
//                 "You played C - that's string 3 (G), use fret 5 on string 4 (D)"
//                 "You played G - right note, but an octave too high: play fret 5 on string 4 (D)"
//   chord:        "string 4 (D) is 1 fret too high: move DOWN to fret 2"
//                 "not sounding: string 5 (A) at fret 2 - press firmly and strum every string"
//
// Strings are named by number and letter ("string 3 (D)"); the banner draws that in the string's
// highway colour. The number is the standard one (1 = the thinnest) unless Neck::fromThick.
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

// Where a wrong note was probably played, for the banner's fretboard picture (a red X there). A
// guess, like the advice: the pitch is known, the string isn't.
struct Mark {
    int string = -1;  // 0 = thickest
    int fret = -1;    // chart fret (0 = open, or the capo when there is one)
    int midi = -1;    // what was heard (for its name)
    bool likely = true;  // the advice's guess; false = another spot with the same pitch (drawn fainter)
};

// The instrument, as the chart says.
struct Neck {
    int strings = 6;                        // 4 for bass
    int open[6] = {40, 45, 50, 55, 59, 64}; // MIDI of each open string with the song's tuning
    int capo = 0;                           // capo fret (0 = none): chart fret 0 then sounds here
    bool bassUnsure = false;                // chart might be bass (sounds an octave lower): allow both
    bool fromThick = false;                 // string numbers: false = the standard numbering of guitar
                                            // books, 1 = the thinnest (high e); true = 1 is the thickest
};

// Single note: the song waits for fret `fret` on `string` (MIDI `want`) and the player played `heard`.
// If `where` is given, it gets the spot the wrong note was most likely played at (string -1 = none).
Line ForNote(const Neck& neck, int string, int fret, int want, int heard, Mark* where = nullptr);

// What the chord detector heard when a chord went wrong.
struct ChordHeard {
    std::vector<int> midi;      // the notes heard (MIDI), strongest first
    std::vector<int> extraPcs;  // the wrong pitch classes among them (0 = C)
    int hits = 0;               // how many of the chord's notes were heard,
    int needed = 0;             // of how many it takes to count as played
};

// Chord: frets/notes per string (-1 = not played), and what the chord detector heard. If `where` is
// given, it gets the spots of the wrong frets the advice names.
Line ForChord(const Neck& neck, const int frets[6], const int notes[6], const ChordHeard& detected,
              std::vector<Mark>* where = nullptr);

// Single note, when the sound told WHERE the wrong note was played (stringid.h): on `playedString`
// (its fret follows from the pitch). Same wording as ForNote, but no guessing:
//   same string:      "You played A  -  move DOWN 2 frets, to fret 5 on string 4 (D)"
//   another string:   "You played C  -  that's fret 5 on string 3 (G), use fret 5 on string 4 (D)"
//   octave:           "You played G  -  right note, but an octave too high: play fret 5 on string 4 (D)"
// `where` gets that spot (likely = true). Empty line = right note, or the spot doesn't exist.
Line ForNotePlayedOn(const Neck& neck, int string, int fret, int want, int heard, int playedString, Mark* where = nullptr);

// The other spots of the neck where the note at `at` can be played (same pitch, other strings), as
// likely = false marks: the player may have played any of them.
std::vector<Mark> SameNoteElsewhere(const Neck& neck, const Mark& at);

// Plain text of a line (for the log).
std::string Text(const Line& line);

}  // namespace nbn::hint
