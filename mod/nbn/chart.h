// chart.h: loads the .nbn charts written by `ChartDump export` (format documented in
// tools/ChartDump/Program.cs, Export()).
#pragma once
#include <string>
#include <vector>

namespace nbn {

struct Target {
    double time = 0;          // seconds, song time
    bool chord = false;
    std::vector<int> midi;    // one pitch for a single note, several for a chord
    int string = -1, fret = -1;  // single notes: 0 = thickest string
};

struct Chart {
    std::string songKey, title, arrangement;
    bool bass = false;
    std::vector<Target> targets;  // sorted by time
    bool Load(const std::wstring& path);
    // Index of the first target at or after time t.
    size_t FirstAtOrAfter(double t) const;
};

}  // namespace nbn
