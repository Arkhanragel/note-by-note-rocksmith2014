"""
Chord detection for wait mode: "did the player just strum THIS chord?"

YIN (pitch.py) follows one note. A strummed chord is several notes at once, so it often has
no clear single period at all. Here we look at the SPECTRUM instead, only right after a pick
attack, and only to answer a yes/no question about a KNOWN chord (we are not transcribing
music, we are checking the player's answer).

How one check works (`analyze` + `ChordDetector`):
  1. Take 4096 samples (85 ms) that start just after the attack, Hann window, FFT
     (zero-padded to 16384 so each bin is 2.9 Hz), and list the spectral PEAKS.
  2. For each candidate note, add up the peaks that sit on its harmonics (f0, 2 f0, 3 f0...),
     low harmonics weighing more ("harmonic sum", Klapuri 2006). The best candidate is a note
     that is really sounding.
  3. Mark that note's harmonic peaks as explained, and repeat while the next best candidate
     is still reasonably strong, counting only the peaks nothing heard so far explains (and
     the candidate's fundamental must be one of them). So the 3rd harmonic of E2 (B3) is not
     heard as a B, while a real B2 is (its fundamental, 3rd, 5th... are not E2 harmonics).
     Result: the notes heard.
  4. Compare the notes heard with the chord by PITCH CLASS (E, B...), not exact notes: the
     octave copies inside a chord (E2 + E3 in a power chord) hide under each other's harmonics.
     Match = the strongest note heard belongs to the chord, enough of the chord's pitch
     classes were heard, and not much else was heard.

The mod runs the C++ port in mod/nbn/detector.cpp (ChordDetector). Keep both in sync.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from pitch import OnsetDetector

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(m: int) -> str:
    return f"{NAMES[m % 12]}{m // 12 - 1}"


@dataclass
class ChordConfig:
    sr: int = 48000
    window: int = 4096            # samples analysed (85 ms)
    nfft: int = 16384             # zero-padded FFT size (2.9 Hz bins)
    gate_db: float = -45.0        # same absolute gate as the note tracker
    delays: tuple = (0.09, 0.18)  # seconds after the attack when the chord is checked (window END)
    fmin: float = 50.0            # peaks outside this range are ignored
    fmax: float = 5000.0
    peak_floor_db: float = 45.0   # peaks this far below the strongest one are ignored
    fund_floor_db: float = 30.0   # a note's fundamental must be within this of the strongest peak
                                  # (the Hann window's sidelobes, ~31 dB down, make fake low peaks)
    tol_cents: float = 30.0       # a peak within this of h*f0 counts as harmonic h
    max_harm: int = 60            # (in practice fmax limits it: every harmonic up to 5 kHz counts)
    max_notes: int = 6
    stop_rel: float = 0.2         # stop picking notes below this fraction of the first salience
    octave_rel: float = 0.5       # prefer a lower note (best = its harmonic) with this fraction of the salience
    extra_rel: float = 0.4        # a non-chord note counts as "wrong" above this fraction
    # A chord must be STRUMMED: a check is only valid if the sound is at most strum_rel_db below the
    # loudest moment of the last strum_window seconds. (In-game: a chord played with one wrong string
    # was fixed by putting a finger down 2.5 s later; the faint remains matched, 14 dB down.)
    strum_rel_db: float = 12.0
    strum_window: float = 2.0


@dataclass
class ChordResult:
    time: float                    # seconds of audio processed (end of the analysed window)
    level_db: float
    heard: list = field(default_factory=list)   # [(midi, salience)] strongest first
    hits: int = 0                  # chord pitch classes heard
    needed: int = 0                # how many are required
    extra: list = field(default_factory=list)   # wrong pitch classes heard (strong ones)
    quiet: bool = False            # much quieter than the last strum: doesn't count
    match: bool = False

    def describe(self) -> str:
        heard = " ".join(note_name(m) for m, _ in self.heard) or "-"
        return (f"{self.time:7.2f}s  chord {'MATCH' if self.match else 'no   '}  heard [{heard}]  "
                f"{self.hits}/{self.needed} chord notes"
                + (f", wrong: {' '.join(NAMES[p] for p in self.extra)}" if self.extra else "")
                + f"  level {self.level_db:6.1f} dB" + ("  (too quiet: not a new strum)" if self.quiet else ""))


def spectral_peaks(x: np.ndarray, cfg: ChordConfig) -> tuple[np.ndarray, np.ndarray]:
    """(frequencies, amplitudes) of the local maxima of the magnitude spectrum."""
    w = np.hanning(len(x))
    mag = np.abs(np.fft.rfft(x * w, cfg.nfft))
    k = np.arange(1, len(mag) - 1)
    is_peak = (mag[k] > mag[k - 1]) & (mag[k] >= mag[k + 1])
    k = k[is_peak]
    hz = cfg.sr / cfg.nfft
    k = k[(k * hz >= cfg.fmin) & (k * hz <= cfg.fmax)]
    if len(k) == 0:
        return np.zeros(0), np.zeros(0)
    # Parabolic interpolation on the log magnitude: a finer frequency and amplitude.
    a, b, c = (np.log(mag[k - 1] + 1e-12), np.log(mag[k] + 1e-12), np.log(mag[k + 1] + 1e-12))
    den = a - 2 * b + c
    shift = np.where(np.abs(den) > 1e-12, 0.5 * (a - c) / np.where(den == 0, 1, den), 0.0)
    freq = (k + shift) * hz
    amp = np.exp(b - 0.25 * (a - c) * shift)
    keep = amp >= amp.max() * 10 ** (-cfg.peak_floor_db / 20)
    return freq[keep], amp[keep]


def midi_hz(m: float) -> float:
    return 440.0 * 2 ** ((m - 69) / 12)


def harmonics(f0: float, freq: np.ndarray, amp: np.ndarray, cfg: ChordConfig) -> list[tuple[int, int, float]]:
    """[(h, peak index, amplitude)] of the peaks sitting on harmonics of f0 (strongest peak per h)."""
    out = []
    if len(freq) == 0:
        return out
    tol = cfg.tol_cents / 1200
    for h in range(1, cfg.max_harm + 1):
        target = h * f0
        if target > cfg.fmax:
            break
        near = np.nonzero(np.abs(np.log2(freq / target)) < tol)[0]
        if len(near):
            i = int(near[np.argmax(amp[near])])
            out.append((h, i, float(amp[i])))
    return out


def salience(f0: float, harm: list[tuple[int, int, float]], claimed: set[int], fund_min: float) -> float:
    """
    Harmonic sum with Klapuri's weights (low harmonics count more, and more so for low notes),
    over the peaks NOT already explained by a note heard before (`claimed`). 0 if the
    fundamental itself is missing or explained: a guitar string always shows its fundamental,
    and a "note" whose fundamental is just the 3rd or 5th harmonic of a lower note is a ghost.
    """
    if not harm or harm[0][0] != 1 or harm[0][1] in claimed or harm[0][2] < fund_min:
        return 0.0
    return sum((f0 + 27) / (h * f0 + 320) * a for h, i, a in harm if i not in claimed)


# A higher candidate whose fundamental is harmonic 2, 3, 4, 5, 6 or 8 of a lower one could just be
# that lower note (semitones above it):
_HARMONIC_INTERVALS = (36, 31, 28, 24, 19, 12)


def analyze(x: np.ndarray, lo: int, hi: int, cfg: ChordConfig) -> list[tuple[int, float]]:
    """Notes heard in x (candidates lo..hi MIDI): [(midi, salience)] in the order found."""
    freq, amp = spectral_peaks(x, cfg)
    harm = {m: harmonics(midi_hz(m), freq, amp, cfg) for m in range(lo, hi + 1)}
    fund_min = (amp.max() if len(amp) else 0.0) * 10 ** (-cfg.fund_floor_db / 20)
    claimed: set[int] = set()   # peak indexes explained by the notes heard so far
    heard: list[tuple[int, float]] = []
    first = 0.0
    while len(heard) < cfg.max_notes:
        sal = {m: salience(midi_hz(m), harm[m], claimed, fund_min) for m in harm if all(m != n for n, _ in heard)}
        best = max(sal, key=sal.get, default=None)
        if best is None or sal[best] <= 0 or sal[best] < cfg.stop_rel * first:
            break
        # The strongest candidate may sit on a harmonic of a lower note that is also sounding
        # (D5 = 4th harmonic of D3, B3 = 3rd of E2): then the lower note is the real one.
        for down in _HARMONIC_INTERVALS:
            if sal.get(best - down, 0) >= cfg.octave_rel * sal[best]:
                best -= down
                break
        first = first or sal[best]
        heard.append((best, sal[best]))
        claimed.update(i for _, i, _ in harm[best])
    return heard


def judge(heard: list[tuple[int, float]], chord: list[int], cfg: ChordConfig) -> tuple[bool, int, int, list[int]]:
    """(match, chord pitch classes heard, needed, wrong pitch classes)."""
    want = sorted({m % 12 for m in chord})
    # Power chords and double stops (2 pitch classes): both. Bigger chords: all but one.
    needed = len(want) if len(want) <= 2 else len(want) - 1
    if not heard or not want:
        return False, 0, needed, []
    got = {m % 12 for m, _ in heard}
    hits = len(got & set(want))
    top = heard[0][1]
    extra = sorted({m % 12 for m, s in heard if m % 12 not in want and s >= cfg.extra_rel * top})
    match = heard[0][0] % 12 in want and hits >= needed and not extra
    return match, hits, needed, extra


class ChordDetector:
    """
    Fed the same 256-sample blocks as the NoteTracker. After each pick attack it checks the
    audio against the expected chord at `delays` after the attack (a strum takes a few tens of
    ms to reach all strings) and returns a ChordResult for each check.
    """

    def __init__(self, cfg: ChordConfig):
        self.cfg = cfg
        self.buf = np.zeros(cfg.window)
        self.onset = OnsetDetector(cfg.sr, gate_db=cfg.gate_db)
        self.samples = 0
        self.due: list[int] = []   # sample counts at which to check
        self.levels: list[tuple[float, float]] = []  # (time, window level dB), last strum_window s

    @property
    def now(self) -> float:
        return self.samples / self.cfg.sr

    def process(self, block: np.ndarray, chord: list[int]) -> ChordResult | None:
        c = self.cfg
        n = len(block)
        self.buf = np.roll(self.buf, -n)
        self.buf[-n:] = block
        self.samples += n
        lvl = 10 * np.log10(float(np.mean(self.buf * self.buf)) + 1e-12)
        self.levels = [(t, v) for t, v in self.levels if self.now - t < c.strum_window] + [(self.now, lvl)]
        if self.onset.process(self.buf, self.now * 1000):
            self.due = [self.samples + int(d * c.sr) for d in c.delays]
        if not self.due or self.samples < self.due[0]:
            return None
        self.due.pop(0)
        if not chord:
            return None
        res = ChordResult(self.now, lvl)
        if lvl < c.gate_db:
            return res
        # Candidates: from a little below the chord's lowest note (a wrong chord a few frets lower)
        # up to an octave above its highest note.
        lo, hi = max(23, min(chord) - 7), min(100, max(chord) + 12)
        res.heard = analyze(self.buf, lo, hi, c)
        res.match, res.hits, res.needed, res.extra = judge(res.heard, chord, c)
        res.quiet = lvl < max(v for _, v in self.levels) - c.strum_rel_db
        res.match = res.match and not res.quiet
        return res
