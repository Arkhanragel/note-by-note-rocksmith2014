// nbn_picking_test: checks the pick strokes (picking.h) on made-up bars at 120 bpm (a beat = 0.5 s, 4 beats
// a bar). Prints each case as D / U / - (not picked); exit code 1 on failure.
#include <cstdio>
#include <string>
#include <vector>

#include "picking.h"

using nbn::Beat;
using nbn::Target;

namespace {

std::vector<Beat> Grid(int bars) {
    std::vector<Beat> b;
    for (int i = 0; i < bars * 4 + 1; ++i) b.push_back({i * 0.5, i / 4 + 1, i % 4 == 0});
    return b;
}

// Notes at these beat positions (in beats from the song's start); mask = technique bits per note (0 = picked).
std::vector<Target> Notes(const std::vector<double>& at, const std::vector<uint32_t>& masks = {}) {
    std::vector<Target> v;
    for (size_t i = 0; i < at.size(); ++i) {
        Target t;
        t.time = at[i] * 0.5;
        t.tech.mask = i < masks.size() ? masks[i] : 0;
        v.push_back(t);
    }
    return v;
}

std::string Strokes(std::vector<Target>& notes) {
    std::vector<Target*> p;
    for (auto& t : notes) p.push_back(&t);
    nbn::picking::Suggest(p, Grid(4));
    std::string s;
    for (const auto& t : notes) s += t.pick < 0 ? '-' : t.pick == 0 ? 'D' : 'U';
    return s;
}

}  // namespace

int main() {
    int fails = 0;
    auto expect = [&](const char* what, const std::string& got, const std::string& want) {
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  %-40s %s%s\n", ok ? "ok  " : "FAIL", what, got.c_str(), ok ? "" : ("   (wanted " + want + ")").c_str());
    };
    using namespace nbn::technique;
    {
        auto n = Notes({0, 1, 2, 3});
        expect("quarter notes: all down", Strokes(n), "DDDD");
    }
    {
        auto n = Notes({0, 0.5, 1, 1.5, 2, 2.5});
        expect("8ths: down on the beat, up on the and", Strokes(n), "DUDUDU");
    }
    {
        auto n = Notes({0.5, 1.5, 2.5});
        expect("only the ands: up", Strokes(n), "UUU");
    }
    {
        auto n = Notes({0, 0.25, 0.5, 0.75, 1, 1.5});
        expect("a bar with 16ths: the and is down", Strokes(n), "DUDUDD");
    }
    {
        auto n = Notes({4, 4.5, 5});  // the next bar has 8ths only: back to D U
        auto all = Notes({0, 0.25, 4, 4.5, 5});
        expect("16ths in one bar, 8ths in the next", Strokes(all), "DUDUD");
        (void)n;
    }
    {
        auto n = Notes({0, 1.0 / 3, 2.0 / 3, 1}, {0, 0, 0, 0});
        expect("triplets: alternate", Strokes(n), "DUDD");
    }
    {
        auto n = Notes({0, 0.5, 1, 1.5}, {0, kHammerOn, 0, kPullOff | kChild});
        expect("hammer-on / pull-off: not picked", Strokes(n), "D-D-");
    }

    // The song's own directions: used only when they make sense.
    auto song = [&](const std::vector<int>& picks) {
        nbn::Chart c;
        c.beats = Grid(4);
        c.pis = {{0, 0, 8}};
        c.levelCounts = {(int)picks.size()};
        std::vector<Target> all = Notes({0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5});
        for (size_t i = 0; i < all.size(); ++i) all[i].songPick = picks[i];
        c.Index(all);
        const auto r = nbn::picking::Assign(&c);
        std::string s;
        for (const auto& t : c.byLevelPi[0][0]) s += t.pick == 0 ? 'D' : 'U';
        return std::string(r.fromSong ? "song " : "rhythm ") + s;
    };
    expect("all down (the default): rhythm", song({0, 0, 0, 0, 0, 0, 0, 0}), "rhythm DUDUDUDU");
    expect("down-up as written: the song's", song({0, 1, 0, 1, 0, 1, 1, 1}), "song DUDUDUUU");
    expect("upside down: rhythm", song({1, 0, 1, 0, 1, 0, 1, 0}), "rhythm DUDUDUDU");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
