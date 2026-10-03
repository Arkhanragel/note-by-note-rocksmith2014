// technique.cpp: see technique.h.
#include "technique.h"

#include <algorithm>
#include <cmath>

namespace nbn::technique {

bool Technique::Any() const {
    constexpr uint32_t kTold = kChordMute | kTremolo | kHarmonic | kPalmMute | kSlap | kPluck | kHammerOn | kPullOff | kSlide |
                               kBend | kTap | kPinchHarmonic | kVibrato | kMute | kUnpitchedSlide | kAccent | kParent;
    return (mask & kTold) != 0;
}

std::string BendSteps(float steps) {
    const int halves = (int)std::lround(steps * 2);  // in half steps
    if (halves <= 1) return "half a step";
    const std::string whole = std::to_string(halves / 2);
    if (halves % 2) return whole + " and a half steps";
    return whole + (halves == 2 ? " step" : " steps");
}

std::string BendLabel(float steps) {
    const int halves = (int)std::lround(steps * 2);
    if (halves <= 1) return "1/2";
    const std::string whole = std::to_string(halves / 2);
    return halves % 2 ? whole + " 1/2" : whole;
}

namespace {

// The steps being collected: at most `max` of them (the banner has room for a few).
struct Steps {
    std::vector<Words> out;
    size_t max = 0;
    void Add(const char* name, std::string how) {
        if (out.size() < max) out.push_back({name, std::move(how)});
    }
};

// The chain's first note: how it starts (instead of picking it), and how it sounds.
void StartWords(uint32_t m, Steps* s) {
    if (m & kHammerOn) s->Add("Hammer-on", "don't pick: hit the fret hard with a finger of your fretting hand");
    if (m & kPullOff) s->Add("Pull-off", "don't pick: pull your finger off the string so it sounds");
    if (m & kTap) s->Add("Tap", "hit the fret with a finger of your picking hand");
    if (m & kSlap) s->Add("Slap", "hit the string with the side of your thumb");
    if (m & kPluck) s->Add("Pop", "hook the string with a finger and let it snap back");
    if (m & kMute) s->Add("Muted", "touch the string without pressing it down: just a dull click");
    if (m & kChordMute) s->Add("Muted chord", "keep your fingers on the strings without pressing them: a dull \"chk\"");
    if (m & kPalmMute) s->Add("Palm mute", "rest the side of your picking hand on the strings, near the bridge");
    if (m & kHarmonic) s->Add("Harmonic", "touch the string right over the fret wire, don't press it down");
    if (m & kPinchHarmonic) s->Add("Pinch harmonic", "let your thumb graze the string as you pick it");
    if (m & kAccent) s->Add("Accent", "play it louder than the others");
}

// A linked note: reached without picking again (unless the slide before already took the finger there).
void LinkedWords(const Link& before, const Link& link, Steps* s) {
    const uint32_t m = link.tech.mask;
    const int fret = link.fret;
    const Technique& p = before.tech;
    const bool slidHere = ((p.mask & kSlide) && p.slideTo == fret);
    const std::string to = "fret " + std::to_string(fret);
    if (m & kHammerOn) s->Add("Hammer-on", "then, without picking, hit " + to + " hard with another finger");
    else if (m & kPullOff) s->Add("Pull-off", "then, without picking, pull your finger off so " + to + " sounds");
    else if (!slidHere && fret != before.fret) s->Add("Linked", "then, without picking, move to " + to);
}

// What happens while the note rings, and how it ends. first: the chain's first note (the later ones'
// steps happen "then").
void RingAndEndWords(const Link& link, bool first, Steps* s) {
    const Technique& t = link.tech;
    const uint32_t m = t.mask;
    const int fret = link.fret;
    const std::string then = first ? "" : "then ";  // later notes of the chain happen after the first
    // While it rings.
    if ((m & kBend) && t.bend > 0.1f) {
        const int halves = std::max(1, (int)std::lround(t.bend * 2));
        s->Add("Bend", then + "push the string sideways until it sounds " + BendSteps(t.bend) + " higher (like " +
                           std::to_string(halves) + (halves == 1 ? " fret" : " frets") + " up)");
    }
    if (m & kVibrato) s->Add("Vibrato", then + "shake the note a little while it rings");
    if (m & kTremolo) s->Add("Tremolo", then + "pick it very fast, again and again");
    // How it ends.
    if ((m & kSlide) && t.slideTo >= 0 && t.slideTo != fret)
        s->Add("Slide", std::string(first ? "then slide " : "then, without picking, slide ") + (t.slideTo > fret ? "UP" : "DOWN") +
                            " to fret " + std::to_string(t.slideTo) + ", keep the string pressed");
    if ((m & kUnpitchedSlide) && t.slideUnpitchTo >= 0 && t.slideUnpitchTo != fret)
        s->Add("Slide", std::string("at the end, slide ") + (t.slideUnpitchTo > fret ? "up" : "down") +
                            " towards fret " + std::to_string(t.slideUnpitchTo) + " as the note fades");
}

}  // namespace

std::vector<Words> Sequence(const std::vector<Link>& chain, size_t max) {
    Steps steps{{}, max};
    for (size_t k = 0; k < chain.size(); ++k) {
        if (k == 0) StartWords(chain[k].tech.mask, &steps);
        else LinkedWords(chain[k - 1], chain[k], &steps);
        RingAndEndWords(chain[k], k == 0, &steps);
    }
    return steps.out;
}

std::vector<Words> Describe(const Technique& t, int fret, size_t max) { return Sequence({{t, fret}}, max); }

Technique ForChord(uint32_t chordMask, const Technique strings[6], const int frets[6], int* fret) {
    Technique out;
    out.mask = chordMask & (kChordMute | kPalmMute | kAccent | kTremolo);
    *fret = 0;
    bool haveFret = false;
    for (int s = 0; s < 6; ++s) {
        if (frets[s] < 0) continue;
        const Technique& t = strings[s];
        out.mask |= t.mask;
        if (!haveFret) { *fret = frets[s]; haveFret = true; }
        // The slide / bend of the lowest string that has one (all of a chord's strings usually move
        // together); its fret is the one the words refer to.
        if (out.slideTo < 0 && out.slideUnpitchTo < 0 && (t.slideTo >= 0 || t.slideUnpitchTo >= 0)) {
            out.slideTo = t.slideTo;
            out.slideUnpitchTo = t.slideUnpitchTo;
            *fret = frets[s];
        }
        out.bend = std::max(out.bend, t.bend);
    }
    return out;
}

}  // namespace nbn::technique
