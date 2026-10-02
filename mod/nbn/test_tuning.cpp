// nbn_tuning_test: checks the tuning check (tuning.h): the steady pitch of a note, when a string or the
// whole guitar is called out of tune (and when it isn't: a player's wrong frets), and the words.
// Exit code 1 on failure.
// With WAV files (48 kHz mono 16-bit, like NoteByNote_debug\wait_*.wav): prints each note's steady
// pitch, and how far the notes are from the nearest half step overall (a guitar in tune: about 0).
//   nbn_tuning_test.exe --wav a.wav b.wav ...
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "detector.h"
#include "tuning.h"

namespace {

std::vector<float> ReadWav(const char* path) {
    std::vector<float> x;
    FILE* f = std::fopen(path, "rb");
    if (!f) return x;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        uint32_t len;
        std::memcpy(&len, &data[pos + 4], 4);
        if (std::memcmp(&data[pos], "data", 4) == 0) {
            const size_t count = std::min<size_t>(len, data.size() - pos - 8) / 2;
            x.resize(count);
            for (size_t i = 0; i < count; ++i) {
                int16_t s;
                std::memcpy(&s, &data[pos + 8 + 2 * i], 2);
                x[i] = s / 32768.0f;
            }
            break;
        }
        pos += 8 + len;
    }
    return x;
}

int Wavs(int argc, char** argv) {
    std::vector<double> all, early;
    for (int a = 2; a < argc; ++a) {
        const std::vector<float> x = ReadWav(argv[a]);
        nbn::NoteTracker tr{nbn::TrackerConfig{}};
        nbn::tuning::SteadyPitch sp;
        double eventCents = 0;
        for (size_t i = 0; i + nbn::NoteTracker::kBlock <= x.size(); i += nbn::NoteTracker::kBlock) {
            nbn::NoteEvent ev;
            const bool got = tr.Process(&x[i], &ev);
            double pitch;
            // A new note ends the one before; else this block's pitch.
            if (got ? sp.Stop(&pitch) : sp.Frame(tr.FramePitch(), &pitch)) {
                const double c = (pitch - std::lround(pitch)) * 100.0;
                all.push_back(c);
                early.push_back(eventCents);
                std::printf("  %-40s %4s steady %+4.0f cents (at the event %+4.0f)\n", argv[a], nbn::MidiName(sp.Midi()).c_str(), c,
                            eventCents);
            }
            if (got) {
                sp.Start(ev.midi);
                eventCents = ev.cents;
            }
        }
    }
    if (all.empty()) { std::printf("no notes\n"); return 0; }
    auto stats = [](std::vector<double> v, const char* what) {
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        std::printf("%-22s %zu notes: median %+.1f cents, middle half %+.1f .. %+.1f, 90%% within %+.1f .. %+.1f\n", what, n,
                    v[n / 2], v[n / 4], v[3 * n / 4], v[n / 20], v[n - 1 - n / 20]);
    };
    stats(all, "steady pitch:");
    stats(early, "at the note event:");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--wav") return Wavs(argc, argv);
    using nbn::tuning::Check;
    using nbn::tuning::Finding;
    int fails = 0;
    auto expect = [&](const char* what, const std::string& got, const std::string& want) {
        const bool ok = got == want;
        fails += !ok;
        std::printf("%s  %-42s %s%s\n", ok ? "ok  " : "FAIL", what, got.c_str(), ok ? "" : ("   (wanted " + want + ")").c_str());
    };
    auto show = [](const Finding& f) {
        if (f.string < 0 && !f.all) return std::string("in tune");
        char b[96];
        std::snprintf(b, sizeof(b), "%s off %+.2f (%d half steps)", f.all ? "all" : ("string " + std::to_string(f.string)).c_str(),
                      f.offset, f.steps);
        return std::string(b);
    };

    // Steady pitch: a sharp attack, then the note; the median of ~50-400 ms.
    {
        nbn::tuning::SteadyPitch sp;
        sp.Start(45);
        double p = 0;
        bool done = false;
        for (int b = 0; b < 100 && !done; ++b) done = sp.Frame(b < 8 ? 45.3 : 44.75 + 0.002 * (b % 5), &p);
        char got[32];
        std::snprintf(got, sizeof(got), "%.2f", p);
        expect("steady pitch (attack left out)", done ? got : "none", "44.75");
        sp.Start(45);
        done = false;
        for (int b = 0; b < 100 && !done; ++b) done = sp.Frame(b < 10 ? 45.0 : -1, &p);
        expect("a short note: not measured", done ? "measured" : "none", "none");
    }

    // A string a little flat (35 cents): its notes, a few frets apart.
    {
        Check c;
        for (int i = 0; i < 5; ++i) c.Add(1, 47 + i, 47 + i - 0.35 + 0.02 * (i % 2));
        expect("string 1 a bit low", show(c.Get()), "string 1 off -0.35 (0 half steps)");
        for (int i = 0; i < 6; ++i) c.Add(1, 47, 47.01);  // tuned: the next notes are right
        expect("tuned again", show(c.Get()), "in tune");
    }
    // In tune, with the usual spread, and wrong frets now and then.
    {
        Check c;
        const double spread[8] = {0.05, -0.08, 0.1, -0.03, 0.0, 0.07, -0.1, 0.02};
        for (int i = 0; i < 8; ++i) c.Add(2, 52 + i % 3, 52 + i % 3 + spread[i] + (i == 3 ? 1 : 0));
        expect("in tune, a wrong fret", show(c.Get()), "in tune");
    }
    // A hand one fret too low for a while, then corrected: not the tuning.
    {
        Check c;
        for (int i = 0; i < 4; ++i) c.Add(3, 57 + i, 56 + i + 0.02);
        c.Add(3, 61, 61.0);
        expect("hand off a fret, then right", show(c.Get()), "in tune");
        Check d;
        for (int i = 0; i < 4; ++i) d.Add(3, 57, 56.02);
        expect("the same note four times", show(d.Get()), "in tune");
    }
    // A string a half step low: every note.
    {
        Check c;
        for (int i = 0; i < 5; ++i) c.Add(0, 40 + i, 39 + i + 0.04);
        expect("string 0 a half step low", show(c.Get()), "string 0 off -0.96 (-1 half steps)");
    }
    // A string tuned down by ear, between half steps: the same open string again and again (user test:
    // 142 cents low), and the wrong note it gives.
    {
        Check c;
        for (int i = 0; i < 4; ++i) c.Add(5, 64, 62.58 + 0.01 * i);
        expect("open string 1.4 half steps low", show(c.Get()), "string 5 off -1.41 (-1 half steps)");
        nbn::hint::Neck n;
        expect("its words", nbn::tuning::AdviceText(c.Get(), n),
               "Out of tune?  -  string 1 (e) sounds more than a half step low. This song is in E standard: tune it up, to E");
        Check d;
        for (int i = 0; i < 4; ++i) d.Add(5, 64, 63.02);  // a whole half step on one note: maybe the B string's fret 4
        expect("the same note a half step low", show(d.Get()), "in tune");
    }
    // The whole guitar a half step high (tuned to E standard, the song is in Eb).
    {
        Check c;
        for (int i = 0; i < 4; ++i) c.Add(0, 39 + i, 40 + i);
        for (int i = 0; i < 4; ++i) c.Add(2, 49 + i, 50 + i + 0.04);
        expect("the whole guitar a half step high", show(c.Get()), "all off +1.02 (1 half steps)");
        c.Add(4, 58, 58.0);  // (one string known right isn't enough to stop it: it needs 4 notes)
        for (int i = 0; i < 4; ++i) c.Add(4, 58, 58.0);
        expect("one string right: the worst string", show(c.Get()), "string 2 off +1.04 (1 half steps)");
    }

    // Words.
    {
        nbn::hint::Neck standard;
        const int eb[6] = {39, 44, 49, 54, 58, 63}, dropD[6] = {38, 45, 50, 55, 59, 64}, dadgad[6] = {38, 45, 50, 55, 57, 62};
        const int bassDropD[6] = {26, 33, 38, 43, 0, 0};
        expect("E standard", nbn::tuning::TuningName(standard.open, 6), "E standard");
        expect("Eb standard", nbn::tuning::TuningName(eb, 6), "Eb standard");
        expect("Drop D", nbn::tuning::TuningName(dropD, 6), "Drop D");
        expect("DADGAD", nbn::tuning::TuningName(dadgad, 6), "D A D G A D");
        expect("bass Drop D", nbn::tuning::TuningName(bassDropD, 4), "Drop D");
        expect("a string a bit low", nbn::tuning::AdviceText({4, false, -0.35, 0}, standard),
               "Out of tune?  -  string 2 (B) sounds a bit low. Tune it up a little, to B");
        nbn::hint::Neck n;
        std::copy(eb, eb + 6, n.open);
        expect("the guitar for another song", nbn::tuning::AdviceText({-1, true, 1.0, 1}, n),
               "Out of tune?  -  Your guitar sounds a half step high. This song is in Eb standard: tune every string down a half step (Eb Ab Db Gb Bb Eb)");
        std::copy(dropD, dropD + 6, n.open);
        n.fromThick = true;
        expect("drop D, the string not dropped", nbn::tuning::AdviceText({0, false, 2.0, 2}, n),
               "Out of tune?  -  string 1 (E) sounds a whole step high. This song is in Drop D: tune it down a whole step, to D");
    }

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
