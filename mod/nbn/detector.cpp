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

}  // namespace nbn
