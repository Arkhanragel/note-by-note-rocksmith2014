// nbn_hint_test: checks the "what went wrong" advice (hint.h). Prints each case; exit code 1 on failure.
#include <cstdio>
#include <string>
#include <vector>

#include "hint.h"

using nbn::hint::Neck;

int main() {
    const Neck gtr;  // standard tuning, no capo
    Neck capo2 = gtr;
    capo2.capo = 2;
    int fails = 0;
    auto check = [&](const char* what, const nbn::hint::Line& l, const char* want) {
        const std::string got = nbn::hint::Text(l);
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  %-28s -> \"%s\"", ok ? "ok  " : "FAIL", what, got.c_str());
        if (!ok) std::printf("\n      wanted \"%s\"", want);
        std::printf("\n");
    };

    // Single notes (strings: 0 = low E ... 5 = high e; MIDI 40 45 50 55 59 64 open).
    check("2 frets too high", nbn::hint::ForNote(gtr, 2, 5, 55, 57), "You played A  -  move DOWN 2 frets, to fret 5 on string 4 (D)");
    check("1 fret too low", nbn::hint::ForNote(gtr, 2, 5, 55, 54), "You played F#  -  move UP 1 fret, to fret 5 on string 4 (D)");
    check("same fret, string below", nbn::hint::ForNote(gtr, 2, 5, 55, 60), "You played C  -  that's string 3 (G), use fret 5 on string 4 (D)");
    check("same fret, string above", nbn::hint::ForNote(gtr, 2, 5, 55, 50), "You played D  -  that's string 5 (A), use fret 5 on string 4 (D)");
    check("octave", nbn::hint::ForNote(gtr, 2, 5, 55, 67), "You played G  -  right note, but an octave too high: play fret 5 on string 4 (D)");
    check("open string pressed", nbn::hint::ForNote(gtr, 0, 0, 40, 42), "You played F#  -  don't press any fret: play string 6 (E) open");
    check("far off", nbn::hint::ForNote(gtr, 0, 3, 43, 76), "You played E  -  too high: play fret 3 on string 6 (E)");
    check("capo, open wanted", nbn::hint::ForNote(capo2, 1, 0, 47, 48), "You played C  -  don't press any fret: play string 5 (A) open");
    check("right note", nbn::hint::ForNote(gtr, 2, 5, 55, 55), "");
    Neck thick = gtr;  // numbered from the thickest: string 1 = low E
    thick.fromThick = true;
    check("numbered from the thickest", nbn::hint::ForNote(thick, 2, 5, 55, 60), "You played C  -  that's string 4 (G), use fret 5 on string 3 (D)");

    // The sound told where the wrong note was played (string identification).
    using nbn::hint::ForNotePlayedOn;
    check("known: same string", ForNotePlayedOn(gtr, 2, 5, 55, 57, 2), "You played A  -  move DOWN 2 frets, to fret 5 on string 4 (D)");
    check("known: other string", ForNotePlayedOn(gtr, 2, 5, 55, 60, 3), "You played C  -  that's fret 5 on string 3 (G), use fret 5 on string 4 (D)");
    check("known: other string, far", ForNotePlayedOn(gtr, 2, 5, 55, 57, 1), "You played A  -  that's fret 12 on string 5 (A), use fret 5 on string 4 (D)");
    check("known: open string", ForNotePlayedOn(gtr, 2, 5, 55, 64, 5), "You played E  -  that's string 1 (e) open, use fret 5 on string 4 (D)");
    check("known: octave", ForNotePlayedOn(gtr, 2, 5, 55, 67, 3), "You played G  -  right note, but an octave too high: play fret 5 on string 4 (D)");
    check("known: open wanted", ForNotePlayedOn(gtr, 0, 0, 40, 42, 0), "You played F#  -  don't press any fret: play string 6 (E) open");
    check("known: right note", ForNotePlayedOn(gtr, 2, 5, 55, 55, 2), "");

    // Chords.
    const int a5f[6] = {-1, 0, 2, -1, -1, -1}, a5n[6] = {-1, 45, 52, -1, -1, -1};
    check("A5, finger 1 fret high", nbn::hint::ForChord(gtr, a5f, a5n, {{45, 53}, {5}, 1, 2}),
          "Fix:  string 4 (D) is 1 fret too high: move DOWN to fret 2");
    const int d5f[6] = {-1, -1, 0, 2, -1, -1}, d5n[6] = {-1, -1, 50, 57, -1, -1};
    check("D5, low E strummed", nbn::hint::ForChord(gtr, d5f, d5n, {{40, 50, 57}, {4}, 2, 2}),
          "Fix:  a note that isn't in the chord is ringing (E): don't strum the x strings");
    const int emf[6] = {0, 2, 2, 0, 0, 0}, emn[6] = {40, 47, 52, 55, 59, 64};
    check("Em, strings not sounding", nbn::hint::ForChord(gtr, emf, emn, {{40, 52, 64}, {}, 1, 2}),
          "Fix:  not sounding: string 5 (A) at fret 2, string 3 (G) open, string 2 (B) open - press firmly and strum every string");

    // Where the wrong note was probably played (the red X of the banner's fretboard).
    auto at = [&](const char* what, nbn::hint::Mark m, int string, int fret, int midi) {
        const bool ok = m.string == string && m.fret == fret && m.midi == midi;
        fails += !ok;
        std::printf("%s  %-28s -> string %d fret %d midi %d", ok ? "ok  " : "FAIL", what, m.string, m.fret, m.midi);
        if (!ok) std::printf("\n      wanted string %d fret %d midi %d", string, fret, midi);
        std::printf("\n");
    };
    nbn::hint::Mark m;
    nbn::hint::ForNote(gtr, 2, 5, 55, 57, &m);
    at("X: 2 frets too high", m, 2, 7, 57);
    nbn::hint::ForNote(gtr, 2, 5, 55, 60, &m);
    at("X: string below", m, 3, 5, 60);
    nbn::hint::ForNote(gtr, 0, 0, 40, 42, &m);
    at("X: open string pressed", m, 0, 2, 42);
    nbn::hint::ForNote(gtr, 2, 5, 55, 67, &m);
    at("X: octave", m, 4, 8, 67);
    nbn::hint::ForNote(capo2, 1, 0, 47, 48, &m);
    at("X: capo", m, 1, 3, 48);
    nbn::hint::ForNote(gtr, 2, 5, 55, 55, &m);
    at("X: right note = none", m, -1, -1, -1);
    std::vector<nbn::hint::Mark> ms = nbn::hint::SameNoteElsewhere(gtr, {1, 8, 53});  // F3 at A8, as in a real test
    at("same F3: low E 13", ms.size() == 2 ? ms[0] : nbn::hint::Mark{}, 0, 13, 53);
    at("same F3: D 3", ms.size() == 2 ? ms[1] : nbn::hint::Mark{}, 2, 3, 53);
    ForNotePlayedOn(gtr, 2, 5, 55, 57, 1, &m);
    at("X: known, A string 12", m, 1, 12, 57);
    ForNotePlayedOn(capo2, 1, 0, 47, 48, 1, &m);
    at("X: known, capo", m, 1, 3, 48);
    ms.clear();
    nbn::hint::ForChord(gtr, a5f, a5n, {{45, 53}, {5}, 1, 2}, &ms);
    at("X: A5 finger high", ms.size() == 1 ? ms[0] : nbn::hint::Mark{}, 2, 3, 53);

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
