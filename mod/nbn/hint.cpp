// hint.cpp: see hint.h.
#include "hint.h"

#include <algorithm>
#include <cstdlib>

#include "music.h"

namespace nbn::hint {
namespace {

const char* kColorName[6] = {"RED", "YELLOW", "BLUE", "ORANGE", "GREEN", "PURPLE"};  // as on the highway
constexpr int kMaxFret = 24;

int Pc(int midi) { return ((midi % 12) + 12) % 12; }

std::string Frets(int n) { return std::to_string(n) + (n == 1 ? " fret" : " frets"); }

Seg StringSeg(int s) { return {std::string(kColorName[s]) + " string", s}; }

// Chart fret -> the fret that actually sounds (with a capo, "open" sounds at the capo) and back.
int Sounding(const Neck& n, int fret) { return (fret == 0 && n.capo > 0) ? n.capo : fret; }
int ChartFret(const Neck& n, int sounding) { return (n.capo > 0 && sounding == n.capo) ? 0 : sounding; }
bool Playable(const Neck& n, int sounding) { return sounding >= n.capo && sounding <= kMaxFret; }

// "fret 5 on the BLUE string" / "the BLUE string open"
void AddWhere(Line* l, int s, int fret) {
    if (fret == 0) {
        l->push_back({"the ", kWhite});
        l->push_back(StringSeg(s));
        l->push_back({" open", kWhite});
    } else {
        l->push_back({"fret " + std::to_string(fret) + " on the ", kWhite});
        l->push_back(StringSeg(s));
    }
}

}  // namespace

Line ForNote(const Neck& neck, int string, int fret, int want, int heard) {
    if (string < 0 || string >= neck.strings) return {};
    int d = heard - want;
    if (neck.bassUnsure && d <= -7) d += 12;  // maybe a bass chart: it sounds an octave lower
    if (d == 0) return {};
    const int got = want + d;
    const int ef = Sounding(neck, fret);
    Line l = {{"You played " + music::NoteName(heard) + "  -  ", kGrey}};

    if (d % 12 == 0) {  // the right note in another place of the neck
        l.push_back({std::string("right note, but an octave too ") + (d > 0 ? "high" : "low") + ": play ", kWhite});
        AddWhere(&l, string, fret);
        return l;
    }

    // Where else could that note have been played? The same pitch on another string, near the
    // fret asked for (the classic slip: right fret, wrong string).
    int other = -1, otherDiff = 99;
    for (int s = 0; s < neck.strings; ++s) {
        if (s == string) continue;
        const int sf = got - neck.open[s];
        if (!Playable(neck, sf)) continue;
        const int diff = std::abs(sf - ef);
        if (diff < otherDiff || (diff == otherDiff && std::abs(s - string) < std::abs(other - string))) {
            other = s;
            otherDiff = diff;
        }
    }
    auto wrongString = [&]() {
        l.push_back({"that's the ", kWhite});
        l.push_back(StringSeg(other));
        l.push_back({", use ", kWhite});
        AddWhere(&l, string, fret);
        return l;
    };
    // Or the right string, a few frets off.
    const int nf = ef + d;
    auto moveFrets = [&]() {
        if (fret == 0) {  // wanted open: no finger at all
            l.push_back({"don't press any fret: play ", kWhite});
            AddWhere(&l, string, 0);
            return l;
        }
        l.push_back({std::string("move ") + (d > 0 ? "DOWN " : "UP ") + Frets(std::abs(d)) + ", to fret " +
                         std::to_string(fret) + " on the ", kWhite});
        l.push_back(StringSeg(string));
        return l;
    };
    const bool sameOk = Playable(neck, nf);

    if (other >= 0 && otherDiff == 0) return wrongString();
    if (sameOk && std::abs(d) <= 4) return moveFrets();
    if (other >= 0 && otherDiff <= 2) return wrongString();
    if (sameOk && std::abs(d) <= 7) return moveFrets();
    l.push_back({std::string("too ") + (d > 0 ? "high" : "low") + ": play ", kWhite});
    AddWhere(&l, string, fret);
    return l;
}

Line ForChord(const Neck& neck, const int frets[6], const int notes[6], const std::vector<int>& heard,
              const std::vector<int>& extraPcs, int hits, int needed) {
    bool got[12] = {};
    for (int m : heard) got[Pc(m)] = true;
    bool played[6] = {}, missing[6] = {}, muted = false;
    for (int s = 0; s < neck.strings && s < 6; ++s) {
        played[s] = frets[s] >= 0 && notes[s] >= 0;
        missing[s] = played[s] && !got[Pc(notes[s])];
        muted = muted || frets[s] < 0;
    }

    std::vector<Line> advice;
    bool advised[6] = {};
    std::vector<int> unexplained;
    // A wrong note 1-2 frets from one of the chord's strings: that finger is on the wrong fret
    // (preferring strings whose note isn't heard at all).
    int lowest = 999;
    for (int s = 0; s < 6; ++s) if (played[s]) lowest = std::min(lowest, notes[s]);
    for (int e : extraPcs) {
        // Only heard below the chord's lowest note: a string that shouldn't be strummed at all.
        bool below = false;
        for (int m : heard) if (Pc(m) == e) { below = m < lowest - 2; break; }
        if (below) { unexplained.push_back(e); continue; }
        int best = -1, bestD = 0, bestScore = 99;
        for (int s = 0; s < neck.strings && s < 6; ++s) {
            if (!played[s] || advised[s]) continue;
            const int dd = ((e - Pc(notes[s])) % 12 + 18) % 12 - 6;  // -6..5 semitones, heard - wanted
            if (dd == 0 || std::abs(dd) > 2) continue;
            const int score = std::abs(dd) + (missing[s] ? 0 : 10);
            if (score < bestScore) { best = s; bestD = dd; bestScore = score; }
        }
        if (best < 0 || !Playable(neck, Sounding(neck, frets[best]) + bestD)) { unexplained.push_back(e); continue; }
        advised[best] = true;
        missing[best] = false;
        Line l = {StringSeg(best)};
        const std::string off = " is " + Frets(std::abs(bestD)) + " too " + (bestD > 0 ? "high" : "low");
        if (frets[best] == 0) l.push_back({off + ": play it open", kWhite});
        else l.push_back({off + ": move " + (bestD > 0 ? "DOWN" : "UP") + " to fret " + std::to_string(frets[best]), kWhite});
        advice.push_back(l);
    }
    if (!unexplained.empty()) {  // a string that isn't part of the chord is ringing
        std::string names;
        for (size_t i = 0; i < unexplained.size(); ++i)
            names += (i ? (i + 1 == unexplained.size() ? " and " : ", ") : "") + music::NoteName(unexplained[i]);
        advice.push_back({{"a note that isn't in the chord is ringing (" + names + ")" +
                               (muted ? ": don't strum the x strings" : ""), kWhite}});
    }
    if (hits < needed) {  // strings that don't sound: not pressed firmly, or not strummed
        Line l = {{"not sounding: ", kWhite}};
        int n = 0;
        for (int s = 0; s < neck.strings && s < 6; ++s) {
            if (!missing[s] || advised[s]) continue;
            if (n++) l.push_back({", ", kWhite});
            l.push_back(StringSeg(s));
            l.push_back({frets[s] == 0 ? " (open)" : " (fret " + std::to_string(frets[s]) + ")", kWhite});
        }
        l.push_back({" - press firmly and strum every string", kWhite});
        if (n) advice.push_back(l);
    }

    Line out;
    for (size_t i = 0; i < advice.size() && i < 2; ++i) {  // at most two pieces of advice at once
        if (i) out.push_back({"   \xC2\xB7   ", kGrey});
        out.insert(out.end(), advice[i].begin(), advice[i].end());
    }
    if (!out.empty()) out.insert(out.begin(), {"Fix:  ", kGrey});
    return out;
}

std::string Text(const Line& line) {
    std::string s;
    for (const auto& seg : line) s += seg.text;
    return s;
}

}  // namespace nbn::hint
