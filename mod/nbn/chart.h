// chart.h: loads the .nbn charts written by `ChartDump export` (format v2: all difficulty levels;
// documented in tools/ChartDump/Program.cs, WriteChartV2).
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
};

struct PhraseIteration {
    int phraseId = 0;
    double start = 0, end = 0;
};

struct Chart {
    std::string songKey, title, arrangement;
    bool bass = false;
    std::vector<PhraseIteration> pis;
    std::vector<int> levelCounts;                      // notes per level (to identify the arrangement)
    std::vector<std::vector<std::vector<Target>>> byLevelPi;  // [level][pi] -> notes sorted by time

    bool Load(const std::wstring& path);
    int Levels() const { return (int)levelCounts.size(); }

    // First target with time > after, using levels[pi] as the current level of each PI (a level
    // outside the chart's range falls back to the nearest valid one). Returns nullptr at the end.
    const Target* NextTarget(double after, const std::vector<int>& levels) const;
};

}  // namespace nbn
