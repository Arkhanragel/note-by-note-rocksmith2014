// tuning.cpp: see tuning.h.
#include "tuning.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "music.h"

namespace nbn::tuning {
namespace {

const char* kLetter[6] = {"E", "A", "D", "G", "B", "e"};  // standard names of the strings (as hint.cpp)
const int kStandard[6] = {40, 45, 50, 55, 59, 64};

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n == 0 ? 0 : (n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]));
}

// The song's tuning as one shift for every string (E standard = 0, Eb standard = -1), or a drop tuning
// (the thickest string 2 lower than the rest); `shift` gets the shift of the strings above the drop.
enum class Shape { kStandard, kDrop, kOther };
Shape TuningShape(const int open[6], int strings, int* shift) {
    const int bassDown = strings == 4 ? 12 : 0;  // the chart's bass strings are an octave below the guitar's
    int d[6];
    for (int s = 0; s < strings; ++s) d[s] = open[s] + bassDown - kStandard[s];
    *shift = d[std::min(1, strings - 1)];
    for (int s = 1; s < strings; ++s)
        if (d[s] != *shift) return Shape::kOther;
    return d[0] == *shift ? Shape::kStandard : d[0] == *shift - 2 ? Shape::kDrop : Shape::kOther;
}

// Note names of the tuning: flats for the Eb family ("Eb standard", Eb Ab Db Gb Bb Eb), else sharps.
bool Flats(const int open[6], int strings) {
    int shift = 0;
    return TuningShape(open, strings, &shift) != Shape::kOther && shift == -1;
}

// "string 3 (D)", coloured like that string (hint.cpp's StringSeg).
hint::Seg StringSeg(const hint::Neck& n, int s) {
    const int number = n.fromThick ? s + 1 : n.strings - s;
    return {"string " + std::to_string(number) + " (" + kLetter[s] + ")", s};
}

// Off by a whole number of half steps (give or take a little), not somewhere between.
bool Whole(const Finding& f) { return std::abs(f.offset - f.steps) < Check::kBetween; }

// "a bit low", "low", "a half step high", "a whole step low", "more than a half step low"...
std::string HowFar(const Finding& f) {
    const char* way = f.offset < 0 ? "low" : "high";
    if (f.steps == 0) return std::string(std::abs(f.offset) < 0.45 ? "a bit " : "") + way;
    const int n = std::abs(f.steps);
    const std::string amount = n == 1 ? "a half step " : n == 2 ? "a whole step " : "several half steps ";
    if (Whole(f)) return amount + way;
    return (std::abs(f.offset) > n ? "more than " : "almost ") + amount + way;
}

// "up a little", "down a half step", "up a whole step"; just "up" for an amount in between (the
// game's tuner does the rest).
std::string HowMuch(const Finding& f) {
    const char* way = f.offset < 0 ? "up" : "down";
    const int n = std::abs(f.steps);
    if (n > 0 && !Whole(f)) return way;
    return std::string(way) + (n == 0 ? " a little" : n == 1 ? " a half step" : n == 2 ? " a whole step" : "");
}

}  // namespace

// ------------------------------------------------------------------ one note's steady pitch
void SteadyPitch::Start(int midi) {
    on_ = true;
    midi_ = midi;
    block_ = 0;
    gap_ = 0;
    got_.clear();
}

bool SteadyPitch::Frame(double pitch, double* out) {
    if (!on_) return false;
    if (++block_ < kFirstBlock) return false;
    if (pitch < 0 || std::abs(pitch - midi_) > 0.6) {
        // Silence or another note for a moment (~16 ms): the note ended, or the next one started.
        if (++gap_ > 3) return Finish(out);
    } else {
        gap_ = 0;
        got_.push_back(pitch);
    }
    return block_ >= kLastBlock && Finish(out);
}

bool SteadyPitch::Finish(double* out) {
    on_ = false;
    if ((int)got_.size() < kMinFrames) return false;
    *out = Median(got_);
    return true;
}

// ------------------------------------------------------------------ the check, per string
void Check::Reset() {
    for (auto& n : notes_) n.clear();
}

void Check::Add(int string, int wanted, double heard) {
    if (string < 0 || string >= 6) return;
    double off = heard - wanted;
    while (off > 6) off -= 12;  // an octave off (octaves accepted, a bass chart heard an octave up)
    while (off < -6) off += 12;
    if (std::abs(off) > 2.5) return;  // another note altogether: a wrong fret or string, not the tuning
    notes_[string].push_back({wanted, off});
    if ((int)notes_[string].size() > kKeep) notes_[string].pop_front();
}

Finding Check::ForString(int s, bool* inTune) const {
    *inTune = false;
    const auto& notes = notes_[s];
    if ((int)notes.size() < kNeed) return {};

    // A half step (or two) off: most of its notes that far off, on different notes, and none of the
    // last few right (a hand off by a fret for a while looks the same, but gets corrected).
    for (int k : {-1, 1, -2, 2}) {
        std::vector<double> got;
        std::set<int> wanted;
        for (const auto& [w, o] : notes)
            if (std::abs(o - k) < 0.3) {
                got.push_back(o);
                wanted.insert(w);
            }
        bool rightLately = false;
        for (size_t i = notes.size() - std::min<size_t>(4, notes.size()); i < notes.size(); ++i)
            rightLately = rightLately || std::abs(notes[i].second) < 0.3;
        if ((int)got.size() >= kNeed && wanted.size() >= 2 && !rightLately) return {s, false, Median(got), k};
    }

    // Off by an amount between whole half steps ("1.4 half steps low"): no wrong fret or string gives
    // that on a guitar in tune, so the same note again and again counts too. (User test 2026-10-02: an
    // open string tuned down by ear, 142 cents low on every pick; the rules above and below missed it.)
    {
        std::vector<double> all;
        for (const auto& [w, o] : notes) all.push_back(o);
        const double m = Median(all);
        int agree = 0;
        for (double o : all) agree += std::abs(o - m) <= kAgree;
        const int k = (int)std::lround(m);
        if (std::abs(m) >= kNear && std::abs(m - k) >= kBetween && agree >= kNeed && agree >= kAgreeShare * all.size())
            return {s, false, m, k};
    }

    // A little off: the notes near the right pitch, most of them agreeing.
    std::vector<double> near;
    for (const auto& [w, o] : notes)
        if (std::abs(o) < kNear) near.push_back(o);
    if ((int)near.size() < kNeed) return {};
    const double m = Median(near);
    int agree = 0;
    for (double o : near) agree += std::abs(o - m) <= kAgree;
    if (agree < kNeed || agree < kAgreeShare * near.size()) return {};
    if (std::abs(m) < kLittle * 0.6) *inTune = true;
    if (std::abs(m) < kLittle) return {};
    return {s, false, m, 0};
}

Finding Check::Get() const {
    std::vector<Finding> off;
    bool anyInTune = false;
    for (int s = 0; s < 6; ++s) {
        bool inTune = false;
        const Finding f = ForString(s, &inTune);
        anyInTune = anyInTune || inTune;
        if (f.string >= 0) off.push_back(f);
    }
    if (off.empty()) return {};
    // Two strings or more off the same way, and none known to be right: the whole guitar (a guitar
    // tuned for another song). Else the string that's furthest off.
    bool same = off.size() >= 2 && !anyInTune;
    std::vector<double> offsets;
    for (const auto& f : off) {
        same = same && f.steps == off[0].steps && (f.offset < 0) == (off[0].offset < 0);
        offsets.push_back(f.offset);
    }
    if (same) return {-1, true, Median(offsets), off[0].steps};
    return *std::max_element(off.begin(), off.end(), [](const Finding& a, const Finding& b) {
        return std::abs(a.steps) != std::abs(b.steps) ? std::abs(a.steps) < std::abs(b.steps) : std::abs(a.offset) < std::abs(b.offset);
    });
}

// ------------------------------------------------------------------ words
std::string TuningName(const int open[6], int strings) {
    int shift = 0;
    const Shape shape = TuningShape(open, strings, &shift);
    const bool flats = Flats(open, strings);
    if (shape == Shape::kStandard) return music::NoteName(kStandard[0] + shift, flats) + " standard";
    if (shape == Shape::kDrop) return "Drop " + music::NoteName(kStandard[0] + shift - 2, flats);
    std::string out;
    for (int s = 0; s < strings; ++s) out += (s ? " " : "") + music::NoteName(open[s], flats);
    return out;
}

hint::Line Advice(const Finding& f, const hint::Neck& neck) {
    if (f.string < 0 && !f.all) return {};
    const bool flats = Flats(neck.open, neck.strings);
    // When the guitar is a half step or more off, it's probably tuned for another song: name this one's.
    const std::string song = f.steps != 0 ? "This song is in " + TuningName(neck.open, neck.strings) + ": tune" : "Tune";
    hint::Line l = {{"Out of tune?  -  ", hint::kGrey}};
    if (f.all) {
        std::string notes;
        for (int s = 0; s < neck.strings; ++s) notes += (s ? " " : "") + music::NoteName(neck.open[s], flats);
        l.push_back({"Your guitar sounds " + HowFar(f) + ". " + song + " every string " + HowMuch(f) + " (" + notes + ")"});
        return l;
    }
    l.push_back(StringSeg(neck, f.string));
    l.push_back({" sounds " + HowFar(f) + ". " + song + " it " + HowMuch(f) + ", to " + music::NoteName(neck.open[f.string], flats)});
    return l;
}

std::string AdviceText(const Finding& f, const hint::Neck& neck) {
    std::string out;
    for (const auto& s : Advice(f, neck)) out += s.text;
    return out;
}

}  // namespace nbn::tuning
