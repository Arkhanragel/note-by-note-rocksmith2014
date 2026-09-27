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
};

struct PhraseIteration {
    int phraseId = 0;
    double start = 0, end = 0;
};

struct Chart {
    std::string songKey, title, arrangement;
    bool bass = false;
    bool bassUnsure = false;  // no chord says guitar or bass, and only 4 strings are used: accept both
    std::vector<PhraseIteration> pis;
    std::vector<int> levelCounts;                      // notes per level (to identify the arrangement)
    std::vector<std::vector<std::vector<Target>>> byLevelPi;  // [level][pi] -> notes sorted by time

    bool Load(const std::wstring& path);
    // Fills byLevelPi from a flat list of notes (levelCounts and pis must be set first).
    void Index(const std::vector<Target>& all);
    int Levels() const { return (int)levelCounts.size(); }

    // First target with time > after, using levels[pi] as the current level of each PI (a level
    // outside the chart's range falls back to the nearest valid one). Returns nullptr at the end.
    const Target* NextTarget(double after, const std::vector<int>& levels) const;
};

}  // namespace nbn
