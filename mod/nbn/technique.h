// technique.h: HOW a note is played (slide, bend, hammer-on, palm mute...), as the game's chart says,
// and the plain words the banner uses for it ("Slide: then slide UP to fret 9, keep the string pressed").
//
// Where it comes from: every note of the song data has the SNG note mask (the same bits as the song
// file, see Rocksmith2014.NET's NoteMask) plus a few fields for the technique's details. In game
// memory (Dec 2024 build) those are at: +0x0 mask, +0x34 slide-to fret (int8, -1 = none), +0x35
// unpitched-slide-to fret, +0x40 the largest bend (float, in steps: 0.5 = a half step, 1 = a whole
// step), found 2026-09-29 by dumping the slide/bend notes of a song from memory.
//
// Sequences: a note marked "parent" flows into the next note on the same string ("child") without
// picking again (a vibrato that ends in a slide, a slide into a pull-off...). Sequence() puts the
// techniques of such a chain in the order they happen, as steps.
// Pure functions, no game state (tested by nbn_music_test).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nbn::technique {

// SNG note mask bits (Rocksmith2014.NET NoteMask).
enum : uint32_t {
    kChordMute = 0x8,         // a chord's fret-hand mute (the chord drawn with x's on the highway)
    kTremolo = 0x10,
    kHarmonic = 0x20,
    kPalmMute = 0x40,
    kSlap = 0x80,
    kPluck = 0x100,
    kHammerOn = 0x200,
    kPullOff = 0x400,
    kSlide = 0x800,
    kBend = 0x1000,
    kTap = 0x4000,
    kPinchHarmonic = 0x8000,
    kVibrato = 0x10000,
    kMute = 0x20000,          // fret-hand mute of a single note (the "x" on the highway)
    kUnpitchedSlide = 0x400000,
    kAccent = 0x4000000,
    kParent = 0x8000000,      // linked to the next note on the same string (not picked again)
    kChild = 0x10000000,      // linked from the previous one
};

struct Technique {
    uint32_t mask = 0;         // the note's SNG mask (only the bits above matter here)
    int slideTo = -1;          // slide: the fret it slides to, -1 = none
    int slideUnpitchTo = -1;   // unpitched slide: the fret it slides toward (the pitch doesn't matter)
    float bend = 0;            // the largest bend, in steps (0.5 = half a step = like 1 fret higher)
    bool Any() const;          // anything to tell the player
};

// One note of a linked sequence: its technique and fret.
struct Link {
    Technique tech;
    int fret = 0;
};

// One step in words: its name ("Slide") and how to do it.
struct Words {
    std::string name, how;
};

// The steps to play a note and the notes linked after it (chain[0] = the note itself, then its
// children, same string), in the order they happen; at most `max`.
std::vector<Words> Sequence(const std::vector<Link>& chain, size_t max = 3);

// The same for one note alone.
std::vector<Words> Describe(const Technique& t, int fret, size_t max = 3);

// A chord: its own flags (palm mute, mute, accent) and each string's technique (frets: -1 = not
// played). The strings' techniques are merged (a palm mute on every string is said once); a slide
// is described from the lowest string that slides. `fret` gets that string's fret.
Technique ForChord(uint32_t chordMask, const Technique strings[6], const int frets[6], int* fret);

// A bend in words: 0.5 -> "half a step", 1 -> "1 step", 1.5 -> "1 and a half steps".
std::string BendSteps(float steps);

// Short tab-style label for the picture: "1/2", "1", "1 1/2" (steps of a bend).
std::string BendLabel(float steps);

}  // namespace nbn::technique
