"""
Offline check of the YIN detector on synthetic "plucked string" tones.

Each test tone has a weak fundamental and a strong 2nd harmonic, plus decay and noise.
That's the case where naive FFT peak-picking reports the wrong octave.
Run: .venv\\Scripts\\python tools\\detector\\test_pitch.py
"""
import numpy as np

from pitch import hz_to_midi, midi_name, yin

SR = 48000


def pluck(midi: int, dur: float = 0.1, seed: int = 0) -> np.ndarray:
    f0 = 440.0 * 2 ** ((midi - 69) / 12)
    t = np.arange(int(SR * dur)) / SR
    amps = [0.4, 1.0, 0.6, 0.4, 0.25, 0.15, 0.1]  # the 2nd harmonic is stronger than the fundamental
    x = sum(a * np.sin(2 * np.pi * f0 * (k + 1) * t + k) for k, a in enumerate(amps))
    x *= np.exp(-t * 3)
    x += np.random.default_rng(seed).normal(0, 0.02, len(t))
    return x


def main():
    fails = 0
    # Guitar range: E2 (40) up to about the 22nd fret of the high e (86). Bass range from E1 (28).
    for midi, window, fmin in [(m, 2048, 70.0) for m in range(40, 87)] + [(m, 4096, 35.0) for m in range(28, 45)]:
        x = pluck(midi, dur=window / SR + 0.01)[-window:]
        p = yin(x, SR, fmin=fmin)
        got = round(p.midi) if p else None
        if got != midi:
            fails += 1
            print(f"FAIL {midi_name(midi)} (window {window}): got {midi_name(got) if got else None}")
    total = (87 - 40) + (45 - 28)
    print(f"{total - fails}/{total} correct")
    # Silence and noise must NOT produce a pitch
    noise = np.random.default_rng(1).normal(0, 0.1, 2048)
    print("noise ->", yin(noise, SR))


if __name__ == "__main__":
    main()
