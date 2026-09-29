// nbn_music_test: checks the friendly chord wording (music.h). Prints each case; exit code 1 on failure.
#include <cstdio>
#include <string>
#include <vector>

#include "music.h"
#include "technique.h"

int main() {
    struct Case {
        const char* name;       // chord name as written in the song ("" = none)
        std::vector<int> notes; // MIDI, lowest string first
        const char* meaning;
        const char* list;
    };
    const Case cases[] = {
        {"B5", {59, 66}, "B power chord", "B and F#"},                 // Sanctuary
        {"A5", {57, 59, 64}, "A power chord", "A, B and E"},           // the song's name wins over the notes
        {"A3", {61, 69}, "part of A major", "C# and A"},               // odd name -> recognised from the notes
        {"", {59, 68}, "part of G# minor", "B and G#"},
        {"Em", {40, 47, 52, 55, 59, 64}, "E minor", "E, B and G"},
        {"G/B", {47, 50, 55, 59}, "G major, with B as the lowest note", "B, D and G"},
        {"Bb", {46, 53, 58, 62}, "Bb major", "Bb, F and D"},
        {"Cmaj7", {48, 52, 55, 59}, "C major 7th", "C, E, G and B"},
        {"C#m", {49, 56, 61, 64}, "C# minor", "C#, G# and E"},
        {"", {45, 52, 57, 61, 64}, "A major", "A, E and C#"},
        {"", {47, 55, 62}, "G major, with B as the lowest note", "B, G and D"},
        {"", {40, 41}, "", "E and F"},                                  // not a chord shape: just the notes
    };
    int fails = 0;
    for (const auto& c : cases) {
        const std::string m = nbn::music::ChordMeaning(c.name, c.notes);
        const std::string l = nbn::music::NoteList(c.notes, nbn::music::UsesFlats(c.name));
        const bool ok = m == c.meaning && l == c.list;
        fails += !ok;
        std::printf("%s  %-6s -> \"%s\"  notes \"%s\"", ok ? "ok  " : "FAIL", c.name, m.c_str(), l.c_str());
        if (!ok) std::printf("   (wanted \"%s\", \"%s\")", c.meaning, c.list);
        std::printf("\n");
    }
    // Techniques in words (technique.h).
    namespace tq = nbn::technique;
    auto words = [&](const char* what, const tq::Technique& t, int fret, const char* want) {
        std::string got;
        for (const auto& w : tq::Describe(t, fret)) got += (got.empty() ? "" : " | ") + w.name + ": " + w.how;
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  %-16s -> \"%s\"\n", ok ? "ok  " : "FAIL", what, got.c_str());
        if (!ok) std::printf("      wanted \"%s\"\n", want);
    };
    words("slide up", {tq::kSlide, 9, -1, 0}, 7, "Slide: then slide UP to fret 9, keep the string pressed");
    words("unpitched slide", {tq::kUnpitchedSlide, -1, 10, 0}, 12, "Slide: at the end, slide down towards fret 10 as the note fades");
    words("bend 1 step", {tq::kBend, -1, -1, 1.0f}, 12, "Bend: push the string sideways until it sounds 1 step higher (like 2 frets up)");
    words("bend half", {tq::kBend, -1, -1, 0.5f}, 12, "Bend: push the string sideways until it sounds half a step higher (like 1 fret up)");
    words("hammer + vibrato", {tq::kHammerOn | tq::kVibrato, -1, -1, 0}, 7,
          "Hammer-on: don't pick: hit the fret hard with a finger of your fretting hand | Vibrato: shake the note a little while it rings");
    words("nothing", {0x800000, -1, -1, 0}, 5, "");  // the "single note" bit alone tells nothing
    std::printf("bend labels: %s, %s, %s\n", tq::BendLabel(0.5f).c_str(), tq::BendLabel(1).c_str(), tq::BendLabel(1.5f).c_str());

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
