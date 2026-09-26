"""
Monophonic pitch detection for guitar/bass: the YIN algorithm, plus a small onset detector.

WHY YIN?
A guitar note isn't a pure sine. It has strong harmonics, and the 2nd harmonic is sometimes
louder than the fundamental, so "take the biggest FFT peak" often gives the wrong octave.
YIN (de CheveignÃ© & Kawahara, 2002) works in the time domain instead. It looks for the
smallest lag tau at which the signal closely repeats itself. That lag is the period, and
the frequency is sample_rate / tau. It's cheap, robust, and a standard choice for tuners.

The steps below follow the paper's numbering:
  (2) difference function  d(tau) = sum_j (x[j] - x[j+tau])^2
  (3) cumulative mean normalized difference d'(tau), so a threshold can be used
  (4) absolute threshold: take the FIRST dip below the threshold, not the global minimum
      (the global minimum is often at 2*period, which would be an octave error)
  (5) parabolic interpolation, for sub-sample precision
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def hz_to_midi(f: float) -> float:
    """Frequency to (fractional) MIDI note number. A4 = 440 Hz = MIDI 69."""
    return 69.0 + 12.0 * math.log2(f / 440.0)


def midi_name(m: int) -> str:
    return f"{NOTE_NAMES[m % 12]}{m // 12 - 1}"


@dataclass
class PitchResult:
    freq: float          # Hz
    midi: float          # fractional MIDI note
    aperiodicity: float  # d'(tau) at the chosen lag: 0 = perfectly periodic, closer to 1 = noise


def yin(x: np.ndarray, sr: int, fmin: float = 60.0, fmax: float = 1400.0,
        threshold: float = 0.15) -> PitchResult | None:
    """Estimate the fundamental frequency of the window x. Returns None if it's not clearly pitched."""
    W = len(x)
    tau_min = max(2, int(sr / fmax))
    tau_max = min(W // 2, int(sr / fmin))
    N = W - tau_max  # integration length: every lag compares the same number of samples

    # (2) Difference function, computed quickly:
    #   d(tau) = sum x[j]^2 + sum x[j+tau]^2 - 2 * sum x[j]*x[j+tau]      for j in [0, N)
    # The cross term is a cross-correlation, which the FFT computes in O(W log W).
    n_fft = 1 << (W + N - 1).bit_length()
    X = np.fft.rfft(x, n_fft)
    Y = np.fft.rfft(x[:N], n_fft)
    r = np.fft.irfft(X * np.conj(Y), n_fft)[: tau_max + 1]  # r[tau] = sum_j x[j] * x[j+tau]

    sq = np.concatenate(([0.0], np.cumsum(x * x)))       # prefix sums of energy
    e0 = sq[N] - sq[0]                                    # energy of x[0:N]
    taus = np.arange(tau_max + 1)
    e_tau = sq[taus + N] - sq[taus]                       # energy of x[tau:tau+N]
    d = e0 + e_tau - 2.0 * r

    # (3) Cumulative mean normalized difference. d'(0) = 1, and d'(tau) = d(tau) / mean(d[1..tau]).
    # This makes the values scale-independent (roughly 0..1+), so a fixed threshold works.
    dn = np.ones_like(d)
    cums = np.cumsum(d[1:])
    dn[1:] = d[1:] * np.arange(1, tau_max + 1) / np.maximum(cums, 1e-12)

    # (4) First lag under the threshold, then walk down to the bottom of that dip.
    below = np.nonzero(dn[tau_min:tau_max] < threshold)[0]
    if len(below) == 0:
        return None  # unvoiced: silence, pick noise, or a chord
    tau = tau_min + int(below[0])
    while tau + 1 < tau_max and dn[tau + 1] < dn[tau]:
        tau += 1

    # (4b) Octave-error guard (our addition, prompted by the first real guitar test).
    # While a low note fades, its 2nd harmonic can dominate. Then there's a weak dip at
    # period/2 that passes the threshold first, even though the true period (2*tau) has a
    # much deeper dip. If the chosen dip is weak and the one at 2*tau is clearly better,
    # take 2*tau. A clean note has dips at P, 2P, 3P... that are all similar and all tiny,
    # so the "weak" test (> 0.05) keeps this from causing octave-DOWN errors.
    t2 = 2 * tau
    if dn[tau] > 0.05 and t2 + 2 < tau_max:
        lo, hi = int(t2 * 0.97), int(t2 * 1.03) + 1
        t2 = lo + int(np.argmin(dn[lo:hi]))
        if dn[t2] < 0.5 * dn[tau]:
            tau = t2

    # (5) Fit a parabola through the 3 points around the minimum to get a fractional lag.
    if 1 <= tau < tau_max - 1:
        a, b, c = dn[tau - 1], dn[tau], dn[tau + 1]
        denom = a - 2 * b + c
        shift = 0.5 * (a - c) / denom if abs(denom) > 1e-12 else 0.0
    else:
        shift = 0.0
    f = sr / (tau + shift)
    return PitchResult(freq=f, midi=hz_to_midi(f), aperiodicity=float(dn[tau]))


class OnsetDetector:
    """
    Detects "a new note was struck" from sudden rises in loudness.

    We need this for repeated notes. If the target is F2 and the previous note was also F2,
    the old note may still be ringing and would match right away. So for a repeated pitch
    we require a fresh attack.

    Lesson from the first real test: energy must be measured over a span longer than the
    lowest note's period (low E = 12 ms, low bass E = 24 ms). A 5 ms block measures part of
    a cycle, so its energy wobbles and fires false onsets. So we measure over ~21 ms, compare
    with the QUIETEST level of the last ~60 ms (a pick jumps above it, a decay never does),
    and then ignore further onsets for a short "refractory" time.

    Refractory raised from 80 to 150 ms after the in-game test: through RS_ASIO the attack rose
    over 30-70 ms, and a second onset fired as soon as 80 ms had passed (7 duplicate events in 60 s).
    150 ms still allows about 6-7 repeated picks per second.
    """

    def __init__(self, sr: int, ratio: float = 2.0, gate_db: float = -45.0,
                 span_ms: float = 21.0, lookback_ms: float = 60.0, refractory_ms: float = 150.0):
        self.ratio = ratio                                 # energy must be ratio x the recent minimum (2.0 = +3 dB)
        self.gate = 10 ** (gate_db / 10)
        self.span = int(sr * span_ms / 1000)
        self.lookback_ms = lookback_ms
        self.refractory_ms = refractory_ms
        self.hist: list[tuple[float, float]] = []          # (time_ms, energy)
        self.last_onset_ms = -1e9

    def process(self, window: np.ndarray, now_ms: float) -> bool:
        tail = window[-self.span:]
        e = float(np.mean(tail * tail))
        self.hist = [(t, v) for t, v in self.hist if now_ms - t <= self.lookback_ms]
        ref = min((v for _, v in self.hist), default=e)
        self.hist.append((now_ms, e))
        if (e > self.gate and e > self.ratio * max(ref, 1e-12)
                and now_ms - self.last_onset_ms > self.refractory_ms):
            self.last_onset_ms = now_ms
            return True
        return False


def level_db(block: np.ndarray) -> float:
    return 10 * math.log10(float(np.mean(block * block)) + 1e-12)
