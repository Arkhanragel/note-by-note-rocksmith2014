"""
String identification experiment: can we tell WHERE on the neck a note was played, not only
which note? An E4 can be played on string 1 open, string 2 fret 5, string 3 fret 9 or string 4
fret 14. The pitch is the same, but the sound is slightly different. This script records the same
notes at every position on YOUR guitar and measures whether those differences are big enough to
tell the positions apart. It's a go/no-go test before we touch the mod.

The cues (all physics, see Fletcher & Rossing, "The Physics of Musical Instruments"):
  1. Inharmonicity. A real string is a bit stiff, so its overtones are slightly sharp:
         f_k = k * f0 * sqrt(1 + B k^2)
     B grows with the string's thickness and gets 4x bigger for every octave you go up the neck
     (B ~ 1/L^2 and fret 12 halves L). Same pitch on a thicker string, higher up = a bigger B.
  2. Pickup (and pick) position. The pickup sits at a fixed distance from the bridge, but pressing a
     fret shortens the vibrating string, so the pickup ends up at a different FRACTION of it.
     Overtones that have a node at that point are weak ("comb filter"). The pattern of weak
     overtones moves with the fret. Same thing for where the pick hits the string.
  3. Decay. Different strings / lengths lose energy at different rates.

Two ways to use them are tested:
  A. Calibrated per note: learn each position from round 1, recognize round 2 (and the reverse).
     The best case: it needs the player to record every note at every position.
  B. Physics model, calibrated on the OPEN strings only: from the open strings we learn B per
     string and the pickup/pick fractions, then predict every fretted position. This is what the
     mod could realistically ship ("pluck each open string 5 times" calibration).

Close Rocksmith first (it holds the audio interface). Use your usual guitar, pickup and settings,
the same for the whole session. Ctrl+C stops at any time; running `record` again resumes.

Usage (from the project root):
  .venv\\Scripts\\python tools\\detector\\string_id.py check         # 30 s: is the guitar signal clean?
  .venv\\Scripts\\python tools\\detector\\string_id.py record --out recordings\\string_id\\take1 --pickup bridge
  .venv\\Scripts\\python tools\\detector\\string_id.py analyze recordings\\string_id\\take1
  .venv\\Scripts\\python tools\\detector\\string_id.py selftest      # checks this code on synthetic strings
"""
from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import queue
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from pitch import midi_name
from tracker import NoteTracker, TrackerConfig

SR = 48000
BLOCK = 256
STD_OPENS = [40, 45, 50, 55, 59, 64]           # E standard, index 0 = thickest string
STRING_LETTERS = ["E", "A", "D", "G", "B", "e"]
STRING_ANSI = [196, 226, 33, 208, 46, 135]     # highway colours: red, yellow, blue, orange, green, purple
# The notes recorded. Together they cover every open string (needed by the physics model) and
# notes that exist at 2-4 places of the neck (up to --max-fret).
TEST_NOTES = [40, 45, 48, 50, 55, 57, 59, 60, 64, 66]   # E2 A2 C3 D3 G3 A3 B3 C4 E4 F#4

MIN_GAP_S = 1.2       # plucks closer than this are ignored (each note must ring)
RING_AFTER_S = 2.0    # keep recording after the last pluck

# Analysis
NFFT = 1 << 17        # zero-padded FFT: 0.37 Hz per bin at 48 kHz
BIN = SR / NFFT
WIN_START_S = 0.03    # analysis window: from the pluck event + this ...
WIN_LEN_S = 0.6       # ... for this long (shorter if the next pluck comes first)
MAX_HZ = 7000.0       # overtones searched up to here
FLOOR_DB = -60.0      # overtones weaker than the strongest peak - 60 dB are ignored
N_PROFILE = 16        # overtones in the "tone" profile
PROFILE_FILL = -50.0  # value for an overtone that wasn't found
MIN_TOP_K = 6         # B needs an overtone at least this high (8 lost dull notes; same in mod/nbn/stringid.cpp)


# ---------------------------------------------------------------------------------------------
# Positions and names
# ---------------------------------------------------------------------------------------------

def positions(opens: list[int], notes: list[int], max_fret: int) -> list[tuple[int, int, int]]:
    """(midi, string, fret) for every place each note can be played, string 0 = thickest."""
    out = []
    for m in notes:
        for s, o in enumerate(opens):
            if 0 <= m - o <= max_fret:
                out.append((m, s, m - o))
    return out


def string_label(s: int, color: bool = True) -> str:
    """"string 4 (D)" with the standard numbering (1 = thinnest), in the string's highway colour."""
    text = f"string {6 - s} ({STRING_LETTERS[s]})"
    return f"\x1b[38;5;{STRING_ANSI[s]}m{text}\x1b[0m" if color else text


def where_label(s: int, f: int, color: bool = True) -> str:
    return f"{string_label(s, color)} open" if f == 0 else f"fret {f} on {string_label(s, color)}"


def short_pos(s: int, f: int) -> str:
    """Compact name for tables: "s4f9" = string 4 (standard numbering), fret 9."""
    return f"s{6 - s}f{f}"


def file_stem(rnd: int, m: int, s: int, f: int) -> str:
    return f"r{rnd}_{midi_name(m).replace('#', 's')}_str{6 - s}_f{f}"


# ---------------------------------------------------------------------------------------------
# Recording
# ---------------------------------------------------------------------------------------------

class Mic:
    """The interface input as a queue of 256-sample blocks (the audio thread only copies)."""

    def __init__(self, device: int, channel: int):
        import sounddevice as sd
        self.q: queue.Queue[np.ndarray] = queue.Queue()
        self.stream = sd.InputStream(device=device, channels=2, samplerate=SR, blocksize=BLOCK, dtype="float32",
                                     latency="low", callback=lambda d, *_: self.q.put(d[:, channel].astype(np.float64)))
        self.stream.start()

    def drain(self):
        """Throw away what arrived while we weren't listening (between positions)."""
        while not self.q.empty():
            self.q.get_nowait()

    def get(self) -> np.ndarray:
        return self.q.get()


def save_wav(path: Path, x: np.ndarray):
    pcm = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def load_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        assert w.getframerate() == SR and w.getnchannels() == 1 and w.getsampwidth() == 2, path
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0


def record_position(mic: Mic, m: int, n_plucks: int) -> tuple[np.ndarray, list[float], list[float]]:
    """Record until n_plucks of the right note were heard (each with a pick attack), plus some ring."""
    tracker = NoteTracker(TrackerConfig(sr=SR, block=BLOCK))
    audio: list[np.ndarray] = []
    plucks: list[float] = []
    freqs: list[float] = []
    done_at = None
    mic.drain()
    while True:
        block = mic.get()
        audio.append(block)
        ev = tracker.process(block)
        now = tracker.now
        if done_at is not None:
            if now - done_at >= RING_AFTER_S:
                break
            continue
        # Only events with a pick attack count: a note still ringing from the previous position
        # would otherwise be taken for the first pluck.
        if ev is None or not ev.attack:
            continue
        if ev.midi != m:
            print(f"      \x1b[90m(heard {midi_name(ev.midi)}, expected {midi_name(m)} - check the fret)\x1b[0m")
        elif plucks and now - plucks[-1] < MIN_GAP_S:
            print("      \x1b[90m(too soon - let each note ring about 2 seconds)\x1b[0m")
        else:
            plucks.append(ev.time)
            freqs.append(ev.freq)
            print(f"      pluck {len(plucks)}/{n_plucks} ok")
            if len(plucks) == n_plucks:
                done_at = now
    return np.concatenate(audio), plucks, freqs


def cmd_record(args):
    from wait_sim import find_focusrite
    os.system("")  # turns on ANSI colours in the Windows console
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    opens = [int(v) for v in args.opens.split(",")]
    # Walk the neck string by string (thickest first), frets going up, so the hand only slides along
    # one string at a time. Round 2 walks back (thinnest first, frets going down).
    pos = sorted(positions(opens, TEST_NOTES, args.max_fret), key=lambda p: (p[1], p[2]))
    plan = []
    for rnd in range(1, args.rounds + 1):
        plan += [(rnd, *p) for p in (pos if rnd % 2 else reversed(pos))]
    todo = [p for p in plan if not (out / (file_stem(*p) + ".json")).exists()]
    if not todo:
        print(f"Everything is recorded in {out}. Run: string_id.py analyze {out}")
        return
    minutes = len(todo) * (args.plucks * 2 + 4) / 60
    print(f"\n{len(todo)} of {len(plan)} positions to record ({args.rounds} rounds, {args.plucks} plucks each), "
          f"about {minutes:.0f} minutes.\n"
          "  - Tune the guitar first. Keep the same pickup, volume and tone for the whole session.\n"
          "  - Pick normally, with the pick where you usually play. Let each note ring ~2 seconds.\n"
          "  - Only the asked string should sound: mute the others.\n"
          "  - Ctrl+C stops. Run the same command again to continue where you left.\n")
    device = args.device if args.device is not None else find_focusrite()
    mic = Mic(device, args.channel)
    for i, (rnd, m, s, f) in enumerate(todo):
        if i == 0 or todo[i - 1][2] != s:
            print(f"\n--- now on {string_label(s)} ---")
        nxt = f"   next: {where_label(todo[i + 1][2], todo[i + 1][3])}" if i + 1 < len(todo) else ""
        print(f"[{i + 1}/{len(todo)}]  round {rnd}:  {midi_name(m)}  =  {where_label(s, f)}   "
              f"(pluck {args.plucks} times){nxt}")
        x, plucks, freqs = record_position(mic, m, args.plucks)
        stem = file_stem(rnd, m, s, f)
        save_wav(out / (stem + ".wav"), x)
        meta = {"file": stem + ".wav", "round": rnd, "midi": m, "string": s, "string_std": 6 - s, "fret": f,
                "plucks": plucks, "freqs": freqs, "pickup": args.pickup, "guitar": args.guitar, "opens": opens,
                "sr": SR, "recorded": datetime.datetime.now().isoformat(timespec="seconds")}
        (out / (stem + ".json")).write_text(json.dumps(meta, indent=1))
    print(f"\nDone. Run: .venv\\Scripts\\python tools\\detector\\string_id.py analyze {out}")


# Inharmonicity of the open strings in recordings\take1.wav (2026-09-26, W900 -> Focusrite, a clean
# signal): the reference for `check`.
CLEAN_LOG_B = {45: math.log10(6.2e-5), 50: math.log10(6.3e-5), 55: math.log10(9.8e-5),
               59: math.log10(2.6e-5), 64: math.log10(1.1e-5)}


def cmd_check(args):
    """Quick check of the signal chain: pluck the open strings and compare with a known clean take."""
    from wait_sim import find_focusrite
    os.system("")
    out = Path(args.out or "recordings/string_id")
    out.mkdir(parents=True, exist_ok=True)
    print(f"\nPluck each OPEN string once, thickest first (6 E, 5 A, 4 D, 3 G, 2 B, 1 e), letting each ring "
          f"~2 seconds.\nRecording for {args.seconds:.0f} seconds...\n")
    mic = Mic(args.device if args.device is not None else find_focusrite(), args.channel)
    tracker = NoteTracker(TrackerConfig(sr=SR, block=BLOCK))
    audio, events = [], []
    mic.drain()
    while tracker.now < args.seconds:
        block = mic.get()
        audio.append(block)
        ev = tracker.process(block)
        if ev and ev.attack and (not events or ev.time - events[-1].time >= MIN_GAP_S):
            events.append(ev)
            print(f"   {midi_name(ev.midi):4} {ev.level_db:5.1f} dB")
    mic.stream.stop()
    x = np.concatenate(audio)
    save_wav(out / "check.wav", x)
    lines = ["", f"{'note':5} {'level':>6} {'after 0.6 s':>11} {'peak/rms':>8} {'log10 B':>8} {'clean take':>10}"]
    drops, deltas = [], []
    for i, ev in enumerate(events):
        t_next = events[i + 1].time if i + 1 < len(events) else len(x) / SR
        a, b = int((ev.time + 0.05) * SR), int((ev.time + 0.25) * SR)
        c, d = int((ev.time + 0.6) * SR), int((ev.time + 0.8) * SR)
        if t_next - ev.time < 0.85 or d > len(x):
            continue
        lv0 = 10 * math.log10(float(np.mean(x[a:b] ** 2)) + 1e-12)
        lv1 = 10 * math.log10(float(np.mean(x[c:d] ** 2)) + 1e-12)
        crest = float(np.abs(x[a:b]).max() / math.sqrt(float(np.mean(x[a:b] ** 2)) + 1e-12))
        p = pluck_features(x, ev.time, t_next, {"round": 0, "midi": ev.midi, "string": -1, "fret": -1}, ev.freq)
        lb = p.log_b if p else float("nan")
        ref = CLEAN_LOG_B.get(ev.midi)
        drops.append(lv0 - lv1)
        if ref is not None and not math.isnan(lb):
            deltas.append(lb - ref)
        lines.append(f"{midi_name(ev.midi):5} {lv0:5.0f}dB {lv1 - lv0:+9.0f}dB {crest:8.2f} {lb:8.2f} "
                     + (f"{ref:10.2f}" if ref is not None else f"{'':10}"))
    if not drops:
        lines.append("No usable plucks (each must ring at least 0.85 s). Try again.")
    else:
        drop = float(np.median(drops))
        # The fade is shown for information only: it varies a lot with how the string is muted.
        lines.append(f"\nThe note fades by {drop:.0f} dB in 0.6 s (median).")
        if not deltas:
            lines.append("VERDICT: unsure - no open A, D, G, B or e string was heard clearly. Try again.")
        else:
            dl = float(np.median(deltas))
            lines.append(f"Overtone stiffness vs the clean take: {dl:+.2f} (0 = the same; the processed take "
                         "of 2026-10-02: about -1.3).")
            # Every string must match, not just the median: a light overdrive changes the loud wound
            # strings (E A D) and leaves the plain ones almost untouched (seen 2026-10-02).
            lines.append("VERDICT: " + ("looks CLEAN - ready to record the test." if min(deltas) > -0.6 else
                                        "still PROCESSED (drive / compression on some strings) - check the chain."))
    text = "\n".join(lines)
    print(text)
    (out / "check.txt").write_text(text + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# Features of one pluck
# ---------------------------------------------------------------------------------------------

@dataclass
class Pluck:
    round: int
    midi: int
    string: int
    fret: int
    f0: float
    log_b: float           # log10 of the inharmonicity B (nan = not enough overtones to measure it)
    profile: np.ndarray    # dB of overtones 1..N_PROFILE relative to the strongest (nan = not found)
    centroid: float        # "brightness": amplitude-weighted mean overtone number
    decay: float           # dB per second
    n_partials: int


def find_peak(db: np.ndarray, fc: float, hw: float):
    """Strongest spectrum peak within fc +- hw Hz: (freq, dB, prominence over the local median) or None."""
    lo = max(1, int((fc - hw) / BIN))
    hi = min(len(db) - 2, int((fc + hw) / BIN) + 1)
    if hi <= lo + 4:
        return None
    i = lo + int(np.argmax(db[lo:hi]))
    if i <= lo or i >= hi - 1:  # on the edge of the search window: no real peak inside
        return None
    a, b, c = db[i - 1], db[i], db[i + 1]
    den = a - 2 * b + c
    p = 0.5 * (a - c) / den if den < 0 else 0.0  # parabolic interpolation
    return (i + p) * BIN, b - 0.25 * (a - c) * p, b - float(np.median(db[lo:hi]))


def fit_inharmonic(ks: np.ndarray, fs: np.ndarray, ws: np.ndarray) -> tuple[float, float]:
    """Least squares fit of f_k = k f0 sqrt(1 + B k^2): (f_k/k)^2 = f0^2 + f0^2 B k^2 is a line in k^2."""
    x = ks.astype(np.float64) ** 2
    y = (fs / ks) ** 2
    w = np.sqrt(ws)
    a = np.stack([np.ones_like(x), x], 1)
    c, *_ = np.linalg.lstsq(a * w[:, None], y * w, rcond=None)
    f0 = math.sqrt(max(c[0], 1.0))
    return f0, c[1] / max(c[0], 1.0)


def partials(x: np.ndarray, f0_guess: float):
    """Find the overtones one by one, predicting each from the fit of the ones found so far."""
    db = 20 * np.log10(np.abs(np.fft.rfft(x * np.hanning(len(x)), NFFT)) + 1e-12)
    floor = db.max() + FLOOR_DB
    found: list[tuple[int, float, float]] = []  # (k, freq, dB)
    f0, b = f0_guess, 0.0
    misses = 0
    for k in range(1, int(MAX_HZ / f0_guess) + 1):
        pk = find_peak(db, k * f0 * math.sqrt(1 + max(b, 0.0) * k * k), 0.25 * f0)
        if pk and pk[1] > floor and pk[2] > 12:
            found.append((k, pk[0], pk[1]))
            misses = 0
        else:
            misses += 1
            if misses >= 5 and k > 10:
                break
        if len(found) >= 5:
            arr = np.array(found)
            f0, b = fit_inharmonic(arr[:, 0], arr[:, 1], arr[:, 2] - floor + 1)
    if len(found) < 3:
        return f0_guess, float("nan"), found
    # One outlier pass: drop overtones more than 10 cents away from the fitted curve, then refit.
    arr = np.array(found)
    model = arr[:, 0] * f0 * np.sqrt(1 + max(b, 0.0) * arr[:, 0] ** 2)
    keep = np.abs(1200 * np.log2(arr[:, 1] / model)) < 10
    if keep.sum() >= 5:
        arr = arr[keep]
        f0, b = fit_inharmonic(arr[:, 0], arr[:, 1], arr[:, 2] - floor + 1)
    if arr[:, 0].max() < MIN_TOP_K:  # B is only measurable with high enough overtones
        b = float("nan")
    return f0, b, [tuple(r) for r in arr]


def pluck_features(x: np.ndarray, t: float, t_next: float, meta: dict, freq: float) -> Pluck | None:
    a = int((t + WIN_START_S) * SR)
    e = int(min(t + WIN_START_S + WIN_LEN_S, t_next - 0.01) * SR)
    e = min(e, len(x))
    if e - a < int(0.2 * SR):
        return None
    f0, b, found = partials(x[a:e], freq)
    if len(found) < 3:
        return None
    ks = np.array([r[0] for r in found], dtype=int)
    amps = np.array([r[2] for r in found])
    profile = np.full(N_PROFILE, np.nan)
    for k, amp in zip(ks, amps):
        if k <= N_PROFILE:
            profile[k - 1] = amp
    profile -= np.nanmax(profile)
    lin = 10 ** (amps / 20)
    centroid = float(np.sum(ks * lin) / np.sum(lin))
    # Decay: slope of the 20 ms RMS level from 50 ms after the pluck to the next one (max 1.5 s).
    d_end = min(int(min(t + 1.5, t_next - 0.02) * SR), len(x))
    hop = int(0.02 * SR)
    starts = range(int((t + 0.05) * SR), d_end - hop, hop)
    lv = [10 * math.log10(float(np.mean(x[i:i + hop] ** 2)) + 1e-12) for i in starts]
    decay = float(np.polyfit(np.arange(len(lv)) * 0.02, lv, 1)[0]) if len(lv) >= 5 else float("nan")
    return Pluck(meta["round"], meta["midi"], meta["string"], meta["fret"], f0,
                 math.log10(b) if not math.isnan(b) and b > 1e-8 else float("nan"), profile, centroid, decay, len(found))


def load_take(folder: Path) -> list[Pluck]:
    out = []
    for jp in sorted(folder.glob("*.json")):
        meta = json.loads(jp.read_text())
        x = load_wav(folder / meta["file"])
        times = meta["plucks"]
        for i, (t, fr) in enumerate(zip(times, meta["freqs"])):
            t_next = times[i + 1] if i + 1 < len(times) else len(x) / SR
            p = pluck_features(x, t, t_next, meta, fr)
            if p:
                out.append(p)
    return out


# ---------------------------------------------------------------------------------------------
# A. Calibrated per note: diagonal LDA (distance to each position's mean, per-feature variance)
# ---------------------------------------------------------------------------------------------

FEATURE_SETS = {
    "inharm": lambda p: np.array([p.log_b]),
    "tone": lambda p: np.concatenate([np.nan_to_num(p.profile, nan=PROFILE_FILL), [p.centroid]]),
    "decay": lambda p: np.array([p.decay]),
    "all": lambda p: np.concatenate([[p.log_b, p.decay, p.centroid], np.nan_to_num(p.profile, nan=PROFILE_FILL)]),
}


def classify_calibrated(train: list[Pluck], test: list[Pluck], feat) -> list[tuple[Pluck, tuple, float]]:
    """For each test pluck: (pluck, predicted (string, fret), margin). Candidates = positions in train."""
    classes = sorted({(p.string, p.fret) for p in train})
    xs = {c: np.array([feat(p) for p in train if (p.string, p.fret) == c]) for c in classes}
    mus = {c: np.nanmean(v, 0) for c, v in xs.items()}
    resid = np.concatenate([v - mus[c] for c, v in xs.items()])
    allx = np.concatenate(list(xs.values()))
    var = np.nanvar(resid, 0) + 0.05 * np.nanvar(allx, 0) + 1e-9  # pooled within-position variance, regularized
    out = []
    for p in test:
        x = feat(p)
        d = sorted((float(np.nanmean((x - mus[c]) ** 2 / var)), c) for c in classes)
        if math.isnan(d[0][0]):
            continue
        out.append((p, d[0][1], d[1][0] - d[0][0] if len(d) > 1 else 0.0))
    return out


# ---------------------------------------------------------------------------------------------
# B. Physics model calibrated on the open strings only
# ---------------------------------------------------------------------------------------------

K = np.arange(1, N_PROFILE + 1)
COMB_EPS = 0.03  # real notches aren't infinitely deep (the pickup "sees" a few mm of string)


def comb_model(rp, rq, alpha):
    """Overtone levels (dB, max 0) for a pickup at fraction rp and a pick at fraction rq of the string."""
    rp, rq, alpha = (np.asarray(v, dtype=np.float64)[..., None] for v in (rp, rq, alpha))
    v = (20 * np.log10(np.abs(np.sin(np.pi * K * rp)) + COMB_EPS)
         + 20 * np.log10(np.abs(np.sin(np.pi * K * rq)) + COMB_EPS) - alpha * 20 * np.log10(K))
    return np.maximum(v - v.max(-1, keepdims=True), PROFILE_FILL)


def profile_distance(profile: np.ndarray, model: np.ndarray) -> np.ndarray:
    """RMS difference over the overtones that were found, ignoring an overall gain offset."""
    ok = ~np.isnan(profile)
    d = model[..., ok] - profile[ok]
    d = d - d.mean(-1, keepdims=True)
    return np.sqrt(np.mean(d * d, -1))


@dataclass
class Physics:
    log_b_open: dict      # string -> log10 B of the open string
    sigma_b: float        # spread of log B between plucks of the same open string
    rp: float
    rq: float
    alpha: float
    sigma_p: float        # typical profile distance on the open strings

    def scores(self, p: Pluck, s: int, f: int) -> tuple[float, float]:
        """(inharmonicity z, profile z) of pluck p for the candidate position (s, f); lower = better."""
        zb = float("nan")
        if s in self.log_b_open and not math.isnan(p.log_b):
            pred = self.log_b_open[s] + f / 6 * math.log10(2)   # B ~ 1/L^2, L halves every 12 frets
            zb = abs(p.log_b - pred) / self.sigma_b
        scale = 2 ** (f / 12)                                   # pickup/pick fractions grow as L shrinks
        zp = float(profile_distance(p.profile, comb_model(self.rp * scale, self.rq * scale, self.alpha))) / self.sigma_p
        return zb, zp


def fit_physics(train: list[Pluck]) -> Physics | None:
    opens = [p for p in train if p.fret == 0]
    if not opens:
        return None
    log_b_open, resid = {}, []
    for s in sorted({p.string for p in opens}):
        v = np.array([p.log_b for p in opens if p.string == s])
        v = v[~np.isnan(v)]
        if len(v):
            log_b_open[s] = float(np.median(v))
            resid += list(v - log_b_open[s])
    sigma_b = max(float(np.std(resid)) if len(resid) > 2 else 0.1, 0.03)
    # Grid search of the pickup/pick fractions and the overall tilt that best explain the opens.
    grid = np.arange(0.02, 0.36, 0.005)
    rp, rq, al = (g.ravel() for g in np.meshgrid(grid, grid, np.arange(0, 2.01, 0.25), indexing="ij"))
    models = comb_model(rp, rq, al)
    total = sum(profile_distance(p.profile, models) ** 2 for p in opens)
    i = int(np.argmin(total))
    sigma_p = max(float(np.sqrt(total[i] / len(opens))), 0.5)
    return Physics(log_b_open, sigma_b, float(rp[i]), float(rq[i]), float(al[i]), sigma_p)


def classify_physics(ph: Physics, test: list[Pluck], cands: dict, mode: str):
    out = []
    for p in test:
        sc = []
        for s, f in cands[p.midi]:
            zb, zp = ph.scores(p, s, f)
            v = {"inharm": zb, "comb": zp, "both": (0 if math.isnan(zb) else zb) ** 2 + zp ** 2}[mode]
            if not math.isnan(v):
                sc.append((v, (s, f)))
        if len(sc) >= 2:
            sc.sort()
            out.append((p, sc[0][1], sc[1][0] - sc[0][0]))
    return out


# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------

def accuracy(res) -> float:
    return sum((p.string, p.fret) == pred for p, pred, _ in res) / len(res) if res else float("nan")


def coverage_line(res) -> str:
    """Accuracy when only the most confident X % of the answers are given (the rest = "not sure")."""
    r = sorted(res, key=lambda t: -t[2])
    parts = []
    for cov in (1.0, 0.75, 0.5):
        n = max(1, int(round(len(r) * cov)))
        parts.append(f"{int(cov * 100)}% answered -> {100 * accuracy(r[:n]):.0f}% right")
    return ",  ".join(parts)


def splits(plucks: list[Pluck]):
    """(train, test) pairs: one round against the other, or every pluck against the rest of its round."""
    rounds = sorted({p.round for p in plucks})
    if len(rounds) >= 2:
        return [([p for p in plucks if p.round == a], [p for p in plucks if p.round == b])
                for a in rounds for b in rounds if a != b], True
    return [([q for q in plucks if q is not p], [p]) for p in plucks], False


def cmd_analyze(args):
    folder = Path(args.folder)
    plucks = load_take(folder)
    if not plucks:
        raise SystemExit(f"No plucks found in {folder}")
    lines: list[str] = []
    say = lines.append
    multi = {m: sorted({(p.string, p.fret) for p in plucks if p.midi == m}) for m in sorted({p.midi for p in plucks})}
    multi = {m: c for m, c in multi.items() if len(c) >= 2}
    pickups = {json.loads(j.read_text()).get("pickup") for j in folder.glob("*.json")}
    say(f"Take: {folder}   plucks analysed: {len(plucks)}   pickup: {', '.join(map(str, pickups))}")
    say("Position names: s4f9 = string 4 (standard numbering, 1 = thinnest) fret 9.\n")

    say("Measured cues per position (mean +- spread over its plucks):")
    say(f"  {'note':5} {'pos':6} {'n':>3} {'log10 B':>14} {'brightness':>12} {'decay dB/s':>12} {'overtones':>9}")
    for m in sorted({p.midi for p in plucks}):
        for s, f in sorted({(p.string, p.fret) for p in plucks if p.midi == m}, key=lambda c: -c[0]):
            g = [p for p in plucks if p.midi == m and (p.string, p.fret) == (s, f)]
            lb = np.array([p.log_b for p in g])
            ce = np.array([p.centroid for p in g])
            de = np.array([p.decay for p in g])
            say(f"  {midi_name(m):5} {short_pos(s, f):6} {len(g):3} {np.nanmean(lb):8.2f} +-{np.nanstd(lb):4.2f} "
                f"{np.mean(ce):6.1f} +-{np.std(ce):3.1f} {np.nanmean(de):7.0f} +-{np.nanstd(de):3.0f} "
                f"{np.mean([p.n_partials for p in g]):9.0f}")

    pairs, cross_round = splits(plucks)
    say("\nA. Calibrated per note (every note recorded at every position on this guitar)")
    say("   " + ("trained on one round, tested on the other" if cross_round else
                 "only one round: each pluck tested against the others (OPTIMISTIC - record 2 rounds)"))
    say(f"   {'note':5} {'positions':>9} {'chance':>7} " + " ".join(f"{k:>7}" for k in FEATURE_SETS))
    overall = {k: [] for k in FEATURE_SETS}
    for m, cands in multi.items():
        row = []
        for name, feat in FEATURE_SETS.items():
            res = []
            for tr, te in pairs:
                res += classify_calibrated([p for p in tr if p.midi == m], [p for p in te if p.midi == m], feat)
            overall[name] += res
            row.append(f"{100 * accuracy(res):6.0f}%")
        say(f"   {midi_name(m):5} {len(cands):9} {100 / len(cands):6.0f}% " + " ".join(row))
    say(f"   {'all':5} {'':9} {'':7} " + " ".join(f"{100 * accuracy(r):6.0f}%" for r in overall.values()))
    say("   with every cue, answering only when sure: " + coverage_line(overall["all"]))
    say("\n   Confusions (every cue): true position -> how often each position was answered")
    for m, cands in multi.items():
        res = []
        for tr, te in pairs:
            res += classify_calibrated([p for p in tr if p.midi == m], [p for p in te if p.midi == m],
                                       FEATURE_SETS["all"])
        say(f"   {midi_name(m)}: " + "   ".join(
            short_pos(*c) + " -> " + " ".join(f"{short_pos(*d)}:{sum(1 for p, pr, _ in res if (p.string, p.fret) == c and pr == d)}"
                                             for d in cands) for c in cands))

    say("\nB. Physics model calibrated on the OPEN strings only (what the mod could ship)")
    phys_res = {k: [] for k in ("inharm", "comb", "both")}
    fitted = None
    for tr, te in (pairs if cross_round else [(plucks, plucks)]):
        ph = fit_physics(tr)
        if ph is None:
            continue
        fitted = fitted or ph
        te = [p for p in te if p.midi in multi]
        for mode in phys_res:
            phys_res[mode] += classify_physics(ph, te, multi, mode)
    if fitted is None:
        say("   no open-string plucks recorded: can't calibrate")
    else:
        say(f"   fitted: pickup-or-pick fractions {fitted.rp:.3f} and {fitted.rq:.3f} of the string, "
            f"tilt {fitted.alpha:.2f}; log10 B of the opens: "
            + ", ".join(f"{string_label(s, False)} {v:.2f}" for s, v in sorted(fitted.log_b_open.items(), reverse=True)))
        if not cross_round:
            say("   (only one round: calibrated and tested on the same plucks - OPTIMISTIC)")
        for mode, res in phys_res.items():
            fretted = [r for r in res if r[0].fret > 0]
            say(f"   {mode:7}: {100 * accuracy(res):3.0f}% right (fretted notes only: {100 * accuracy(fretted):3.0f}%)")
        say("   both, answering only when sure: " + coverage_line(phys_res["both"]))
    say("\nHow to read it: chance = guessing. Above ~90% (or ~95% on the most confident half) is good")
    say("enough to show a single red X; otherwise the mod keeps the faint alternatives.")
    text = "\n".join(lines)
    print(text)
    (folder / "report.txt").write_text(text + "\n", encoding="utf-8")
    print(f"\n(saved to {folder / 'report.txt'})")


# ---------------------------------------------------------------------------------------------
# Self test on synthetic strings (checks the code, says nothing about real guitars)
# ---------------------------------------------------------------------------------------------

def cmd_selftest(args):
    rng = np.random.default_rng(1)
    out = Path(args.out or tempfile.mkdtemp(prefix="string_id_selftest_"))
    out.mkdir(parents=True, exist_ok=True)
    b_open = [8e-5, 6e-5, 5e-5, 1.2e-4, 6e-5, 2.5e-5]   # made-up but plausible values
    rp = 0.065                                         # bridge pickup ~4 cm on a 65 cm string
    for rnd in (1, 2):
        for m, s, f in positions(STD_OPENS, TEST_NOTES, 15):
            onsets = [0.5 + 2.0 * i for i in range(5)]
            x = np.zeros(int(11 * SR))
            for i, t0 in enumerate(onsets):
                f0 = 440 * 2 ** ((m - 69) / 12) * 2 ** (rng.normal(0, 3) / 1200)
                b = b_open[s] * 2 ** (f / 6) * (1 + rng.normal(0, 0.05))
                rq = (0.13 + rng.normal(0, 0.015)) * 2 ** (f / 12)   # the pick moves a little each time
                n = int(2.0 * SR)
                t = np.arange(n) / SR
                y = np.zeros(n)
                for k in range(1, 200):
                    fk = k * f0 * math.sqrt(1 + b * k * k)
                    if fk > 6000:
                        break
                    amp = abs(math.sin(math.pi * k * rp * 2 ** (f / 12))) * abs(math.sin(math.pi * k * rq)) / k
                    y += amp * np.exp(-(1 + 0.3 * k * f0 / 200) * t) * np.sin(2 * np.pi * fk * t + rng.uniform(0, 6.3))
                y *= (1 - np.exp(-t / 0.002)) * 0.3 * rng.uniform(0.5, 1) / (np.abs(y).max() + 1e-9)
                a = int(t0 * SR)
                x[a:] = 0  # the new pluck stops the previous note
                x[a:a + n] += y[:len(x) - a]
            x += rng.normal(0, 10 ** (-70 / 20), len(x))
            stem = file_stem(rnd, m, s, f)
            save_wav(out / (stem + ".wav"), x)
            meta = {"file": stem + ".wav", "round": rnd, "midi": m, "string": s, "string_std": 6 - s, "fret": f,
                    "plucks": [t0 + 0.025 for t0 in onsets], "freqs": [440 * 2 ** ((m - 69) / 12)] * 5,
                    "pickup": "synthetic", "opens": STD_OPENS, "sr": SR}
            (out / (stem + ".json")).write_text(json.dumps(meta))
    print(f"Synthetic take written to {out}\n")
    args.folder = str(out)
    cmd_analyze(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["record", "check", "analyze", "selftest"])
    ap.add_argument("folder", nargs="?", help="analyze: the take's folder")
    ap.add_argument("--out", default=None, help="record: folder for the take (selftest: optional)")
    ap.add_argument("--pickup", default="unknown", help="record: which pickup (bridge, neck, middle, bridge+middle...)")
    ap.add_argument("--guitar", default="", help="record: a note about the guitar/strings (saved with the take)")
    ap.add_argument("--plucks", type=int, default=5, help="record: plucks per position and round")
    ap.add_argument("--rounds", type=int, default=2, help="record: times through every position (2 = train/test)")
    ap.add_argument("--max-fret", type=int, default=15)
    ap.add_argument("--seconds", type=float, default=30.0, help="check: seconds to record")
    ap.add_argument("--opens", default=",".join(map(str, STD_OPENS)), help="open strings' MIDI, thickest first")
    ap.add_argument("--device", type=int, default=None)
    ap.add_argument("--channel", type=int, default=0, help="Focusrite input channel: 0 = input 1, 1 = input 2")
    args = ap.parse_args()
    if args.mode == "record" and not args.out:
        ap.error("record needs --out <folder>")
    if args.mode == "analyze" and not args.folder:
        ap.error("analyze needs the take's folder")
    try:
        {"record": cmd_record, "check": cmd_check, "analyze": cmd_analyze, "selftest": cmd_selftest}[args.mode](args)
    except KeyboardInterrupt:
        print("\nStopped. Run the same command again to continue.")


if __name__ == "__main__":
    main()
