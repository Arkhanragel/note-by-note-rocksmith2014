"""
NoteTracker turns a stream of audio blocks into note EVENTS: "the player just played X".

It's the single piece of logic shared by live logging, offline WAV analysis and the
wait-mode game, and it's what we'll port to C++ for the in-game mod.

Time is counted in SAMPLES, not the wall clock, so running a recorded WAV through it
gives exactly the same result as the live run (this makes it testable).

When is an event emitted?
  - the pitch has been the same semitone for `stable` consecutive blocks, AND
  - it's a different note from the last reported one, OR a fresh pick attack happened
    since the last report (a repeated note).
Frames are ignored (unvoiced) when:
  - they're below the absolute gate (silence, hum), or
  - they're more than `rel_gate_db` below the loudest point since the last attack
    (the tail of a dying note, fret noise, finger squeak).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from pitch import OnsetDetector, yin


@dataclass
class NoteEvent:
    time: float         # seconds since the tracker started
    midi: int
    freq: float
    cents: float
    level_db: float
    aperiodicity: float
    attack: bool        # True if caused by a fresh pick attack


@dataclass
class TrackerConfig:
    sr: int = 48000
    block: int = 256
    window: int = 2048          # YIN window. Use 4096 for bass
    fmin: float = 70.0          # 35 for bass
    fmax: float = 1400.0
    threshold: float = 0.15     # YIN threshold
    gate_db: float = -45.0      # absolute gate
    rel_gate_db: float = 18.0   # ignore frames this far below the peak since the last attack
    stable: int = 3             # blocks needed after a pick attack (3 x 5.3 ms = 16 ms)
    # Blocks needed for a pitch change WITHOUT a pick attack (hammer-ons, pull-offs, slides).
    # It's stricter because finger noise and slides between frets produce short false
    # pitches with no attack (seen in the second real test).
    stable_legato: int = 6
    onset_ratio: float = 2.0


class NoteTracker:
    def __init__(self, cfg: TrackerConfig):
        self.cfg = cfg
        self.buf = np.zeros(cfg.window, dtype=np.float64)
        self.onset = OnsetDetector(cfg.sr, ratio=cfg.onset_ratio, gate_db=cfg.gate_db)
        self.samples = 0
        self.cur: int | None = None        # semitone seen in the last frame
        self.stable = 0
        self.reported: int | None = None   # last emitted semitone
        self.onset_pending = False         # an attack happened since the last event
        self.peak_db = -120.0              # loudest level since the last attack

    @property
    def now(self) -> float:
        return self.samples / self.cfg.sr

    def process(self, block: np.ndarray) -> NoteEvent | None:
        c = self.cfg
        n = len(block)
        self.buf = np.roll(self.buf, -n)
        self.buf[-n:] = block
        self.samples += n

        tail = self.buf[-1024:]  # ~21 ms, longer than one low-E period, so the level doesn't wobble
        lvl = 10 * np.log10(float(np.mean(tail * tail)) + 1e-12)

        if self.onset.process(self.buf, self.now * 1000):
            self.onset_pending = True
            self.peak_db = lvl
        self.peak_db = max(self.peak_db, lvl)

        voiced = lvl > c.gate_db and lvl > self.peak_db - c.rel_gate_db
        p = yin(self.buf, c.sr, fmin=c.fmin, fmax=c.fmax, threshold=c.threshold) if voiced else None
        m = round(p.midi) if p else None

        self.stable = self.stable + 1 if (m is not None and m == self.cur) else (1 if m is not None else 0)
        self.cur = m
        if m is None:
            self.reported = None  # a gap lets the same note be reported again
            return None

        needed = c.stable if self.onset_pending else c.stable_legato
        if self.stable >= needed and (m != self.reported or self.onset_pending):
            ev = NoteEvent(self.now, m, p.freq, (p.midi - m) * 100, lvl, p.aperiodicity, self.onset_pending)
            self.reported = m
            self.onset_pending = False
            return ev
        return None
