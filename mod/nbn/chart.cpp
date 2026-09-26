// chart.cpp: see chart.h.
#include "chart.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace nbn {

bool Chart::Load(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) return false;
    targets.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        s.imbue(std::locale::classic());  // "18.000" always uses a dot
        std::string tag;
        s >> tag;
        if (tag == "song") s >> songKey;
        else if (tag == "title") { std::getline(s, title); if (!title.empty() && title[0] == ' ') title.erase(0, 1); }
        else if (tag == "arrangement") s >> arrangement;
        else if (tag == "bass") { int b = 0; s >> b; bass = b != 0; }
        else if (tag == "N") {
            Target t;
            int m;
            s >> t.time >> m >> t.string >> t.fret;
            t.midi.push_back(m);
            if (s) targets.push_back(t);
        } else if (tag == "C") {
            Target t;
            int n = 0;
            s >> t.time >> n;
            t.chord = true;
            for (int i = 0; i < n; ++i) { int m; if (s >> m) t.midi.push_back(m); }
            if (s) targets.push_back(t);
        }
    }
    std::stable_sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.time < b.time; });
    return !targets.empty();
}

size_t Chart::FirstAtOrAfter(double t) const {
    return std::lower_bound(targets.begin(), targets.end(), t, [](const Target& x, double v) { return x.time < v; }) -
           targets.begin();
}

}  // namespace nbn
