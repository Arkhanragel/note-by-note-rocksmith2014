// chart.cpp: see chart.h.
#include "chart.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace nbn {

bool Chart::Load(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) return false;
    pis.clear();
    levelCounts.clear();
    byLevelPi.clear();
    std::vector<Target> all;
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
        else if (tag == "count") { int lv = 0, n = 0; s >> lv >> n; if (lv >= 0) { if ((int)levelCounts.size() <= lv) levelCounts.resize(lv + 1); levelCounts[lv] = n; } }
        else if (tag == "pi") {
            int idx; PhraseIteration p;
            s >> idx >> p.phraseId >> p.start >> p.end;
            if (s && idx >= 0) { if ((int)pis.size() <= idx) pis.resize(idx + 1); pis[idx] = p; }
        } else if (tag == "N") {
            Target t; int m, ign;
            s >> t.level >> t.pi >> t.time >> m >> t.string >> t.fret >> ign;
            t.midi.push_back(m);
            t.ignore = ign != 0;
            if (s) all.push_back(t);
        } else if (tag == "C") {
            Target t; int ign = 0, n = 0;
            s >> t.level >> t.pi >> t.time >> ign >> n;
            t.chord = true;
            t.ignore = ign != 0;
            for (int i = 0; i < n; ++i) { int m; if (s >> m) t.midi.push_back(m); }
            if (s) all.push_back(t);
        }
    }
    if (levelCounts.empty() || pis.empty()) return false;  // not a v2 chart (re-run the exporter)
    byLevelPi.assign(levelCounts.size(), std::vector<std::vector<Target>>(pis.size()));
    for (const auto& t : all)
        if (t.level >= 0 && t.level < (int)levelCounts.size() && t.pi >= 0 && t.pi < (int)pis.size())
            byLevelPi[t.level][t.pi].push_back(t);
    for (auto& lv : byLevelPi)
        for (auto& v : lv)
            std::stable_sort(v.begin(), v.end(), [](const Target& a, const Target& b) { return a.time < b.time; });
    return true;
}

const Target* Chart::NextTarget(double after, const std::vector<int>& levels) const {
    for (size_t pi = 0; pi < pis.size(); ++pi) {
        if (pis[pi].end <= after) continue;  // this phrase iteration is already over
        int lv = pi < levels.size() ? levels[pi] : 0;
        lv = std::max(0, std::min(lv, (int)byLevelPi.size() - 1));
        const auto& notes = byLevelPi[lv][pi];
        auto it = std::upper_bound(notes.begin(), notes.end(), after,
                                   [](double v, const Target& t) { return v < t.time; });
        if (it != notes.end()) return &*it;
    }
    return nullptr;
}

}  // namespace nbn
