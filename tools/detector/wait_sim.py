"""
Wait-mode simulator: the Note-by-Note logic in a terminal, with no game involved.

Close Rocksmith before running this, because the game holds the audio interface.

Usage (from the project root):
  .venv\\Scripts\\python tools\\detector\\wait_sim.py monitor                     # live readout (one line, redrawn)
  .venv\\Scripts\\python tools\\detector\\wait_sim.py log --duration 90 --record recordings\\take1.wav
  .venv\\Scripts\\python tools\\detector\\wait_sim.py analyze recordings\\take1.wav   # same logic, offline
  .venv\\Scripts\\python tools\\detector\\wait_sim.py play charts\\notegel1_lead.json [--start 0]
Options: --channel 0|1  --device <index>  --threshold  --gate  --stable  --bass
"""
from __future__ import annotations

import argparse
import json
import queue
import sys
import time
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd

from pitch import level_db, midi_name, yin
from tracker import NoteEvent, NoteTracker, TrackerConfig

SR = 48000
BLOCK = 256  # 5.3 ms at 48 kHz: the same low-latency idea as your ASIO buffer
STRING_NAMES_GTR = ["6 (low E)", "5 (A)", "4 (D)", "3 (G)", "2 (B)", "1 (high e)"]
STRING_NAMES_BASS = ["4 (E)", "3 (A)", "2 (D)", "1 (G)"]


def find_focusrite() -> int:
    """Use the WASAPI version of the Focusrite input (the lowest latency one sounddevice supports)."""
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        if d["max_input_channels"] > 0 and "Focusrite" in d["name"] and "WASAPI" in api:
            return i
    raise SystemExit("Focusrite WASAPI input not found. Pass --device <index> (see python -m sounddevice).")


def live_blocks(device: int, channel: int):
    """Yield 256-sample float64 blocks from the interface. The audio thread only copies data into a queue."""
    q: queue.Queue[np.ndarray] = queue.Queue()

    def cb(indata, frames, t, status):
        q.put(indata[:, channel].astype(np.float64).copy())

    with sd.InputStream(device=device, channels=2, samplerate=SR, blocksize=BLOCK,
                        dtype="float32", latency="low", callback=cb):
        while True:
            yield q.get()


def wav_blocks(path: str):
    """Yield 256-sample blocks from a mono 16-bit WAV (as written by --record)."""
    with wave.open(path, "rb") as w:
        assert w.getframerate() == SR and w.getnchannels() == 1 and w.getsampwidth() == 2
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    for i in range(0, len(data) - BLOCK + 1, BLOCK):
        yield data[i:i + BLOCK]


def make_config(args, bass: bool = False) -> TrackerConfig:
    return TrackerConfig(sr=SR, block=BLOCK, window=4096 if bass else 2048, fmin=35.0 if bass else 70.0,
                         threshold=args.threshold, gate_db=args.gate, stable=args.stable,
                         stable_legato=args.stable_legato, rel_gate_db=args.rel_gate)


def fmt(ev: NoteEvent) -> str:
    return (f"{ev.time:7.2f}s  {midi_name(ev.midi):>4}  {ev.freq:7.1f} Hz  {ev.cents:+4.0f} cents"
            f"  level {ev.level_db:6.1f} dB  aper {ev.aperiodicity:.2f}" + ("  (attack)" if ev.attack else ""))


# ------------------------------------------------------------------------------------------ modes

def cmd_monitor(args):
    print(f"Monitoring channel {args.channel}. Play single notes. Ctrl+C to stop.\n")
    win = np.zeros(2048)
    last = 0.0
    for block in live_blocks(args.device, args.channel):
        win = np.roll(win, -len(block))
        win[-len(block):] = block
        now = time.perf_counter()
        if now - last < 0.05:  # redraw 20 times per second
            continue
        last = now
        lvl = level_db(win[-1024:])
        p = yin(win, SR, threshold=args.threshold) if lvl > args.gate else None
        bar = "#" * int(max(0, lvl + 70) / 2)
        txt = f"{midi_name(round(p.midi)):>4} {p.freq:7.1f} Hz {(p.midi - round(p.midi)) * 100:+4.0f}c" if p else "   -"
        sys.stdout.write(f"\r{lvl:6.1f} dB {bar:<35} {txt:<30}")
        sys.stdout.flush()


def cmd_log(args):
    """Print one line per note event. With --record, also save the raw audio for offline analysis."""
    tracker = NoteTracker(make_config(args, args.bass))
    recorded: list[np.ndarray] = []
    print(f"Logging channel {args.channel} for {args.duration:.0f}s. Play single notes.", flush=True)
    for block in live_blocks(args.device, args.channel):
        if args.record:
            recorded.append(block)
        ev = tracker.process(block)
        if ev:
            print(fmt(ev), flush=True)
        if tracker.now >= args.duration:
            break
    if args.record:
        Path(args.record).parent.mkdir(parents=True, exist_ok=True)
        pcm = (np.clip(np.concatenate(recorded), -1, 1) * 32767).astype(np.int16)
        with wave.open(args.record, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm.tobytes())
        print(f"Recorded {len(pcm) / SR:.1f}s to {args.record}", flush=True)


def cmd_analyze(args):
    """Run the recorded WAV through the exact same tracker. Used to tune the parameters offline."""
    tracker = NoteTracker(make_config(args, args.bass))
    count = 0
    for block in wav_blocks(args.target):
        ev = tracker.process(block)
        if ev:
            count += 1
            print(fmt(ev))
    print(f"{count} events")


def cmd_play(args):
    """
    The wait-mode loop: the current target is hit when the tracker reports an event with
    the target's pitch. Events only fire on a new pitch or a fresh attack, so a repeated
    note needs a new pick, and a note still ringing from before can't count again.
    """
    chart = json.load(open(args.target, encoding="utf-8"))
    targets = [t for t in chart["Targets"] if not t["IsChord"]]  # chords are not supported yet
    names = STRING_NAMES_BASS if chart["IsBass"] else STRING_NAMES_GTR
    tracker = NoteTracker(make_config(args, chart["IsBass"]))
    idx = args.start

    def show(i):
        n = targets[i]["Notes"][0]
        upcoming = ", ".join(x["Notes"][0]["Name"] for x in targets[i + 1:i + 5])
        techs = f"  [{', '.join(n['Techniques'])}]" if n["Techniques"] else ""
        print(f"\n[{i + 1}/{len(targets)}]  PLAY  string {names[n['String']]}  fret {n['Fret']}"
              f"  ->  {n['Name']}{techs}      then: {upcoming}", flush=True)

    print(f"Chart: {chart['SngName']}  ({len(targets)} single notes). Ctrl+C to stop.")
    show(idx)
    armed_at = 0.0
    waits: list[float] = []
    for block in live_blocks(args.device, args.channel):
        ev = tracker.process(block)
        if not ev:
            continue
        want = targets[idx]["Notes"][0]["Midi"]
        if ev.midi != want:
            print(f"   ...heard {midi_name(ev.midi)}", flush=True)
            continue
        waits.append(ev.time - armed_at)
        print(f"   HIT {midi_name(ev.midi)} ({ev.cents:+.0f} cents)", flush=True)
        idx += 1
        if idx >= len(targets):
            break
        armed_at = ev.time
        show(idx)
    print(f"\nDone. Median time between notes: {np.median(waits) * 1000:.0f} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["monitor", "log", "analyze", "play"])
    ap.add_argument("target", nargs="?", help="chart .json (play) or .wav (analyze)")
    ap.add_argument("--device", type=int, default=None)
    ap.add_argument("--channel", type=int, default=0, help="Focusrite input channel: 0 = input 1, 1 = input 2")
    ap.add_argument("--start", type=int, default=0, help="play: target index to start from")
    ap.add_argument("--duration", type=float, default=60.0, help="log: seconds to record")
    ap.add_argument("--record", default=None, help="log: save the raw audio to this .wav")
    ap.add_argument("--bass", action="store_true", help="bass range (longer window, lower fmin)")
    ap.add_argument("--threshold", type=float, default=0.15, help="YIN threshold (lower = stricter)")
    ap.add_argument("--gate", type=float, default=-45.0, help="ignore signal below this level (dBFS)")
    ap.add_argument("--stable", type=int, default=3, help="consecutive matching blocks needed (3 x 5.3 ms = 16 ms)")
    ap.add_argument("--stable-legato", type=int, default=6, help="blocks needed for a change without a pick attack")
    ap.add_argument("--rel-gate", type=float, default=18.0, help="ignore frames this many dB below the last attack peak")
    args = ap.parse_args()
    if args.mode in ("monitor", "log", "play") and args.device is None:
        args.device = find_focusrite()
    try:
        {"monitor": cmd_monitor, "log": cmd_log, "analyze": cmd_analyze, "play": cmd_play}[args.mode](args)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
