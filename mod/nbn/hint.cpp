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

// Chart fret -> the fret that actually sounds (with a capo, "open" sounds at the capo).
int Sounding(const Neck& n, int fret) { return (fret == 0 && n.capo > 0) ? n.capo : fret; }
bool Playable(const Neck& n, int sounding) { return sounding >= n.capo && sounding <= kMaxFret; }
// And back: with a capo, the capo's fret is the chart's "open".
int ChartFret(const Neck& n, int sounding) { return (n.capo > 0 && sounding == n.capo) ? 0 : sounding; }

// The spot of pitch `midi` nearest to fret `ef` on `string` (a string away counts like 3 frets), for
// the picture when the advice itself can't say where the note was played. string -1 = not on the neck.
Mark NearestSpot(const Neck& n, int midi, int string, int ef, int heard) {
    Mark m;
    int best = 999;
    for (int s = 0; s < n.strings && s < 6; ++s) {
        const int sf = midi - n.open[s];
        if (!Playable(n, sf)) continue;
        const int cost = std::abs(sf - ef) + 3 * std::abs(s - string);
        if (cost < best) { best = cost; m = {s, ChartFret(n, sf), heard}; }
    }
    return m;
}

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

Line ForNote(const Neck& neck, int string, int fret, int want, int heard, Mark* where) {
    Mark unused;
    Mark& at = where ? *where : unused;
    at = Mark{};
    if (string < 0 || string >= neck.strings) return {};
    int d = heard - want;
    if (neck.bassUnsure && d <= -7) d += 12;  // maybe a bass chart: it sounds an octave lower
    if (d == 0) return {};
    const int got = want + d;
    const int ef = Sounding(neck, fret);
    Line l = {{"You played " + music::NoteName(heard) + "  -  ", kGrey}};

    if (d % 12 == 0) {  // the right note in another place of the neck
        at = NearestSpot(neck, got, string, ef, heard);
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
        at = {other, ChartFret(neck, got - neck.open[other]), heard};
        l.push_back({"that's the ", kWhite});
        l.push_back(StringSeg(other));
        l.push_back({", use ", kWhite});
        AddWhere(&l, string, fret);
        return l;
    };
    // Or the right string, a few frets off.
    const int nf = ef + d;
    auto moveFrets = [&]() {
        at = {string, ChartFret(neck, nf), heard};
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
    at = NearestSpot(neck, got, string, ef, heard);
    l.push_back({std::string("too ") + (d > 0 ? "high" : "low") + ": play ", kWhite});
    AddWhere(&l, string, fret);
    return l;
}

namespace {

// The chord's strings, compared with what was heard.
struct ChordStrings {
    bool played[6] = {};   // part of the chord
    bool missing[6] = {};  // part of the chord, but its note isn't heard
    bool advised[6] = {};  // already has a piece of advice
    bool muted = false;    // the chord has x (not played) strings
    int lowest = 999;      // the chord's lowest note (MIDI)
};

// The string whose note is 1-2 semitones from the wrong pitch class e (preferring strings whose note
// isn't heard at all), -1 = none; *d = heard - wanted, in semitones.
int NearestString(const Neck& neck, const int notes[6], const ChordStrings& cs, int e, int* d) {
    int best = -1, bestScore = 99;
    for (int s = 0; s < neck.strings && s < 6; ++s) {
        if (!cs.played[s] || cs.advised[s]) continue;
        const int dd = ((e - Pc(notes[s])) % 12 + 18) % 12 - 6;  // -6..5 semitones, heard - wanted
        if (dd == 0 || std::abs(dd) > 2) continue;
        const int score = std::abs(dd) + (cs.missing[s] ? 0 : 10);
        if (score < bestScore) { best = s; *d = dd; bestScore = score; }
    }
    return best;
}

// "BLUE string is 1 fret too high: move DOWN to fret 2"
Line WrongFret(int s, int fret, int d) {
    Line l = {StringSeg(s)};
    const std::string off = " is " + Frets(std::abs(d)) + " too " + (d > 0 ? "high" : "low");
    if (fret == 0) l.push_back({off + ": play it open", kWhite});
    else l.push_back({off + ": move " + (d > 0 ? "DOWN" : "UP") + " to fret " + std::to_string(fret), kWhite});
    return l;
}

// A string that isn't part of the chord is ringing (pitch classes).
Line Ringing(const std::vector<int>& pcs, bool muted) {
    std::string names;
    for (size_t i = 0; i < pcs.size(); ++i)
        names += (i ? (i + 1 == pcs.size() ? " and " : ", ") : "") + music::NoteName(pcs[i]);
    return {{"a note that isn't in the chord is ringing (" + names + ")" + (muted ? ": don't strum the x strings" : ""), kWhite}};
}

// Strings that don't sound (not pressed firmly, or not strummed); empty if there are none left.
Line NotSounding(const Neck& neck, const int frets[6], const ChordStrings& cs) {
    Line l = {{"not sounding: ", kWhite}};
    int n = 0;
    for (int s = 0; s < neck.strings && s < 6; ++s) {
        if (!cs.missing[s] || cs.advised[s]) continue;
        if (n++) l.push_back({", ", kWhite});
        l.push_back(StringSeg(s));
        l.push_back({frets[s] == 0 ? " (open)" : " (fret " + std::to_string(frets[s]) + ")", kWhite});
    }
    l.push_back({" - press firmly and strum every string", kWhite});
    return n ? l : Line{};
}

}  // namespace

Line ForChord(const Neck& neck, const int frets[6], const int notes[6], const std::vector<int>& heard,
              const std::vector<int>& extraPcs, int hits, int needed, std::vector<Mark>* where) {
    if (where) where->clear();
    bool got[12] = {};
    for (int m : heard) got[Pc(m)] = true;
    ChordStrings cs;
    for (int s = 0; s < neck.strings && s < 6; ++s) {
        cs.played[s] = frets[s] >= 0 && notes[s] >= 0;
        cs.missing[s] = cs.played[s] && !got[Pc(notes[s])];
        cs.muted = cs.muted || frets[s] < 0;
    }
    for (int s = 0; s < 6; ++s) if (cs.played[s]) cs.lowest = std::min(cs.lowest, notes[s]);

    std::vector<Line> advice;
    std::vector<int> unexplained;
    // A wrong note 1-2 frets from one of the chord's strings: that finger is on the wrong fret.
    for (int e : extraPcs) {
        // Only heard below the chord's lowest note: a string that shouldn't be strummed at all.
        bool below = false;
        for (int m : heard) if (Pc(m) == e) { below = m < cs.lowest - 2; break; }
        int d = 0;
        const int best = below ? -1 : NearestString(neck, notes, cs, e, &d);
        if (best < 0 || !Playable(neck, Sounding(neck, frets[best]) + d)) { unexplained.push_back(e); continue; }
        cs.advised[best] = true;
        cs.missing[best] = false;
        advice.push_back(WrongFret(best, frets[best], d));
        if (where) where->push_back({best, ChartFret(neck, Sounding(neck, frets[best]) + d), notes[best] + d});
    }
    if (!unexplained.empty()) advice.push_back(Ringing(unexplained, cs.muted));
    if (hits < needed) {
        Line l = NotSounding(neck, frets, cs);
        if (!l.empty()) advice.push_back(l);
    }

    Line out;
    for (size_t i = 0; i < advice.size() && i < 2; ++i) {  // at most two pieces of advice at once
        if (i) out.push_back({"   \xC2\xB7   ", kGrey});
        out.insert(out.end(), advice[i].begin(), advice[i].end());
    }
    if (!out.empty()) out.insert(out.begin(), {"Fix:  ", kGrey});
    return out;
}

std::vector<Mark> SameNoteElsewhere(const Neck& neck, const Mark& at) {
    std::vector<Mark> out;
    if (at.string < 0 || at.string >= neck.strings) return out;
    const int pitch = neck.open[at.string] + Sounding(neck, at.fret);
    for (int s = 0; s < neck.strings && s < 6; ++s) {
        const int sf = pitch - neck.open[s];
        if (s == at.string || !Playable(neck, sf)) continue;
        out.push_back({s, ChartFret(neck, sf), at.midi, false});
    }
    return out;
}

std::string Text(const Line& line) {
    std::string s;
    for (const auto& seg : line) s += seg.text;
    return s;
}

}  // namespace nbn::hint
