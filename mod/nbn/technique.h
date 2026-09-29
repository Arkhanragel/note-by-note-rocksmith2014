// technique.h: HOW a note is played (slide, bend, hammer-on, palm mute...), as the game's chart says,
// and the plain words the banner uses for it ("Slide: then slide UP to fret 9, keep the string pressed").
//
// Where it comes from: every note of the song data has the SNG note mask (the same bits as the song
// file, see Rocksmith2014.NET's NoteMask) plus a few fields for the technique's details. In game
// memory (Dec 2024 build) those are at: +0x0 mask, +0x34 slide-to fret (int8, -1 = none), +0x35
// unpitched-slide-to fret, +0x40 the largest bend (float, in steps: 0.5 = a half step, 1 = a whole
// step), found 2026-09-29 by dumping the slide/bend notes of Ode to Joy (tools, BITACORA).
// Pure functions, no game state (tested by nbn_music_test).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nbn::technique {

// SNG note mask bits (Rocksmith2014.NET NoteMask).
enum : uint32_t {
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
};

struct Technique {
    uint32_t mask = 0;         // the note's SNG mask (only the bits above matter here)
    int slideTo = -1;          // slide: the fret it slides to, -1 = none
    int slideUnpitchTo = -1;   // unpitched slide: the fret it slides toward (the pitch doesn't matter)
    float bend = 0;            // the largest bend, in steps (0.5 = half a step = like 1 fret higher)
    bool Any() const;          // anything to tell the player
};

// One technique in words: its name ("Slide") and how to do it.
struct Words {
    std::string name, how;
};

// What to tell the player for a note at `fret`, most important first (at most `max`).
std::vector<Words> Describe(const Technique& t, int fret, size_t max = 2);

// A bend in words: 0.5 -> "half a step", 1 -> "1 step", 1.5 -> "1 and a half steps".
std::string BendSteps(float steps);

// Short tab-style label for the picture: "1/2", "1", "1 1/2" (steps of a bend).
std::string BendLabel(float steps);

}  // namespace nbn::technique
