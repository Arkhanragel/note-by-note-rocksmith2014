// stringid.cpp: see stringid.h. Line-by-line port of partials() / fit_inharmonic() in
// tools/detector/string_id.py; keep both in sync.
#include "stringid.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <sstream>

#include "detector.h"

namespace nbn::stringid {
namespace {

constexpr int kNfft = 1 << 17;      // zero-padded FFT: 0.37 Hz per bin at 48 kHz
constexpr double kMaxHz = 7000.0;   // overtones searched up to here
constexpr double kFloorDb = -60.0;  // overtones weaker than the strongest peak - 60 dB are ignored
// B needs an overtone at least this high (stretch ~ B k^2). 8 lost dull notes (high frets on wound
// strings: 5-7 overtones) whose B is big enough to show by overtone 6; 6 or 7: 94 % sure on take2
// instead of 92 %, same 97.4 % right.
constexpr int kMinTopK = 6;
constexpr double kPi = 3.14159265358979323846;

struct Peak {
    double freq, db, prominence;
};

double Median(std::vector<double> v) {  // as np.median: the mean of the two middle values when even
    const size_t n = v.size(), h = n / 2;
    std::nth_element(v.begin(), v.begin() + h, v.end());
    const double hi = v[h];
    if (n % 2) return hi;
    return 0.5 * (hi + *std::max_element(v.begin(), v.begin() + h));
}

// Strongest spectrum peak within fc +- hw Hz (parabolic interpolation in dB), and how far it stands
// above the local median. false = no real peak inside the window.
bool FindPeak(const std::vector<double>& db, double bin, double fc, double hw, Peak* out) {
    const int lo = std::max(1, (int)((fc - hw) / bin));
    const int hi = std::min((int)db.size() - 2, (int)((fc + hw) / bin) + 1);
    if (hi <= lo + 4) return false;
    const int i = (int)(std::max_element(db.begin() + lo, db.begin() + hi) - db.begin());
    if (i <= lo || i >= hi - 1) return false;  // on the edge of the search window
    const double a = db[i - 1], b = db[i], c = db[i + 1];
    const double den = a - 2 * b + c;
    const double p = den < 0 ? 0.5 * (a - c) / den : 0.0;
    *out = {(i + p) * bin, b - 0.25 * (a - c) * p,
            b - Median(std::vector<double>(db.begin() + lo, db.begin() + hi))};
    return true;
}

struct Partial {
    int k;
    double freq, db;
};

// Weighted least squares of f_k = k f0 sqrt(1 + B k^2): (f_k/k)^2 = f0^2 + f0^2 B k^2 is a line in
// k^2 (weights: dB above the floor; sqrt as in the Python's row scaling).
void FitInharmonic(const std::vector<Partial>& ps, double floor, double* f0, double* b) {
    double s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    for (const auto& p : ps) {
        const double w = p.db - floor + 1;  // (sqrt(w) on both sides of the system = w in the normal equations)
        const double x = (double)p.k * p.k, y = (p.freq / p.k) * (p.freq / p.k);
        s00 += w;
        s01 += w * x;
        s11 += w * x * x;
        t0 += w * y;
        t1 += w * x * y;
    }
    const double det = s00 * s11 - s01 * s01;
    if (std::fabs(det) < 1e-30) return;
    const double c0 = (t0 * s11 - s01 * t1) / det, c1 = (s00 * t1 - s01 * t0) / det;
    *f0 = std::sqrt(std::max(c0, 1.0));
    *b = c1 / std::max(c0, 1.0);
}

}  // namespace

Measure Analyze(const double* x, int n, double f0Guess, int sr) {
    Measure m;
    if (n < 64 || f0Guess <= 0 || n > kNfft) return m;
    double energy = 0;
    for (int i = 0; i < n; ++i) energy += x[i] * x[i];
    m.levelDb = 10 * std::log10(energy / n + 1e-12);
    std::vector<std::complex<double>> a(kNfft);
    for (int i = 0; i < n; ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1)));  // np.hanning
    Fft(a, false);
    const int bins = kNfft / 2 + 1;
    std::vector<double> db(bins);
    for (int k = 0; k < bins; ++k) db[k] = 20 * std::log10(std::abs(a[k]) + 1e-12);
    const double bin = (double)sr / kNfft;
    const double floor = *std::max_element(db.begin(), db.end()) + kFloorDb;

    // Find the overtones one by one, predicting each from the fit of the ones found so far.
    std::vector<Partial> found;
    double f0 = f0Guess, b = 0;
    int misses = 0;
    for (int k = 1; k <= (int)(kMaxHz / f0Guess); ++k) {
        Peak pk{};  // (only read when FindPeak filled it)
        if (FindPeak(db, bin, k * f0 * std::sqrt(1 + std::max(b, 0.0) * k * k), 0.25 * f0, &pk) && pk.db > floor &&
            pk.prominence > 12) {
            found.push_back({k, pk.freq, pk.db});
            misses = 0;
        } else if (++misses >= 5 && k > 10) {
            break;
        }
        if (found.size() >= 5) FitInharmonic(found, floor, &f0, &b);
    }
    m.partials = (int)found.size();
    for (const auto& p : found) m.maxK = std::max(m.maxK, p.k);
    if (found.size() < 3) return m;
    // One outlier pass: drop overtones more than 10 cents away from the fitted curve, then refit.
    std::vector<Partial> kept;
    for (const auto& p : found) {
        const double model = p.k * f0 * std::sqrt(1 + std::max(b, 0.0) * p.k * p.k);
        if (std::fabs(1200 * std::log2(p.freq / model)) < 10) kept.push_back(p);
    }
    if (kept.size() >= 5) {
        found = kept;
        FitInharmonic(found, floor, &f0, &b);
    }
    m.f0 = f0;
    m.partials = (int)found.size();
    m.maxK = 0;
    for (const auto& p : found) m.maxK = std::max(m.maxK, p.k);
    // B is only measurable with high enough overtones.
    m.ok = m.maxK >= kMinTopK && b > 1e-8;
    m.unstretched = m.maxK >= kMinTopK && !m.ok;
    m.logB = m.ok ? std::log10(b) : 0;
    return m;
}

bool Calibration::Complete() const {
    for (bool h : has)
        if (!h) return false;
    return true;
}

std::string Calibration::ToString() const {
    std::string s;
    char buf[32];
    for (int i = 0; i < 6; ++i) {
        if (!has[i]) {
            s += i ? " -" : "-";
            continue;
        }
        std::snprintf(buf, sizeof(buf), "%s%d:%.3f", i ? " " : "", midi[i], logB[i]);
        s += buf;
    }
    return s;
}

Calibration Calibration::FromString(const std::string& s) {
    Calibration c;
    std::istringstream in(s);
    std::string item;
    for (int i = 0; i < 6 && in >> item; ++i) {
        int m = 0;
        double v = 0;
        if (std::sscanf(item.c_str(), "%d:%lf", &m, &v) == 2 && m > 0 && m < 128 && v < 0 && v > -9) {
            c.has[i] = true;
            c.midi[i] = m;
            c.logB[i] = v;
        }
    }
    return c;
}

std::string CheckCalibration(const Calibration& c) {
    if (!c.Complete()) return "not every string was measured";
    // Clean recordings (2026-09-26 and take2 of 2026-10-02): the wound E, A, D strings open measure
    // log10 B -3.8 .. -4.2; through a compressor / drive preset they measured -5.6 .. -6.4.
    for (int s = 0; s < 3; ++s)
        if (c.logB[s] < -4.8)
            return "the guitar signal sounds processed (compressor, drive or an EQ preset): use a plain cable or a "
                   "clean sound, then calibrate again";
    return "";
}

double OpenLogB(const Calibration& c, int string, int openMidi) {
    return c.logB[string] + 2.0 * (c.midi[string] - openMidi) / 12.0 * std::log10(2.0);
}

Guess Identify(const Calibration& c, const int open[6], double logB, const std::vector<std::pair<int, int>>& candidates,
               int handString, int handFret, bool weak) {
    struct Spot {
        int s, f;
        double d;
    };
    std::vector<Spot> spots;
    for (const auto& [s, f] : candidates) {
        if (s < 0 || s >= 6 || !c.has[s]) continue;
        // B ~ 1/L^2 and the length halves every 12 frets: +log10(2)/6 per fret.
        spots.push_back({s, f, std::fabs(logB - (OpenLogB(c, s, open[s]) + f / 6.0 * std::log10(2.0)))});
    }
    Guess g;
    if (spots.empty()) return g;
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.d < b.d; });
    g.string = spots[0].s;
    g.fret = spots[0].f;
    g.dist = spots[0].d;
    g.margin = spots.size() > 1 ? spots[1].d - spots[0].d : 1e9;
    if (g.dist > kMaxDist) return g;  // fits no spot well: a dead, bent or processed note
    const bool hand = handString >= 0 && handFret >= 0;
    // How far a spot is from the hand: frets, and 3 per string; an open string needs no fret.
    auto cost = [&](const Spot& p) {
        const int strings = 3 * std::abs(p.s - handString);
        return p.f == 0 ? strings : std::abs(p.f - handFret) + strings;
    };
    // A weak measure (low overtones only) is trusted only near the hand.
    auto reachable = [&](const Spot& p) { return !weak || (hand && cost(p) <= kWeakReach); };
    if (g.margin >= kMinMargin) {
        g.sure = reachable(spots[0]);
        return g;
    }
    // The sound fits several spots about as well: the one near the hand, if clearly nearer.
    if (!hand) return g;
    const Spot* near = nullptr;
    int nearCost = 1 << 20, nextCost = 1 << 20;
    for (const auto& p : spots) {
        if (p.d - spots[0].d >= kMinMargin) break;  // the sound tells these apart from the best
        const int k = cost(p);
        if (k < nearCost) {
            nextCost = nearCost;
            nearCost = k;
            near = &p;
        } else if (k < nextCost) {
            nextCost = k;
        }
    }
    if (!near || near->d > kMaxDist || nextCost - nearCost < kHandMargin || !reachable(*near)) return g;
    g.string = near->s;
    g.fret = near->f;
    g.dist = near->d;
    g.sure = g.byHand = true;
    return g;
}

}  // namespace nbn::stringid
