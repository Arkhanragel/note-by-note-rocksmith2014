"""
Wait-mode simulator: the Note-by-Note logic in a terminal, with no game involved.

Close Rocksmith before running this, because the game holds the audio interface.

Usage (from the project root):
  .venv\\Scripts\\python tools\\detector\\wait_sim.py monitor                     # live readout (one line, redrawn)
  .venv\\Scripts\\python tools\\detector\\wait_sim.py log --duration 90 --record recordings\\take1.wav
  .venv\\Scripts\\python tools\\detector\\wait_sim.py analyze recordings\\take1.wav   # same logic, offline
  .venv\\Scripts\\python tools\\detector\\wait_sim.py chord wait_012.345.wav --notes 40,47,52  # chord checks (E5)
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


def tap_blocks():
    """
    Yield 256-sample blocks from the GuitarTap shared memory, i.e. the guitar signal inside the running
    game, written by our RS_ASIO build (see mod/common/GuitarTapShared.h for the layout).
    """
    import mmap
    import struct
    header = struct.Struct("<IIIIqIII7I")  # magic, version, sampleRate, capacity, writePos, pid, blockFrames, lastTick, reserved
    capacity = 1 << 16
    size = header.size + 4 * capacity
    m = mmap.mmap(-1, size, tagname="Local\\NoteByNote_GuitarInput")
    magic, _, sr, _cap, write_pos, *_ = header.unpack_from(m, 0)
    if magic != 0x314E424E:
        raise SystemExit("GuitarTap not active: is the game running with our RS_ASIO build installed?")
    if sr != SR:
        print(f"Note: the tap runs at {sr} Hz (the analysis assumes {SR} Hz)")
    read_pos = write_pos
    ring = np.frombuffer(m, dtype=np.float32, count=capacity, offset=header.size)
    while True:
        write_pos = header.unpack_from(m, 0)[4]
        if write_pos - read_pos > capacity // 2:  # we fell far behind: skip ahead
            read_pos = write_pos - BLOCK
        while write_pos - read_pos >= BLOCK:
            idx = (np.arange(read_pos, read_pos + BLOCK) & (capacity - 1))
            yield ring[idx].astype(np.float64)
            read_pos += BLOCK
        time.sleep(0.002)


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
    source = tap_blocks() if args.source == "tap" else live_blocks(args.device, args.channel)
    for block in source:
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


def cmd_chord(args):
    """Check a recording against a chord after every pick attack (same code as the mod's chord waits)."""
    from chord import ChordConfig, ChordDetector, note_name
    notes = [int(n) for n in args.notes.split(",")]
    det = ChordDetector(ChordConfig())
    print(f"Chord: {' '.join(note_name(m) for m in notes)}")
    matches = 0
    for block in wav_blocks(args.target):
        r = det.process(block, notes)
        if r:
            matches += r.match
            print(r.describe())
    print(f"{matches} matches")


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


# Friendly names for the play screen. SNG string 0 is the thickest string.
FRIENDLY_GTR = ["6th string (thickest, low E)", "5th string (A)", "4th string (D)",
                "3rd string (G)", "2nd string (B)", "1st string (thinnest, high e)"]
FRIENDLY_BASS = ["4th string (thickest, E)", "3rd string (A)", "2nd string (D)", "1st string (thinnest, G)"]
TAB_LABELS_GTR = ["E", "A", "D", "G", "B", "e"]
TAB_LABELS_BASS = ["E", "A", "D", "G"]


def open_string_midi(chart) -> list[int]:
    """Pitch (MIDI) of each open string for this chart: standard tuning + the song's tuning offsets.
    (Frets in the chart are absolute neck positions, so the capo isn't added. See the ChartDump TODO.)"""
    base = [28, 33, 38, 43] if chart["IsBass"] else [40, 45, 50, 55, 59, 64]
    return [b + chart["Tuning"][s] for s, b in enumerate(base)]


def where_played(midi: int, target_string: int, opens: list[int]) -> tuple[int, int] | None:
    """
    A pitch can be played in several places on the neck. Guess where the player actually
    played it: prefer the string they were asked to play, otherwise the closest string
    where it fits between fret 0 and 24.
    """
    options = [(s, midi - o) for s, o in enumerate(opens) if 0 <= midi - o <= 24]
    if not options:
        return None
    return min(options, key=lambda sf: (abs(sf[0] - target_string), sf[1]))


def mini_tab(targets, i: int, labels: list[str], count: int = 6) -> str:
    """
    Draw the current note and the next few as guitar tablature. Each line is a string
    (thinnest on top, as in standard tab), and the number is the fret to press.
    The current note is marked with [ ].
    """
    rows = {s: labels[s] + "|-" for s in range(len(labels))}
    for k, t in enumerate(targets[i:i + count]):
        n = t["Notes"][0]
        for s in rows:
            if s != n["String"]:
                cell = "---"
            elif k == 0:
                cell = f"[{n['Fret']}]"  # the note being waited for
            else:
                cell = f" {n['Fret']} "
            rows[s] += cell.ljust(4, "-") + "-"
    return "\n".join("      " + rows[s] for s in reversed(range(len(labels))))


def cmd_play(args):
    """
    The wait-mode loop: the current target is hit when the tracker reports an event with
    the target's pitch. Events only fire on a new pitch or a fresh attack, so a repeated
    note needs a new pick, and a note still ringing from before can't count again.
    """
    chart = json.load(open(args.target, encoding="utf-8"))
    targets = [t for t in chart["Targets"] if not t["IsChord"]]  # chords are not supported yet
    bass = chart["IsBass"]
    names = FRIENDLY_BASS if bass else FRIENDLY_GTR
    labels = TAB_LABELS_BASS if bass else TAB_LABELS_GTR
    opens = open_string_midi(chart)
    tracker = NoteTracker(make_config(args, bass))
    idx = args.start

    def show(i):
        n = targets[i]["Notes"][0]
        fret = "open string (don't press any fret)" if n["Fret"] == 0 else f"fret {n['Fret']}"
        print(f"\n----- Note {i + 1} of {len(targets)} " + "-" * 40)
        print(f"  PLAY:  {names[n['String']]}, {fret}")
        if n["Techniques"]:
            print(f"  Technique: {', '.join(n['Techniques'])}")
        print(mini_tab(targets, i, labels))
        print("  (waiting for you...)", flush=True)

    def explain_miss(ev, n):
        pos = where_played(ev.midi, n["String"], opens)
        diff = ev.midi - n["Midi"]
        if pos is None:
            return "  X  Not that one: that sound is outside the neck range. Try again."
        s, f = pos
        what = f"{names[s]}, " + ("open" if f == 0 else f"fret {f}")
        if abs(diff) % 12 == 0:
            hint = "right note but in a different octave (same name, higher or lower)"
        elif s == n["String"]:
            hint = f"{abs(diff)} fret{'s' if abs(diff) > 1 else ''} too {'high' if diff > 0 else 'low'}"
        else:
            hint = "wrong string or fret"
        return f"  X  Not that one: sounded like {what} ({hint}). Try again."

    print(f"Song: {chart['SngName']}  ({len(targets)} single notes). Press Ctrl+C to stop.")
    print("How to read the tab: each line is a string (thinnest on top), the number is the fret.")
    show(idx)
    armed_at = 0.0
    waits: list[float] = []
    for block in live_blocks(args.device, args.channel):
        ev = tracker.process(block)
        if not ev:
            continue
        n = targets[idx]["Notes"][0]
        if ev.midi != n["Midi"]:
            print(explain_miss(ev, n), flush=True)
            continue
        waits.append(ev.time - armed_at)
        tuning = ""
        if abs(ev.cents) >= 25:
            tuning = "  (a bit sharp: check tuning)" if ev.cents > 0 else "  (a bit flat: check tuning)"
        print(f"  OK  Correct!{tuning}", flush=True)
        idx += 1
        if idx >= len(targets):
            break
        armed_at = ev.time
        show(idx)
    print(f"\nFinished! You played {len(waits)} notes. Typical time per note: {np.median(waits):.1f} s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["monitor", "log", "analyze", "chord", "play"])
    ap.add_argument("target", nargs="?", help="chart .json (play) or .wav (analyze, chord)")
    ap.add_argument("--notes", default="40,47,52", help="chord: the chord's MIDI notes, e.g. 40,47,52 (E5)")
    ap.add_argument("--device", type=int, default=None)
    ap.add_argument("--channel", type=int, default=0, help="Focusrite input channel: 0 = input 1, 1 = input 2")
    ap.add_argument("--start", type=int, default=0, help="play: target index to start from")
    ap.add_argument("--duration", type=float, default=60.0, help="log: seconds to record")
    ap.add_argument("--record", default=None, help="log: save the raw audio to this .wav")
    ap.add_argument("--source", choices=["interface", "tap"], default="interface",
                    help="log: 'tap' = read the guitar from the running game (our RS_ASIO build)")
    ap.add_argument("--bass", action="store_true", help="bass range (longer window, lower fmin)")
    ap.add_argument("--threshold", type=float, default=0.15, help="YIN threshold (lower = stricter)")
    ap.add_argument("--gate", type=float, default=-45.0, help="ignore signal below this level (dBFS)")
    ap.add_argument("--stable", type=int, default=3, help="consecutive matching blocks needed (3 x 5.3 ms = 16 ms)")
    ap.add_argument("--stable-legato", type=int, default=6, help="blocks needed for a change without a pick attack")
    ap.add_argument("--rel-gate", type=float, default=18.0, help="ignore frames this many dB below the last attack peak")
    args = ap.parse_args()
    if args.mode in ("monitor", "log", "play") and args.device is None and args.source != "tap":
        args.device = find_focusrite()
    try:
        {"monitor": cmd_monitor, "log": cmd_log, "analyze": cmd_analyze, "chord": cmd_chord, "play": cmd_play}[args.mode](args)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
