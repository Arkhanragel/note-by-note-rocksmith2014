"""
Wait-mode simulator: the Note-by-Note logic in a terminal, with no game involved.

It loads a chart made by ChartDump, shows the next note, and waits until you play it
on the guitar. This lets us tune detection (thresholds, stability, repeated notes,
octave errors) before we put the same logic inside the game.

Close Rocksmith before running this, because the game holds the audio interface.

Usage (from the project root):
  .venv\\Scripts\\python tools\\detector\\wait_sim.py monitor            # live pitch and level readout
  .venv\\Scripts\\python tools\\detector\\wait_sim.py play charts\\notegel1_lead.json [--start 0]
Options: --channel 0|1  --device <index>  --threshold 0.15  --gate -50
"""
from __future__ import annotations

import argparse
import json
import queue
import sys
import time

import numpy as np
import sounddevice as sd

from pitch import OnsetDetector, level_db, midi_name, yin

SR = 48000
BLOCK = 256            # samples per callback: 5.3 ms at 48 kHz (low latency, same idea as your ASIO buffer)
STRING_NAMES_GTR = ["6 (low E)", "5 (A)", "4 (D)", "3 (G)", "2 (B)", "1 (high e)"]
STRING_NAMES_BASS = ["4 (E)", "3 (A)", "2 (D)", "1 (G)"]


def find_focusrite() -> int:
    """Use the WASAPI version of the Focusrite input (the lowest latency one sounddevice supports)."""
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        if d["max_input_channels"] > 0 and "Focusrite" in d["name"] and "WASAPI" in api:
            return i
    raise SystemExit("Focusrite WASAPI input not found. Pass --device <index> (see python -m sounddevice).")


class AudioIn:
    """Collects audio blocks from the driver thread into a queue, and keeps a sliding analysis window."""

    def __init__(self, device: int, channel: int, window: int):
        self.q: queue.Queue[np.ndarray] = queue.Queue()
        self.buf = np.zeros(window, dtype=np.float64)
        self.channel = channel
        self.stream = sd.InputStream(device=device, channels=2, samplerate=SR, blocksize=BLOCK,
                                     dtype="float32", latency="low", callback=self._cb)

    def _cb(self, indata, frames, t, status):
        # Runs on the audio thread: copy the data and return right away.
        self.q.put(indata[:, self.channel].astype(np.float64).copy())

    def blocks(self):
        """Yield (new_block, analysis_window) for each block of 256 samples."""
        with self.stream:
            while True:
                b = self.q.get()
                self.buf = np.roll(self.buf, -len(b))
                self.buf[-len(b):] = b
                yield b, self.buf


def cmd_monitor(args):
    audio = AudioIn(args.device, args.channel, args.window)
    print(f"Monitoring channel {args.channel}. Play single notes. Ctrl+C to stop.\n")
    last = 0.0
    for block, win in audio.blocks():
        now = time.perf_counter()
        if now - last < 0.05:  # redraw 20 times per second
            continue
        last = now
        lvl = level_db(block)
        p = yin(win, SR, threshold=args.threshold) if lvl > args.gate else None
        bar = "#" * int(max(0, lvl + 70) / 2)
        if p:
            m = round(p.midi)
            cents = (p.midi - m) * 100
            txt = f"{midi_name(m):>4} {p.freq:7.1f} Hz {cents:+4.0f}c  aper={p.aperiodicity:.2f}"
        else:
            txt = "   -"
        sys.stdout.write(f"\r{lvl:6.1f} dB {bar:<35} {txt:<45}")
        sys.stdout.flush()


def cmd_play(args):
    chart = json.load(open(args.chart, encoding="utf-8"))
    targets = chart["Targets"]
    names = STRING_NAMES_BASS if chart["IsBass"] else STRING_NAMES_GTR
    fmin = 35.0 if chart["IsBass"] else 70.0
    window = max(args.window, 4096 if chart["IsBass"] else 2048)  # YIN needs >= 2 periods of the lowest note

    audio = AudioIn(args.device, args.channel, window)
    onset = OnsetDetector(gate_db=args.gate)

    idx = args.start
    armed_at = time.perf_counter()   # when the current target became active
    last_onset = 0.0
    stable = 0                       # consecutive analysis frames matching the target
    hits_time: list[float] = []
    octave_errors = 0

    def show(i):
        t = targets[i]
        if t["IsChord"]:
            print(f"\n[{i + 1}/{len(targets)}] CHORD {t['ChordName']} - chords not supported yet, skipped")
            return
        n = t["Notes"][0]
        nxt = "  then: " + ", ".join(f"{x['Notes'][0]['Name']}" for x in targets[i + 1:i + 5] if not x["IsChord"])
        print(f"\n[{i + 1}/{len(targets)}] t={t['Time']:.2f}s  PLAY  string {names[n['String']]}  fret {n['Fret']}"
              f"  ->  {n['Name']}   [{', '.join(n['Techniques'])}]" + nxt)

    print(f"Chart: {chart['SngName']}  ({len(targets)} targets). Waiting for each note. Ctrl+C to stop.")
    show(idx)
    for block, win in audio.blocks():
        now = time.perf_counter()
        if onset.process(block):
            last_onset = now

        t = targets[idx]
        if t["IsChord"]:  # TODO: chords (Phase 2, later). For now, skip them.
            idx += 1
            if idx >= len(targets):
                break
            armed_at, stable = now, 0
            show(idx)
            continue

        if level_db(block) < args.gate:
            stable = 0
            continue
        p = yin(win, SR, fmin=fmin, threshold=args.threshold)
        if p is None:
            stable = 0
            continue

        want = t["Notes"][0]["Midi"]
        prev_same = idx > 0 and not targets[idx - 1]["IsChord"] and targets[idx - 1]["Notes"][0]["Midi"] == want
        # A repeated pitch needs a fresh pick attack after the target was armed (see OnsetDetector).
        fresh = (not prev_same) or (last_onset > armed_at)

        err = p.midi - want
        if abs(err) < 0.5 and fresh:
            stable += 1
        else:
            if abs(abs(err) - 12) < 0.5:
                octave_errors += 1  # counted only as a diagnostic for now
            stable = 0

        if stable >= args.stable:
            dt = now - armed_at
            hits_time.append(dt)
            print(f"   HIT {midi_name(round(p.midi))} ({(p.midi - round(p.midi)) * 100:+.0f} cents) after {dt * 1000:.0f} ms")
            idx += 1
            if idx >= len(targets):
                break
            armed_at, stable = now, 0
            show(idx)

    print(f"\nDone. Median time per note: {np.median(hits_time) * 1000:.0f} ms. Octave-error frames: {octave_errors}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["monitor", "play"])
    ap.add_argument("chart", nargs="?")
    ap.add_argument("--device", type=int, default=None)
    ap.add_argument("--channel", type=int, default=0, help="Focusrite input channel: 0 = input 1, 1 = input 2")
    ap.add_argument("--start", type=int, default=0, help="target index to start from")
    ap.add_argument("--threshold", type=float, default=0.15, help="YIN threshold (lower = stricter)")
    ap.add_argument("--gate", type=float, default=-50.0, help="ignore signal below this level (dBFS)")
    ap.add_argument("--stable", type=int, default=3, help="consecutive matching frames needed (3 x 5.3 ms = 16 ms)")
    ap.add_argument("--window", type=int, default=2048, help="analysis window in samples")
    args = ap.parse_args()
    if args.device is None:
        args.device = find_focusrite()
    try:
        cmd_monitor(args) if args.mode == "monitor" else cmd_play(args)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
