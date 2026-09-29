// technique.cpp: see technique.h.
#include "technique.h"

#include <algorithm>
#include <cmath>

namespace nbn::technique {

bool Technique::Any() const {
    constexpr uint32_t kTold = kTremolo | kHarmonic | kPalmMute | kSlap | kPluck | kHammerOn | kPullOff | kSlide |
                               kBend | kTap | kPinchHarmonic | kVibrato | kMute | kUnpitchedSlide | kAccent;
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

std::vector<Words> Describe(const Technique& t, int fret, size_t max) {
    std::vector<Words> out;
    auto add = [&](const char* name, std::string how) {
        if (out.size() < max) out.push_back({name, std::move(how)});
    };
    const uint32_t m = t.mask;
    // How the note starts (instead of picking it).
    if (m & kHammerOn) add("Hammer-on", "don't pick: hit the fret hard with a finger of your fretting hand");
    if (m & kPullOff) add("Pull-off", "don't pick: pull your finger off the string so it sounds");
    if (m & kTap) add("Tap", "hit the fret with a finger of your picking hand");
    if (m & kSlap) add("Slap", "hit the string with the side of your thumb");
    if (m & kPluck) add("Pop", "hook the string with a finger and let it snap back");
    // How it sounds.
    if (m & kMute) add("Muted", "touch the string without pressing it down: just a dull click");
    if (m & kPalmMute) add("Palm mute", "rest the side of your picking hand on the strings, near the bridge");
    if (m & kHarmonic) add("Harmonic", "touch the string right over the fret wire, don't press it down");
    if (m & kPinchHarmonic) add("Pinch harmonic", "let your thumb graze the string as you pick it");
    // What happens while it rings.
    if ((m & kSlide) && t.slideTo >= 0 && t.slideTo != fret)
        add("Slide", std::string("then slide ") + (t.slideTo > fret ? "UP" : "DOWN") + " to fret " +
                         std::to_string(t.slideTo) + ", keep the string pressed");
    if ((m & kUnpitchedSlide) && t.slideUnpitchTo >= 0 && t.slideUnpitchTo != fret)
        add("Slide", std::string("at the end, slide ") + (t.slideUnpitchTo > fret ? "up" : "down") +
                         " towards fret " + std::to_string(t.slideUnpitchTo) + " as the note fades");
    if ((m & kBend) && t.bend > 0.1f) {
        const int halves = std::max(1, (int)std::lround(t.bend * 2));
        add("Bend", "push the string sideways until it sounds " + BendSteps(t.bend) + " higher (like " +
                        std::to_string(halves) + (halves == 1 ? " fret" : " frets") + " up)");
    }
    if (m & kVibrato) add("Vibrato", "shake the note a little while it rings");
    if (m & kTremolo) add("Tremolo", "pick it very fast, again and again");
    if (m & kAccent) add("Accent", "play it louder than the others");
    return out;
}

}  // namespace nbn::technique
