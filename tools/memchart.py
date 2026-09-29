"""memchart.py: build a Note-by-Note chart (v2 text format) from the game's MEMORY, while a song is
playing. Prototype of what the mod does in C++ (mod/nbn/game.cpp ReadSongChart), used to check it
against the charts ChartDump exports from the song files:

    python tools/memchart.py > mem.nbn          then compare with charts/nbn/<song>/<arrangement>.nbn

Layout (verified on the Dec 2024 build, see docs/TECHNICAL.md, "Object layouts"):
  song data = [song + 0x78]  (the SNG file, loaded; std::vector = begin, end, capacity)
    +0x40 levels vector (Level = 0x64 bytes, in difficulty order: +0x30 notes vector)
    +0x64 phrase iterations (0x18 bytes: phraseId, start, end, ...)
    +0x94 chord templates (0x48 bytes: mask, frets[6], fingers[6], MIDI notes[6] int32, name[32])
    +0x110 tuning vector<int16> (6), +0x11C capo (int8, -1 = none)
  Note = 0x1C8 bytes: +0x0 mask, +0xC time, +0x10 string, +0x11 fret, +0x14 chordId, +0x20 phrase iteration
"""
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
import memread  # noqa: E402

GUITAR = [40, 45, 50, 55, 59, 64]
MASK_CHORD, MASK_IGNORE = 0x2, 0x40000


def vec(g, addr, size):
    b, e = struct.unpack("<2I", g.read(addr, 8))
    n = (e - b) // size
    raw = g.read(b, n * size) if n else b""
    return [raw[i * size:(i + 1) * size] for i in range(n)]


def sounding(fret, capo):
    """With a capo, an open string sounds at the capo fret."""
    return capo if (fret == 0 and capo > 0) else fret


def chord_offsets(chords, tuning, capo):
    """Bass or guitar: the chord templates store MIDI notes computed with the bass offset (-12). This is
    every template note minus what a guitar in this tuning plays there: {-12} = bass, {0} = guitar."""
    offs = set()
    for c in chords:
        frets = struct.unpack("<6b", c[4:10])
        notes = struct.unpack("<6i", c[16:40])
        for s in range(6):
            if frets[s] >= 0:
                offs.add(notes[s] - (GUITAR[s] + tuning[s] + sounding(frets[s], capo)))
    return offs


def level_notes(g, levels):
    """The raw notes (0x1C8 bytes each) of every level, in order (+0x0 isn't the difficulty in memory)."""
    out = []
    for lv in levels:
        b, e = struct.unpack("<2I", lv[0x30:0x38])
        n = (e - b) // 0x1C8
        raw = g.read(b, n * 0x1C8) if n else b""
        out.append([raw[i * 0x1C8:(i + 1) * 0x1C8] for i in range(n)])
    return out


def note_line(diff, nt, chords, tuning, capo, bass):
    """One note as a chart line: "C ..." for a chord, "N ..." for a single note."""
    mask = struct.unpack("<I", nt[:4])[0]
    t = struct.unpack("<f", nt[0xC:0x10])[0]
    string, fret = struct.unpack("<bb", nt[0x10:0x12])
    chord = struct.unpack("<i", nt[0x14:0x18])[0]
    pi = struct.unpack("<i", nt[0x20:0x24])[0]
    ign = 1 if mask & MASK_IGNORE else 0
    if chord >= 0 and mask & MASK_CHORD:
        c = chords[chord]
        cn = [m for m, f in zip(struct.unpack("<6i", c[16:40]), struct.unpack("<6b", c[4:10])) if f >= 0]
        return "C %d %d %.3f %d %d %s" % (diff, pi, t, ign, len(cn), " ".join(map(str, cn)))
    midi = (GUITAR[string] - (12 if bass else 0)) + tuning[string] + sounding(fret, capo)
    return "N %d %d %.3f %d %d %d %d" % (diff, pi, t, midi, string, fret, ign)


def main():
    g = memread.Game()
    d = g.u32(g.song() + 0x78)
    tb, te = struct.unpack("<2I", g.read(d + 0x110, 8))
    tuning = list(struct.unpack("<%dh" % ((te - tb) // 2), g.read(tb, te - tb)))
    capo = struct.unpack("<b", g.read(d + 0x11C, 1))[0]
    chords = vec(g, d + 0x94, 0x48)
    levels = vec(g, d + 0x40, 0x64)
    pis = vec(g, d + 0x64, 0x18)

    offs = chord_offsets(chords, tuning, capo)
    bass = offs == {-12}
    print("# memory chart; chord MIDI offsets seen: %s" % sorted(offs), file=sys.stderr)
    print("bass %d" % bass)
    print("tuning " + " ".join(map(str, tuning)))
    print("levels %d" % len(levels))
    lvnotes = level_notes(g, levels)
    for diff, notes in enumerate(lvnotes):
        print("count %d %d" % (diff, len(notes)))
    for i, p in enumerate(pis):
        pid, start, end = struct.unpack("<iff", p[:12])
        print("pi %d %d %.3f %.3f" % (i, pid, start, end))
    for diff, notes in enumerate(lvnotes):
        for nt in notes:
            print(note_line(diff, nt, chords, tuning, capo, bass))


if __name__ == "__main__":
    main()
