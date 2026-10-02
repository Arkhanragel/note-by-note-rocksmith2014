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
    // A linked chain: vibrato that ends in an unpitched slide (the note after it, not picked again),
    // and a slide into a pull-off.
    auto seq = [&](const char* what, const std::vector<tq::Link>& chain, const char* want) {
        std::string got;
        for (const auto& w : tq::Sequence(chain)) got += (got.empty() ? "" : " | ") + w.name + ": " + w.how;
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  %-16s -> \"%s\"\n", ok ? "ok  " : "FAIL", what, got.c_str());
        if (!ok) std::printf("      wanted \"%s\"\n", want);
    };
    seq("vibrato->slide", {{{tq::kVibrato | tq::kParent, -1, -1, 0}, 12}, {{tq::kUnpitchedSlide | tq::kChild, -1, 7, 0}, 12}},
        "Vibrato: shake the note a little while it rings | Slide: at the end, slide down towards fret 7 as the note fades");
    seq("slide->pull-off", {{{tq::kSlide | tq::kParent, 9, -1, 0}, 7}, {{tq::kPullOff | tq::kChild, -1, -1, 0}, 5}},
        "Slide: then slide UP to fret 9, keep the string pressed | Pull-off: then, without picking, pull your finger off so fret 5 sounds");
    // A chord: palm mute on its strings, said once; a slide of the whole chord from its lowest string.
    {
        tq::Technique str[6];
        const int frets[6] = {3, 5, -1, -1, -1, -1};
        str[0].mask = str[1].mask = tq::kPalmMute;
        int f = 0;
        words("palm-muted chord", tq::ForChord(0x2, str, frets, &f), f, "Palm mute: rest the side of your picking hand on the strings, near the bridge");
        str[0] = {tq::kUnpitchedSlide, -1, 1, 0};
        str[1] = {tq::kUnpitchedSlide, -1, 3, 0};
        words("sliding chord", tq::ForChord(0x4000002, str, frets, &f), f,
              "Accent: play it louder than the others | Slide: at the end, slide down towards fret 1 as the note fades");
    }
    std::printf("bend labels: %s, %s, %s\n", tq::BendLabel(0.5f).c_str(), tq::BendLabel(1).c_str(), tq::BendLabel(1.5f).c_str());

    // Sections, as the game keeps them (Seven Nation Army remix, lead): split at every phrase, the pieces
    // after the first numbered on from 13. Joined again and numbered 1, 2, 3.
    {
        const std::vector<nbn::music::SectionPiece> pieces = {
            {"intro", 1, 33.219, 35.252},       {"verse", 1, 35.252, 51.201},       {"postvs", 1, 51.201, 67.192},
            {"postvs", 13, 67.192, 71.224},     {"chorus", 1, 71.224, 87.205},      {"chorus", 14, 87.205, 91.253},
            {"postchorus", 1, 91.253, 105.185}, {"postchorus", 15, 105.185, 107.186}, {"verse", 2, 107.186, 123.204},
            {"postvs", 2, 123.204, 139.209},    {"postvs", 16, 139.209, 143.210},   {"chorus", 2, 143.210, 159.203},
            {"solo", 1, 159.203, 175.244},      {"solo", 17, 175.244, 179.244},     {"postchorus", 2, 179.244, 187.246},
            {"noguitar", 18, 187.246, 193.209}, {"postchorus", 19, 193.209, 195.208}, {"verse", 3, 195.208, 211.182},
            {"postvs", 3, 211.182, 227.227},    {"postvs", 20, 227.227, 231.238},   {"chorus", 3, 231.238, 247.196},
            {"chorus", 21, 247.196, 251.196},   {"noguitar", 1, 251.196, 260.162},
        };
        std::string got;
        for (const auto& sec : nbn::music::NameSections(pieces)) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s%s %.0f", got.empty() ? "" : ", ", sec.name.c_str(), sec.start);
            got += b;
        }
        const std::string want = "Intro 33, Verse 1 35, Post-verse 1 51, Chorus 1 71, Post-chorus 1 91, Verse 2 107, "
                                 "Post-verse 2 123, Chorus 2 143, Solo 159, Post-chorus 2 179, No guitar 187, Post-chorus 2 193, "
                                 "Verse 3 195, Post-verse 3 211, Chorus 3 231, No guitar 251";
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  sections -> %s\n", ok ? "ok  " : "FAIL", got.c_str());
        if (!ok) std::printf("      wanted %s\n", want.c_str());
        const bool w = nbn::music::SectionWord("modchorus") == "Chorus (new key)" && nbn::music::SectionWord("weird") == "Weird";
        fails += !w;
        std::printf("%s  section words\n", w ? "ok  " : "FAIL");
    }

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
