// chart.h: the notes of the arrangement being played, with all difficulty levels. The mod builds it
// straight from game memory (game::ReadSongChart); Load() reads the same thing from the .nbn text
// files written by `ChartDump export` (format v2, documented in tools/ChartDump/Program.cs), kept
// for tests and tools.
//
// With Dynamic Difficulty, the highway shows for each phrase iteration (PI) the notes of that
// phrase's CURRENT level, which the mod reads from game memory. So the "next note" depends on the
// levels: NextTarget() walks the PIs in time order, using each PI's current level.
#pragma once
#include <string>
#include <vector>

#include "music.h"
#include "technique.h"

namespace nbn {

struct Target {
    double time = 0;          // seconds, song time
    int level = 0, pi = 0;    // difficulty level and phrase iteration it belongs to
    bool chord = false;
    bool ignore = false;      // notes marked "ignore" in the chart are never scored by the game
    std::vector<int> midi;    // one pitch for a single note, several for a chord
    int string = -1, fret = -1;  // single notes: 0 = thickest string
    // Chords (from the song's chord template; unknown in .nbn files): the name shown in the game
    // ("Em", "A5"...; empty for double stops) and the fret on each string (0 = thickest; -1 = not
    // played, 0 = open).
    std::string chordName;
    int frets[6] = {-1, -1, -1, -1, -1, -1};
    int notes[6] = {-1, -1, -1, -1, -1, -1};  // MIDI note per string (chords; -1 = not played)
    double sustain = 0;       // seconds the note is held (0 = short note); only from game memory
    technique::Technique tech;  // how to play it (slide, bend...); chords: the chord's own flags and
                                // all its strings' techniques merged (technique::ForChord); only from
                                // game memory
    int techFret = -1;          // chords: the fret `tech` refers to (a slide's start); notes: = fret
    technique::Technique strings[6];  // chords: each string's technique (the tab draws them)
    // Where the fretting hand is (from game memory): the fret under the index finger and how many
    // frets the hand covers (Rocksmith's "anchor"; 0 = unknown). Fingers: 1 = index .. 4 = little
    // finger, 0 = thumb, -1 = none/unknown; per string (chords: the song's; single notes: one finger
    // per fret from the anchor).
    int anchorFret = 0, anchorWidth = 0;
    int fingers[6] = {-1, -1, -1, -1, -1, -1};
    // Pick stroke (picking.h): what the song file says (0 = down, 1 = up, -1 = none; almost every song
    // leaves 0), and what the tab shows: 0 = down, 1 = up, -1 = not picked (hammer-on, pull-off, tap,
    // linked from the note before).
    int songPick = -1;
    int pick = -1;
};

// One beat of the song's beat grid (the highway's bar lines). From game memory only.
struct Beat {
    double time = 0;          // song time (s)
    int measure = 0;          // bar number (Rocksmith's own numbering)
    bool downbeat = false;    // first beat of a bar
};

struct PhraseIteration {
    int phraseId = 0;
    double start = 0, end = 0;
};

struct Chart {
    std::string songKey, title, arrangement;
    bool bass = false;
    bool bassUnsure = false;  // no chord says guitar or bass, and only 4 strings are used: accept both
    int open[6] = {40, 45, 50, 55, 59, 64};  // MIDI of each open string (song tuning; bass: -12)
    int capo = 0;             // capo fret, 0 = none
    std::vector<PhraseIteration> pis;
    std::vector<Beat> beats;                           // beat grid, in time order (empty = unknown)
    std::vector<music::Section> sections;              // "Intro", "Verse 1"... in time order (empty = unknown)
    bool picksFromSong = false;                        // Target::pick is the song's own (else from the rhythm)
    std::vector<int> levelCounts;                      // notes per level (to identify the arrangement)
    std::vector<std::vector<std::vector<Target>>> byLevelPi;  // [level][pi] -> notes sorted by time

    bool Load(const std::wstring& path);
    // Fills byLevelPi from a flat list of notes (levelCounts and pis must be set first).
    void Index(const std::vector<Target>& all);
    int Levels() const { return (int)levelCounts.size(); }

    // First target with time > after, using levels[pi] as the current level of each PI (a level
    // outside the chart's range falls back to the nearest valid one). Returns nullptr at the end.
    const Target* NextTarget(double after, const std::vector<int>& levels) const;

    // All targets with from <= time < to, in time order, with the same level rules (for the
    // scrolling tab: what the highway shows around the current time).
    void TargetsBetween(double from, double to, const std::vector<int>& levels, std::vector<const Target*>* out) const;

    // All beats with from <= time < to, in time order (for the tab's bar and beat lines).
    void BeatsBetween(double from, double to, std::vector<Beat>* out) const;
};

}  // namespace nbn
