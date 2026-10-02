// music.h: friendly music words for the overlay. Turns MIDI notes and chord names into plain text a
// non-musician can follow: note names without octave numbers ("F#"), and what a chord symbol means
// ("B5" -> "B power chord", "Em" -> "E minor"). Pure functions, no game state (tested by
// nbn_music_test).
#pragma once
#include <string>
#include <vector>

namespace nbn::music {

// "C", "F#"... (or "Gb" with flats = true). No octave number.
std::string NoteName(int midi, bool flats = false);

// True if the chord symbol's root is written with a flat ("Bb", "Eb5"): its notes are then shown
// with flats too, so they match the name.
bool UsesFlats(const std::string& chordName);

// What a chord is, in words, from the name used in the song (may be empty) and its notes (MIDI,
// lowest string first). Examples:
//   "B5",  {B, F#}         -> "B power chord"
//   "Em",  {E, B, E, G...} -> "E minor"
//   "G/B", {...}           -> "G major, with B as the lowest note"
//   "",    {C#, A}         -> "part of A major"
// Unknown names fall back to recognising the notes; "" if nothing fits (just show the notes).
std::string ChordMeaning(const std::string& name, const std::vector<int>& notes);

// "B and F#", "E, G and B": the chord's different notes, lowest first.
std::string NoteList(const std::vector<int>& notes, bool flats);

// ---- Sections of the song ("Intro", "Verse 2", "Chorus 1"...)
// A section as the game keeps it in memory: the song file's name ("verse", "postchorus"...) and number,
// split at every phrase. The pieces after the first get a running number of their own (verse #1, then
// verse #13 for the rest of that verse), so the real numbers are the ones that come in order (1, 2, 3).
struct SectionPiece {
    std::string name;
    int number = 0;
    double start = 0, end = 0;
};

struct Section {
    std::string name;   // in words, numbered when the song has more than one: "Verse 2", "Solo", "No guitar"
    double start = 0, end = 0;
};

// The sections of the song in words: the pieces of one section joined again, each named and numbered.
std::vector<Section> NameSections(const std::vector<SectionPiece>& pieces);

// One section name in words: "postchorus" -> "Post-chorus", "noguitar" -> "No guitar".
std::string SectionWord(const std::string& name);

}  // namespace nbn::music
