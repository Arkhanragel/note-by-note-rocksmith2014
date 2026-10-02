// nbn_stringid_test: checks the string identification (stringid.h).
//
//   nbn_stringid_test                 synthetic notes with a known B: the measure must find it, and a
//                                     calibration + identification must pick the right string
//   nbn_stringid_test <take folder>   a take recorded by tools/detector/string_id.py record: calibrates
//                                     on one round's open strings, identifies the other round's notes
//                                     (as in the game: every spot up to fret 22 is a candidate)
//   nbn_stringid_test <take> --dump   one line per pluck (file, time, log10 B), to compare with Python
//
// Exit code 1 on failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "stringid.h"

namespace fs = std::filesystem;
namespace sid = nbn::stringid;

namespace {

const int kStd[6] = {40, 45, 50, 55, 59, 64};

// Mono 16-bit 48 kHz WAV (as written by string_id.py).
bool LoadWav(const fs::path& p, std::vector<double>* out) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 44) return false;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {  // walk the chunks to "data"
        const uint32_t len = *(const uint32_t*)&d[pos + 4];
        if (std::string(&d[pos], 4) == "data") {
            const size_t n = std::min<size_t>(len, d.size() - pos - 8) / 2;
            out->resize(n);
            for (size_t i = 0; i < n; ++i) (*out)[i] = *(const int16_t*)&d[pos + 8 + 2 * i] / 32768.0;
            return true;
        }
        pos += 8 + len;
    }
    return false;
}

// The few fields we need from a take's .json (a flat object written by json.dumps).
struct Take {
    std::string file;
    int round = 0, midi = 0, string = 0, fret = 0;
    std::vector<double> plucks, freqs;
};

std::string After(const std::string& s, const std::string& key) {
    const size_t p = s.find("\"" + key + "\"");
    return p == std::string::npos ? "" : s.substr(s.find(':', p) + 1);
}
int Int(const std::string& s, const std::string& key) { return std::atoi(After(s, key).c_str()); }
std::vector<double> List(const std::string& s, const std::string& key) {
    std::string a = After(s, key);
    a = a.substr(a.find('[') + 1, a.find(']') - a.find('[') - 1);
    for (char& c : a) if (c == ',') c = ' ';
    std::istringstream in(a);
    std::vector<double> v;
    for (double x; in >> x;) v.push_back(x);
    return v;
}

struct Pluck {
    Take* take;
    double t;
    sid::Measure m;
};

// The window the mod analyses: from kStartAfter after the event, kLength long, cut before the next pluck.
sid::Measure Measure(const std::vector<double>& x, double t, double next, double f0) {
    const long a = (long)((t + sid::kStartAfter) * sid::kSr);
    long e = (long)(std::min(t + sid::kStartAfter + sid::kLength, next - 0.01) * sid::kSr);
    e = std::min<long>(e, (long)x.size());
    if (e - a < (long)(sid::kMinLength * sid::kSr)) return {};
    return sid::Analyze(&x[a], (int)(e - a), f0);
}

std::vector<std::pair<int, int>> Candidates(int midi) {
    std::vector<std::pair<int, int>> c;
    for (int s = 0; s < 6; ++s)
        if (midi - kStd[s] >= 0 && midi - kStd[s] <= 22) c.push_back({s, midi - kStd[s]});
    return c;
}

int RunTake(const fs::path& dir, bool dump) {
    std::vector<Take> takes;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json" || e.path().filename().string().rfind("r", 0) != 0) continue;
        std::ifstream f(e.path());
        const std::string s((std::istreambuf_iterator<char>(f)), {});
        Take t;
        t.file = e.path().stem().string() + ".wav";
        t.round = Int(s, "round");
        t.midi = Int(s, "midi");
        t.string = Int(s, "string");
        t.fret = Int(s, "fret");
        t.plucks = List(s, "plucks");
        t.freqs = List(s, "freqs");
        takes.push_back(t);
    }
    std::sort(takes.begin(), takes.end(), [](const Take& a, const Take& b) { return a.file < b.file; });
    std::vector<Pluck> plucks;
    for (auto& t : takes) {
        std::vector<double> x;
        if (!LoadWav(dir / t.file, &x)) { std::printf("can't read %s\n", t.file.c_str()); return 1; }
        for (size_t i = 0; i < t.plucks.size() && i < t.freqs.size(); ++i) {
            const double next = i + 1 < t.plucks.size() ? t.plucks[i + 1] : x.size() / (double)sid::kSr;
            plucks.push_back({&t, t.plucks[i], Measure(x, t.plucks[i], next, t.freqs[i])});
            if (dump) std::printf("%s %.3f %s\n", t.file.c_str(), t.plucks[i], plucks.back().m.ok ? std::to_string(plucks.back().m.logB).c_str() : "nan");
        }
    }
    if (dump) return 0;
    int total = 0, answered = 0, right = 0;
    for (int train = 1; train <= 2; ++train) {
        sid::Calibration cal;
        for (int s = 0; s < 6; ++s) {
            std::vector<double> v;
            for (const auto& p : plucks)
                if (p.take->round == train && p.take->fret == 0 && p.take->string == s && p.m.ok) v.push_back(p.m.logB);
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            cal.has[s] = true;
            cal.midi[s] = kStd[s];
            cal.logB[s] = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
        }
        const std::string why = sid::CheckCalibration(cal);
        std::printf("round %d calibration: %s  %s\n", train, cal.ToString().c_str(), why.empty() ? "(clean)" : why.c_str());
        for (const auto& p : plucks) {
            if (p.take->round == train) continue;
            const auto cands = Candidates(p.take->midi);
            if (cands.size() < 2) continue;
            ++total;
            if (!p.m.ok || !why.empty()) continue;
            const sid::Guess g = sid::Identify(cal, kStd, p.m.logB, cands);
            if (!g.sure) continue;
            ++answered;
            right += g.string == p.take->string && g.fret == p.take->fret;
        }
    }
    std::printf("%d plucks: sure about %d (%.0f%%), right %d (%.1f%% of those)\n", total, answered,
                100.0 * answered / std::max(1, total), right, 100.0 * right / std::max(1, answered));
    return 0;
}

// A plucked-string-like tone with inharmonicity b (1/k amplitudes, higher overtones decay faster).
std::vector<double> Synth(double f0, double b, double seconds, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ph(0, 6.283);
    std::normal_distribution<double> noise(0, 1e-4);
    std::vector<double> x((size_t)(seconds * sid::kSr));
    for (int k = 1; k < 200; ++k) {
        const double fk = k * f0 * std::sqrt(1 + b * k * k);
        if (fk > 6000) break;
        const double p = ph(rng), amp = 0.3 / k, decay = 1 + 0.3 * k * f0 / 200;
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = i / (double)sid::kSr;
            x[i] += amp * std::exp(-decay * t) * std::sin(6.283185307 * fk * t + p);
        }
    }
    for (double& v : x) v += noise(rng);
    return x;
}

int RunSynthetic() {
    int fails = 0;
    auto check = [&](const char* what, bool ok, const std::string& detail) {
        fails += !ok;
        std::printf("%s  %-36s %s\n", ok ? "ok  " : "FAIL", what, detail.c_str());
    };
    // 1. The measure finds the B that was put in.
    const double bs[] = {2.5e-5, 6e-5, 1.2e-4, 3e-4};
    for (double b : bs) {
        const auto x = Synth(196.0, b, sid::kLength, 7);
        const sid::Measure m = sid::Analyze(x.data(), (int)x.size(), 196.0);
        char d[96];
        std::snprintf(d, sizeof(d), "B %.1e -> log10 %.3f (wanted %.3f), %d overtones", b, m.logB, std::log10(b), m.partials);
        check("measure B", m.ok && std::fabs(m.logB - std::log10(b)) < 0.05, d);
    }
    // 2. Calibrate on synthetic open strings, then identify E4 played at each of its spots.
    const double bOpen[6] = {8e-5, 6e-5, 5e-5, 1.2e-4, 6e-5, 2.5e-5};
    sid::Calibration cal;
    for (int s = 0; s < 6; ++s) {
        const double f0 = 440 * std::pow(2.0, (kStd[s] - 69) / 12.0);
        const auto x = Synth(f0, bOpen[s], sid::kLength, 11 + s);
        const sid::Measure m = sid::Analyze(x.data(), (int)x.size(), f0);
        cal.has[s] = m.ok;
        cal.midi[s] = kStd[s];
        cal.logB[s] = m.logB;
    }
    check("calibration complete", cal.Complete(), cal.ToString());
    check("calibration round trip", sid::Calibration::FromString(cal.ToString()).ToString() == cal.ToString(), "");
    for (int s = 2; s < 6; ++s) {
        const int fret = 64 - kStd[s];
        const double f0 = 440 * std::pow(2.0, (64 - 69) / 12.0);
        const auto x = Synth(f0, bOpen[s] * std::pow(2.0, fret / 6.0), sid::kLength, 31 + s);
        const sid::Measure m = sid::Analyze(x.data(), (int)x.size(), f0);
        const sid::Guess g = sid::Identify(cal, kStd, m.logB, Candidates(64));
        char d[96];
        std::snprintf(d, sizeof(d), "E4 on string %d fret %d -> string %d fret %d (dist %.2f, margin %.2f)", s, fret, g.string,
                      g.fret, g.dist, g.margin);
        check("identify E4", g.sure && g.string == s && g.fret == fret, d);
    }
    // 3. Tuned down a semitone (Eb standard): the open strings' predicted B grows a little.
    check("tuning correction", sid::OpenLogB(cal, 0, 39) > cal.logB[0] && sid::OpenLogB(cal, 0, 39) - cal.logB[0] < 0.06, "");
    // 4. A processed signal (wound strings far too harmonic) is refused.
    sid::Calibration processed = cal;
    processed.logB[1] = -5.9;
    check("processed signal refused", !sid::CheckCalibration(processed).empty(), sid::CheckCalibration(processed));
    check("clean signal accepted", sid::CheckCalibration(cal).empty(), "");
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) return RunTake(argv[1], argc >= 3 && std::string(argv[2]) == "--dump");
    return RunSynthetic();
}
