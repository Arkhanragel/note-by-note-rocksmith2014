"""
Synthetic tests for chord.py: strummed Karplus-Strong "guitars", checked against chords.

This only proves the algorithm is sane on clean, idealised strings. The thresholds must still
be checked on real recordings (the mod saves NoteByNote_debug\\wait_<t>.wav for chord waits;
run them through `wait_sim.py chord <wav> <midis>`).

Run: .venv\\Scripts\\python tools\\detector\\test_chord.py  (writes the synthetic audio to
tools/detector/synth_chords.wav for the C++ comparison: nbn_detector_test --chords ...)
"""
from __future__ import annotations

import sys
import wave
from pathlib import Path

import numpy as np

from chord import ChordConfig, ChordDetector, note_name

SR = 48000
RNG = np.random.default_rng(1)


def pluck(midi: int, dur: float, level: float = 0.25) -> np.ndarray:
    """Karplus-Strong string: a noise burst circulating in a delay line with a lowpass."""
    f = 440.0 * 2 ** ((midi - 69) / 12)
    period = SR / f
    n = int(period)
    frac = period - n                     # first-order allpass for the fractional part (in tune)
    coef = (1 - frac) / (1 + frac)
    line = RNG.uniform(-1, 1, n)
    for i in range(1, n):                 # a softer pluck: fewer high harmonics, like a real string
        line[i] = 0.35 * line[i] + 0.65 * line[i - 1]
    line -= line.mean()
    out = np.zeros(int(dur * SR))
    idx, prev_in, prev_out, last = 0, 0.0, 0.0, 0.0
    for i in range(len(out)):
        x = line[idx]
        y = coef * x + prev_in - coef * prev_out  # allpass
        prev_in, prev_out = x, y
        new = 0.996 * 0.5 * (y + last)            # averaging lowpass + decay
        last = y
        line[idx] = new
        out[i] = x
        idx = (idx + 1) % n
    return level * out / (np.abs(out[:n * 4]).max() + 1e-9)


def strum(notes: list[int], dur: float = 1.2, spread: float = 0.012) -> np.ndarray:
    """Low string first, `spread` s between strings (a downstroke)."""
    out = np.zeros(int((dur + spread * len(notes)) * SR))
    for k, m in enumerate(sorted(notes)):
        s = pluck(m, dur, level=0.25 * RNG.uniform(0.7, 1.0))
        start = int(k * spread * SR)
        out[start:start + len(s)] += s
    return out


CHORDS = {
    "E5":  [40, 47, 52],
    "A5":  [45, 52, 57],
    "G5":  [43, 50, 55],
    "E":   [40, 47, 52, 56, 59, 64],
    "Em":  [40, 47, 52, 55, 59, 64],
    "G":   [43, 47, 50, 55, 59, 67],
    "C":   [48, 52, 55, 60, 64],
    "D":   [50, 57, 62, 66],
    "Am":  [45, 52, 57, 60, 64],
    "A":   [45, 52, 57, 61, 64],
}

# (what the player plays, what the chart wants, expected result)
CASES: list[tuple[str, list[int], str, bool]] = []
for name, notes in CHORDS.items():
    CASES.append((f"{name} as asked", notes, name, True))
    CASES.append((f"only the lowest note of {name}", notes[:1], name, False))
for played, wanted in [("G5", "E5"), ("A5", "E5"), ("E5", "A5"), ("G", "E"), ("C", "D"), ("D", "Am"), ("A", "G")]:
    CASES.append((f"{played} instead of {wanted}", CHORDS[played], wanted, False))
CASES.append(("E5 one fret too high (F5)", [m + 1 for m in CHORDS["E5"]], "E5", False))
CASES.append(("A5 two frets too low (G5)", [m - 2 for m in CHORDS["A5"]], "A5", False))
# Close calls, reported but not asserted (one note differs by a semitone).
SOFT = [("E instead of Em", CHORDS["E"], "Em"), ("Am instead of A", CHORDS["Am"], "A")]


def run(audio: np.ndarray, chord: list[int]) -> list:
    det = ChordDetector(ChordConfig())
    out = []
    for i in range(0, len(audio) - 256 + 1, 256):
        r = det.process(audio[i:i + 256], chord)
        if r:
            out.append(r)
    return out


def main() -> int:
    failures = 0
    stream = []  # everything, for the C++ comparison
    for label, played, wanted, expect in CASES + [(a, b, c, None) for a, b, c in SOFT]:
        audio = np.concatenate([np.zeros(SR // 4), strum(played), np.zeros(SR // 4)])
        audio += RNG.normal(0, 10 ** (-65 / 20), len(audio))  # a little hiss
        stream.append((audio, CHORDS[wanted], label))
        results = run(audio, CHORDS[wanted])
        got = any(r.match for r in results)
        ok = expect is None or got == expect
        failures += not ok
        tag = "ok  " if ok else "FAIL"
        if expect is None:
            tag = "info"
        print(f"{tag} {label:32s} -> {'MATCH' if got else 'no':5s}  wanted {wanted:3s}"
              f" ({' '.join(note_name(m) for m in CHORDS[wanted])})")
        for r in results:
            print("       " + r.describe())
    wav = Path(__file__).with_name("synth_chords.wav")
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(np.concatenate([a for a, _, _ in stream]), -1, 1) * 32767).astype(np.int16).tobytes())
    print(f"\n{len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
