// nbn_detector_test: runs the C++ NoteTracker on a 48 kHz mono 16-bit WAV and prints the events in
// the same format as `wait_sim.py analyze`, so the two outputs can be compared line by line.
// Usage: nbn_detector_test.exe recordings\take1.wav
//        nbn_detector_test.exe --chord 40,47,52 <file.wav>   (chord checks, like `wait_sim.py chord`)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "detector.h"

int main(int argc, char** argv) {
    std::vector<int> chord;
    if (argc >= 4 && std::string(argv[1]) == "--chord") {
        for (const char* p = argv[2]; *p;) {
            char* end;
            chord.push_back((int)std::strtol(p, &end, 10));
            if (end == p) return 1;
            p = *end == ',' ? end + 1 : end;
        }
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) { std::printf("usage: nbn_detector_test [--chord 40,47,52] <file.wav>\n"); return 1; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);

    // Find the "data" chunk (our recordings are plain PCM WAV written by Python's wave module).
    size_t pos = 12, dataOff = 0, dataLen = 0;
    while (pos + 8 <= data.size()) {
        uint32_t len;
        std::memcpy(&len, &data[pos + 4], 4);
        if (std::memcmp(&data[pos], "data", 4) == 0) { dataOff = pos + 8; dataLen = len; break; }
        pos += 8 + len;
    }
    if (!dataOff) { std::printf("no data chunk\n"); return 1; }
    const size_t count = dataLen / 2;
    std::vector<float> x(count);
    for (size_t i = 0; i < count; ++i) {
        int16_t s;
        std::memcpy(&s, &data[dataOff + 2 * i], 2);
        x[i] = s / 32768.0f;
    }

    if (!chord.empty()) {
        std::string names;
        for (int m : chord) names += (names.empty() ? "" : " ") + nbn::MidiName(m);
        std::printf("Chord: %s\n", names.c_str());
        nbn::ChordDetector det;
        int matches = 0;
        for (size_t i = 0; i + nbn::NoteTracker::kBlock <= count; i += nbn::NoteTracker::kBlock) {
            nbn::ChordResult r;
            if (det.Process(&x[i], chord, &r)) {
                matches += r.match;
                std::printf("%s\n", r.Describe().c_str());
            }
        }
        std::printf("%d matches\n", matches);
        return 0;
    }

    nbn::NoteTracker tr{nbn::TrackerConfig{}};
    int events = 0;
    for (size_t i = 0; i + nbn::NoteTracker::kBlock <= count; i += nbn::NoteTracker::kBlock) {
        nbn::NoteEvent ev;
        if (tr.Process(&x[i], &ev)) {
            ++events;
            std::printf("%7.2fs  %4s  %7.1f Hz  %+4.0f cents  level %6.1f dB  aper %.2f%s\n", ev.time,
                        nbn::MidiName(ev.midi).c_str(), ev.freq, ev.cents, ev.levelDb, ev.aperiodicity,
                        ev.attack ? "  (attack)" : "");
        }
    }
    std::printf("%d events\n", events);
    return 0;
}
