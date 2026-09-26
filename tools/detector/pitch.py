"""
Monophonic pitch detection for guitar/bass: the YIN algorithm, plus a small onset detector.

WHY YIN?
A guitar note isn't a pure sine. It has strong harmonics, and the 2nd harmonic is sometimes
louder than the fundamental, so "take the biggest FFT peak" often gives the wrong octave.
YIN (de Cheveigné & Kawahara, 2002) works in the time domain instead. It looks for the
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
    we require a fresh attack. A pick attack makes the short-term energy jump well above
    the recent average, and that jump is what we look for.
    """

    def __init__(self, ratio: float = 2.0, gate_db: float = -50.0, history: int = 6):
        self.ratio = ratio                  # energy must be this many times the recent average (2.0 = +3 dB)
        self.gate = 10 ** (gate_db / 10)    # ignore anything quieter than this (energy units)
        self.hist: list[float] = []
        self.history = history

    def process(self, block: np.ndarray) -> bool:
        e = float(np.mean(block * block))
        avg = (sum(self.hist) / len(self.hist)) if self.hist else e
        self.hist.append(e)
        if len(self.hist) > self.history:
            self.hist.pop(0)
        return e > self.gate and e > self.ratio * max(avg, 1e-12)


def level_db(block: np.ndarray) -> float:
    return 10 * math.log10(float(np.mean(block * block)) + 1e-12)
