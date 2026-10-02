// picking.cpp: see picking.h.
#include "picking.h"

#include <algorithm>
#include <cmath>

namespace nbn::picking {
namespace {

constexpr double kOnGrid = 0.15;  // of a 16th: a note this close to a 16th counts as on it

// Where song time t is in the beat grid: the beat it's in (index) and how far into it (0..1). False
// before the first beat or without a grid.
bool BeatPos(const std::vector<Beat>& beats, double t, size_t* beat, double* frac) {
    if (beats.size() < 2 || t < beats.front().time - 0.01) return false;
    // The last beat at or before t (a note a hair before its beat belongs to it).
    auto it = std::upper_bound(beats.begin(), beats.end(), t + 0.01, [](double x, const Beat& b) { return x < b.time; });
    const size_t i = (size_t)(it - beats.begin()) - 1;
    const double len = i + 1 < beats.size() ? beats[i + 1].time - beats[i].time : beats[i].time - beats[i - 1].time;
    if (len <= 0) return false;
    *beat = i;
    *frac = std::max(0.0, (t - beats[i].time) / len);
    return true;
}

// The bar a beat is in: the index of the last downbeat at or before it.
size_t BarOf(const std::vector<Beat>& beats, size_t beat) {
    while (beat > 0 && !beats[beat].downbeat) --beat;
    return beat;
}

}  // namespace

bool Picked(const Target& t) {
    using namespace technique;
    return !(t.tech.mask & (kHammerOn | kPullOff | kTap | kChild));
}

void Suggest(const std::vector<Target*>& notes, const std::vector<Beat>& beats) {
    // Each note's place: its bar, and its position in 16ths from the beat (q, 0..4).
    struct Place {
        bool known = false;
        size_t bar = 0;
        double q = 0;
        double beatLen = 0.5;
    };
    std::vector<Place> place(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
        size_t b;
        double frac;
        if (!BeatPos(beats, notes[i]->time, &b, &frac)) continue;
        const double len = b + 1 < beats.size() ? beats[b + 1].time - beats[b].time : beats[b].time - beats[b - 1].time;
        place[i] = {true, BarOf(beats, b), frac * 4.0, len};
    }
    // How fast each bar's hand moves: 16ths if a note sits on an odd 16th ("e" or "a"), else 8ths if one
    // sits on the "and", else quarters (every note down).
    std::vector<std::pair<size_t, int>> grid;  // (bar, 1 / 2 / 4)
    auto gridOf = [&](size_t bar) -> int& {
        for (auto& g : grid)
            if (g.first == bar) return g.second;
        grid.push_back({bar, 1});
        return grid.back().second;
    };
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!place[i].known) continue;
        const double q = place[i].q, r = std::round(q);
        if (std::abs(q - r) > kOnGrid) continue;
        int& g = gridOf(place[i].bar);
        const int slot = (int)r % 4;
        if (slot == 1 || slot == 3) g = 4;
        else if (slot == 2) g = std::max(g, 2);
    }
    // The strokes.
    int prev = -1;
    double prevT = -1e9, prevLen = 0.5;
    for (size_t i = 0; i < notes.size(); ++i) {
        Target& t = *notes[i];
        t.pick = -1;
        if (!Picked(t)) continue;
        const Place& p = place[i];
        int dir = 0;
        const double r = std::round(p.q);
        if (p.known && std::abs(p.q - r) <= kOnGrid) {
            const int slot = (int)r % 4, g = gridOf(p.bar);
            dir = g == 4 ? slot % 2 : (g == 2 && slot == 2) ? 1 : 0;
        } else if (prev >= 0 && t.time - prevT < (p.known ? p.beatLen : prevLen)) {
            dir = 1 - prev;  // off the grid (triplets...): the other way from the note before
        }
        t.pick = dir;
        prev = dir;
        prevT = t.time;
        if (p.known) prevLen = p.beatLen;
    }
}

Result Assign(Chart* chart) {
    Result res;
    std::vector<Target*> level;
    for (auto& pis : chart->byLevelPi) {
        level.clear();
        for (auto& notes : pis)
            for (auto& t : notes) level.push_back(&t);
        std::stable_sort(level.begin(), level.end(), [](const Target* a, const Target* b) { return a->time < b->time; });
        Suggest(level, chart->beats);
        for (const Target* t : level) {
            if (t->pick < 0 || t->songPick < 0) continue;
            ++res.picked;
            res.songUps += t->songPick == 1;
            res.agree += t->songPick == t->pick;
        }
    }
    // The song's own directions make sense: some "up" (all "down" is just the default), at least 1 in 20
    // picked notes, and at least half agree with the rhythm.
    res.fromSong = res.songUps > 0 && res.songUps * 20 >= res.picked && res.agree * 2 >= res.picked;
    chart->picksFromSong = res.fromSong;
    if (res.fromSong)
        for (auto& pis : chart->byLevelPi)
            for (auto& notes : pis)
                for (auto& t : notes)
                    if (t.pick >= 0 && t.songPick >= 0) t.pick = t.songPick;
    return res;
}

}  // namespace nbn::picking
