// detector.cpp: see detector.h. Line-by-line port of tools/detector/pitch.py and tracker.py.
#include "detector.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>

namespace nbn {

namespace {

// In-place iterative radix-2 FFT (n must be a power of two). inverse=true computes the unscaled
// inverse transform (the caller divides by n).
void Fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {  // bit-reversal permutation
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * 3.14159265358979323846 / (double)len * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double HzToMidi(double f) { return 69.0 + 12.0 * std::log2(f / 440.0); }

}  // namespace

std::string MidiName(int m) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((m % 12) + 12) % 12]) + std::to_string(m / 12 - 1);
}

// YIN, as in pitch.py (see the comments there for the math): (2) difference function via FFT
// cross-correlation, (3) cumulative mean normalization, (4) first dip under the threshold plus
// the octave-error guard, (5) parabolic interpolation.
PitchResult Yin(const double* x, int W, int sr, double fmin, double fmax, double threshold) {
    PitchResult res;
    const int tauMin = std::max(2, (int)(sr / fmax));
    const int tauMax = std::min(W / 2, (int)(sr / fmin));
    const int N = W - tauMax;

    size_t nfft = 1;
    while (nfft < (size_t)(W + N - 1)) nfft <<= 1;
    std::vector<std::complex<double>> X(nfft), Y(nfft);
    for (int i = 0; i < W; ++i) X[i] = x[i];
    for (int i = 0; i < N; ++i) Y[i] = x[i];
    Fft(X, false);
    Fft(Y, false);
    for (size_t i = 0; i < nfft; ++i) X[i] *= std::conj(Y[i]);
    Fft(X, true);  // X[tau].real()/nfft = sum_j x[j] * x[j+tau]

    std::vector<double> sq(W + 1, 0.0);  // prefix sums of energy
    for (int i = 0; i < W; ++i) sq[i + 1] = sq[i] + x[i] * x[i];
    const double e0 = sq[N] - sq[0];
    std::vector<double> d(tauMax + 1), dn(tauMax + 1, 1.0);
    for (int tau = 0; tau <= tauMax; ++tau) {
        const double r = X[tau].real() / (double)nfft;
        d[tau] = e0 + (sq[tau + N] - sq[tau]) - 2.0 * r;
    }
    double cum = 0;
    for (int tau = 1; tau <= tauMax; ++tau) {
        cum += d[tau];
        dn[tau] = d[tau] * tau / std::max(cum, 1e-12);
    }

    int tau = -1;
    for (int t = tauMin; t < tauMax; ++t)
        if (dn[t] < threshold) { tau = t; break; }
    if (tau < 0) return res;  // unvoiced
    while (tau + 1 < tauMax && dn[tau + 1] < dn[tau]) ++tau;

    // Octave-error guard: a weak dip whose double lag has a much deeper dip -> take the double lag.
    int t2 = 2 * tau;
    if (dn[tau] > 0.05 && t2 + 2 < tauMax) {
        const int lo = (int)(t2 * 0.97), hi = (int)(t2 * 1.03) + 1;
        int best = lo;
        for (int t = lo; t < hi; ++t) if (dn[t] < dn[best]) best = t;
        if (dn[best] < 0.5 * dn[tau]) tau = best;
    }

    double shift = 0;
    if (tau >= 1 && tau < tauMax - 1) {
        const double a = dn[tau - 1], b = dn[tau], c = dn[tau + 1];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) shift = 0.5 * (a - c) / den;
    }
    res.ok = true;
    res.freq = sr / (tau + shift);
    res.midi = HzToMidi(res.freq);
    res.aperiodicity = dn[tau];
    return res;
}

OnsetDetector::OnsetDetector(int sr, double ratio, double gateDb, double spanMs, double lookbackMs, double refractoryMs)
    : ratio_(ratio), gate_(std::pow(10.0, gateDb / 10.0)), lookbackMs_(lookbackMs), refractoryMs_(refractoryMs),
      span_((int)(sr * spanMs / 1000.0)) {}

bool OnsetDetector::Process(const double* window, int W, double nowMs) {
    double e = 0;
    for (int i = W - span_; i < W; ++i) e += window[i] * window[i];
    e /= span_;
    while (!hist_.empty() && nowMs - hist_.front().first > lookbackMs_) hist_.pop_front();
    double ref = e;
    if (!hist_.empty()) {
        ref = hist_.front().second;
        for (const auto& h : hist_) ref = std::min(ref, h.second);
    }
    hist_.emplace_back(nowMs, e);
    if (e > gate_ && e > ratio_ * std::max(ref, 1e-12) && nowMs - lastOnsetMs_ > refractoryMs_) {
        lastOnsetMs_ = nowMs;
        return true;
    }
    return false;
}

NoteTracker::NoteTracker(const TrackerConfig& cfg)
    : cfg_(cfg), buf_(cfg.window, 0.0), onset_(cfg.sr, cfg.onsetRatio, cfg.gateDb) {}

bool NoteTracker::Process(const float* block, NoteEvent* ev) {
    const int n = kBlock, W = cfg_.window;
    std::move(buf_.begin() + n, buf_.end(), buf_.begin());  // slide the window
    for (int i = 0; i < n; ++i) buf_[W - n + i] = block[i];
    samples_ += n;

    double e = 0;  // level over the last 1024 samples (~21 ms, longer than one low-E period)
    for (int i = W - 1024; i < W; ++i) e += buf_[i] * buf_[i];
    const double lvl = 10.0 * std::log10(e / 1024.0 + 1e-12);

    if (onset_.Process(buf_.data(), W, Now() * 1000.0)) {
        onsetPending_ = true;
        peakDb_ = lvl;
    }
    peakDb_ = std::max(peakDb_, lvl);

    const bool voiced = lvl > cfg_.gateDb && lvl > peakDb_ - cfg_.relGateDb;
    PitchResult p;
    if (voiced) p = Yin(buf_.data(), W, cfg_.sr, cfg_.fmin, cfg_.fmax, cfg_.threshold);
    const int m = p.ok ? (int)std::lround(p.midi) : -1;

    stable_ = (m >= 0 && m == cur_) ? stable_ + 1 : (m >= 0 ? 1 : 0);
    cur_ = m;
    if (m < 0) {
        reported_ = -1;  // a gap lets the same note be reported again
        return false;
    }

    const int needed = onsetPending_ ? cfg_.stable : cfg_.stableLegato;
    const bool tooSoon = m == lastEventMidi_ && Now() - lastEventTime_ < 0.12;
    if (stable_ >= needed && (m != reported_ || onsetPending_) && !tooSoon) {
        ev->time = Now();
        ev->midi = m;
        ev->freq = p.freq;
        ev->cents = (p.midi - m) * 100.0;
        ev->levelDb = lvl;
        ev->aperiodicity = p.aperiodicity;
        ev->attack = onsetPending_;
        reported_ = m;
        onsetPending_ = false;
        lastEventMidi_ = m;
        lastEventTime_ = Now();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ chords (port of chord.py)
namespace {

const char* kPcNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

double MidiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

struct Peaks {
    std::vector<double> freq, amp, log2f;
};

// Local maxima of the Hann-windowed, zero-padded magnitude spectrum (parabolic interpolation on
// the log magnitude), as spectral_peaks() in chord.py.
Peaks SpectralPeaks(const double* x, int n, const ChordConfig& c) {
    std::vector<std::complex<double>> a(c.nfft);
    const double kPi = 3.14159265358979323846;
    for (int i = 0; i < n; ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1)));  // np.hanning
    Fft(a, false);
    const int bins = c.nfft / 2 + 1;
    std::vector<double> mag(bins);
    for (int k = 0; k < bins; ++k) mag[k] = std::abs(a[k]);
    const double hz = (double)c.sr / c.nfft;
    Peaks p;
    for (int k = 1; k < bins - 1; ++k) {
        if (!(mag[k] > mag[k - 1] && mag[k] >= mag[k + 1]) || k * hz < c.fmin || k * hz > c.fmax) continue;
        const double la = std::log(mag[k - 1] + 1e-12), lb = std::log(mag[k] + 1e-12), lc = std::log(mag[k + 1] + 1e-12);
        const double den = la - 2 * lb + lc;
        const double shift = std::fabs(den) > 1e-12 ? 0.5 * (la - lc) / den : 0.0;
        p.freq.push_back((k + shift) * hz);
        p.amp.push_back(std::exp(lb - 0.25 * (la - lc) * shift));
    }
    if (p.amp.empty()) return p;
    const double floor = *std::max_element(p.amp.begin(), p.amp.end()) * std::pow(10.0, -c.peakFloorDb / 20);
    Peaks kept;
    for (size_t i = 0; i < p.amp.size(); ++i)
        if (p.amp[i] >= floor) {
            kept.freq.push_back(p.freq[i]);
            kept.amp.push_back(p.amp[i]);
            kept.log2f.push_back(std::log2(p.freq[i]));
        }
    return kept;
}

struct Harm {
    int h, peak;
    double amp;
};

std::vector<Harm> Harmonics(double f0, const Peaks& p, const ChordConfig& c) {
    std::vector<Harm> out;
    const double tol = c.tolCents / 1200;
    for (int h = 1; h <= c.maxHarm; ++h) {
        const double target = h * f0;
        if (target > c.fmax) break;
        const double lt = std::log2(target);
        int best = -1;
        for (size_t i = 0; i < p.freq.size(); ++i)
            if (std::fabs(p.log2f[i] - lt) < tol && (best < 0 || p.amp[i] > p.amp[best])) best = (int)i;
        if (best >= 0) out.push_back({h, best, p.amp[best]});
    }
    return out;
}

double Salience(double f0, const std::vector<Harm>& harm, const std::vector<char>& claimed, double fundMin) {
    if (harm.empty() || harm[0].h != 1 || claimed[harm[0].peak] || harm[0].amp < fundMin) return 0.0;
    double s = 0;
    for (const auto& h : harm)
        if (!claimed[h.peak]) s += (f0 + 27) / (h.h * f0 + 320) * h.amp;
    return s;
}

}  // namespace

std::vector<std::pair<int, double>> AnalyzeChord(const double* x, int n, int lo, int hi, const ChordConfig& c) {
    static const int kHarmonicIntervals[] = {36, 31, 28, 24, 19, 12};  // harmonics 8, 6, 5, 4, 3, 2
    const Peaks p = SpectralPeaks(x, n, c);
    std::vector<std::vector<Harm>> harm;
    for (int m = lo; m <= hi; ++m) harm.push_back(Harmonics(MidiHz(m), p, c));
    const double fundMin = (p.amp.empty() ? 0.0 : *std::max_element(p.amp.begin(), p.amp.end())) *
                           std::pow(10.0, -c.fundFloorDb / 20);
    std::vector<char> claimed(p.amp.size(), 0);
    std::vector<std::pair<int, double>> heard;
    std::vector<double> sal(hi - lo + 1);
    double first = 0;
    while ((int)heard.size() < c.maxNotes) {
        int best = -1;
        for (int m = lo; m <= hi; ++m) {
            const bool done = std::any_of(heard.begin(), heard.end(), [&](const auto& h) { return h.first == m; });
            sal[m - lo] = done ? 0.0 : Salience(MidiHz(m), harm[m - lo], claimed, fundMin);
            if (!done && (best < 0 || sal[m - lo] > sal[best - lo])) best = m;
        }
        if (best < 0 || sal[best - lo] <= 0 || sal[best - lo] < c.stopRel * first) break;
        for (int down : kHarmonicIntervals) {
            const int low = best - down;
            if (low >= lo && sal[low - lo] >= c.octaveRel * sal[best - lo]) { best = low; break; }
        }
        if (first == 0) first = sal[best - lo];
        heard.emplace_back(best, sal[best - lo]);
        for (const auto& h : harm[best - lo]) claimed[h.peak] = 1;
    }
    return heard;
}

std::string ChordResult::Describe() const {
    char buf[512];
    std::string h, ex;
    for (const auto& n : heard) h += (h.empty() ? "" : " ") + MidiName(n.first);
    for (int pc : extra) ex += (ex.empty() ? "" : " ") + std::string(kPcNames[pc]);
    std::snprintf(buf, sizeof(buf), "%7.2fs  chord %s  heard [%s]  %d/%d chord notes%s%s  level %6.1f dB", time,
                  match ? "MATCH" : "no   ", h.empty() ? "-" : h.c_str(), hits, needed, ex.empty() ? "" : ", wrong: ",
                  ex.c_str(), levelDb);
    return buf;
}

ChordDetector::ChordDetector(const ChordConfig& cfg)
    : cfg_(cfg), buf_(cfg.window, 0.0), onset_(cfg.sr, 2.0, cfg.gateDb) {}

bool ChordDetector::Process(const float* block, const std::vector<int>& chord, ChordResult* res) {
    const int n = NoteTracker::kBlock, W = cfg_.window;
    std::move(buf_.begin() + n, buf_.end(), buf_.begin());
    for (int i = 0; i < n; ++i) buf_[W - n + i] = block[i];
    samples_ += n;
    if (onset_.Process(buf_.data(), W, Now() * 1000.0)) {
        due_.clear();
        for (double d : cfg_.delays) due_.push_back(samples_ + (long long)(d * cfg_.sr));
    }
    if (due_.empty() || samples_ < due_.front()) return false;
    due_.pop_front();
    if (chord.empty()) return false;

    *res = ChordResult();
    double e = 0;
    for (double v : buf_) e += v * v;
    res->time = Now();
    res->levelDb = 10.0 * std::log10(e / W + 1e-12);
    if (res->levelDb < cfg_.gateDb) return true;
    const int lo = std::max(23, *std::min_element(chord.begin(), chord.end()) - 7);
    const int hi = std::min(100, *std::max_element(chord.begin(), chord.end()) + 12);
    res->heard = AnalyzeChord(buf_.data(), W, lo, hi, cfg_);

    // Judge by pitch class (judge() in chord.py).
    bool want[12] = {};
    int wantCount = 0;
    for (int m : chord) if (!want[m % 12]) { want[m % 12] = true; ++wantCount; }
    res->needed = wantCount <= 2 ? wantCount : wantCount - 1;  // power chords: both; bigger: all but one
    if (res->heard.empty()) return true;
    bool got[12] = {}, isExtra[12] = {};
    const double top = res->heard[0].second;
    for (const auto& h : res->heard) {
        const int pc = h.first % 12;
        if (want[pc] && !got[pc]) { got[pc] = true; ++res->hits; }
        if (!want[pc] && h.second >= cfg_.extraRel * top) isExtra[pc] = true;
    }
    for (int pc = 0; pc < 12; ++pc) if (isExtra[pc]) res->extra.push_back(pc);
    res->match = want[res->heard[0].first % 12] && res->hits >= res->needed && res->extra.empty();
    return true;
}

}  // namespace nbn
