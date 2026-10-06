"""figures.py: the user guide's pictures, one function each (see draw.py for the pieces).

Every note, chord and song part in them is a made-up example: nothing here comes from a song.
The numbered marks are explained in docs/GUIDE.md under each picture.
"""
# draw.py is a small drawing vocabulary, used everywhere below: all of it is imported.
from draw import *  # noqa: F401,F403


def P_chordchip(name):
    """A chord in the "Then" row: its name in a gold frame."""
    w = tw(name, 15, 700) + 16
    return rect(0, -15, w, 30, PANEL, 7, 1, GOLD, 2) + T(w / 2, 0, name, 15, GOLD, 'middle', 700), w


def keys_line(x, y, extra=''):
    return T(x, y, extra + 'F6 = skip   F5 = menu', 14, DIM, w=400)


# ---------------------------------------------------------------- the whole screen
def fig_overview():
    W, H = 1040, 585
    g = ''
    # The game's highway (only hinted at: it is the game's own picture).
    vx, vy = 690, 300
    for i in range(6):
        bx = 430 + i * 118
        g += line(vx - 20 + i * 8, vy, bx, H, SC[i], 2, 0.4)
    for k, yy in enumerate((330, 372, 430, 510)):
        f = (yy - vy) / (H - vy)
        g += line(vx - 20 - f * 250, yy, vx + 20 + f * 330, yy, '#7fb8ff', 1.2, 0.25)
    g += rect(640, 396, 46, 16, SC[2], 4, 0.8) + rect(700, 470, 62, 20, SC[3], 5, 0.8)
    g += T(1016, 566, "the game's highway", 13, DIM, 'end', 400, 0.8)
    # The clock.
    g += rect(20, 18, 196, 34, PANEL, 8, 0.75)
    g += T(34, 35, [('0:13  /  2:58', TEXT), ('   Intro 1', GOLD)], 15, w=700)
    # The game's progress bar with the practice bar.
    bar, _ = gamebar(300, 20, 640, 40, [.5, .5, .6, .6, .7, .7, .8, .8, .8, .6, .6, .7, .9, .9, .7, .7, .5, .6, .6, .4],
                     {7: 0.85, 8: 0.5}, [(7, 9)], 0.12)
    g += bar
    # The banner.
    g += panel(300, 84, 440, 168, SC[2], 2.2, 10)
    g += T(318, 112, [('Play fret 5 on ', TEXT), ('string 4 (D)', SC[2])], 17, w=700)
    g += T(318, 196, 'Then', 10, DIM, w=400)
    for k, (st, f) in enumerate(((2, 7), (3, 5), (3, 7))):
        g += rect(350 + k * 30, 187, 18, 18, SC[st], 4) + T(359 + k * 30, 196, f, 10, INK if LIGHT[st] else '#fff', 'middle', 700)
    g += T(318, 234, 'F6 = skip   F5 = menu', 9.5, DIM, w=400)
    nk = neck(536, 122, 1, 6, s=0.5, zone=(2, 5), used=[2], bright=[5])
    g += nk.g + dot(nk, 2, 5)
    # The wrong-note panel.
    g += panel(752, 84, 252, 168, WARN, 2.2, 10)
    g += T(768, 108, '!  You played A', 14, WARN, w=700)
    g += T(768, 130, [('move DOWN 2 frets, to fret 5 on ', TEXT), ('string 4', SC[2])], 10.5, w=400)
    nk2 = neck(776, 146, 3, 8, s=0.5, used=[2], bright=[5])
    g += nk2.g + arrow(nk2.FX(7), nk2.Y(2), nk2.FX(5), nk2.Y(2), nk2.rad * 0.85, nk2.rad, 0.5) + xmark(nk2, 2, 7) + dot(nk2, 2, 5)
    # The tab: two rows.
    g += rect(20, 290, 568, 272, PANEL, 8, 0.72)
    seq1 = [(0.03, 2, 5), (0.15, 2, 7), (0.28, 3, 5), (0.40, 3, 7), (0.53, 3, 9), (0.65, 3, 7), (0.78, 2, 7), (0.90, 2, 5)]
    seq2 = [(0.03, 1, 7), (0.15, 2, 5), (0.28, 2, 7), (0.40, 3, 5), (0.53, 3, 4), (0.65, 2, 7), (0.78, 2, 5), (0.90, 1, 7)]
    n1 = [dict(t=t, st=st, f=f, stem=1, pick='down' if i % 2 == 0 else 'up', mark='g' if i < 2 else None, next=(i == 2))
          for i, (t, st, f) in enumerate(seq1)]
    n2 = [dict(t=t, st=st, f=f, stem=1, pick='down' if i % 2 == 0 else 'up') for i, (t, st, f) in enumerate(seq2)]
    s1, h1, _, _ = staff(28, 292, 550, 0.66, n1, bars=[(0, 12), (0.5, 13)], cursor=0.28)
    s2, _, _, _ = staff(28, 292 + h1 + 4, 550, 0.66, n2, bars=[(0, 14), (0.5, 15)])
    g += s1 + s2
    g += call(1, 236, 35) + call(2, 962, 40) + call(3, 280, 104) + call(4, 1020, 104) + call(5, 606, 310)
    g += call(6, 880, 520)
    return svg(W, H, 'The game screen with the clock, the practice bar, the banner, the wrong-note panel and the tab', g)


# ---------------------------------------------------------------- the three modes
_MODE_FRETS = [(2, 5), (2, 7), (3, 5), (3, 7), (3, 9)]  # the five notes of each row: (string, fret)


def _mode_note(x, y, st, f, fill=None):
    """A note of a row, as a tab box (fill: how it went)."""
    q = rect(x - 12, y - 12, 24, 24, PANEL, 5)
    if fill:
        q += rect(x - 12, y - 12, 24, 24, fill, 5, 0.62)
    return q + rect(x - 12, y - 12, 24, 24, 'none', 5, 1, SC[st], 1.8) + T(x, y, f, 13, TEXT, 'middle', 700)


def _mode_song(waits, y, end):
    """The song as an arrow of time under the notes; waits: it stops at the third note until it is played."""
    if not waits:
        return arrow(310, y + 30, end, y + 30, 0, 0, 1.3, '#d0d0dc', outline=False)
    g = line(310, y + 30, 610, y + 30, '#d0d0dc', 2.8)
    g += line(610, y + 30, 730, y + 30, AMBER, 2.8, 1, '3 7')
    g += arrow(730, y + 30, end, y + 30, 0, 0, 1.3, '#d0d0dc', outline=False)
    g += rect(601, y + 20, 7, 20, AMBER, 2) + rect(612, y + 20, 7, 20, AMBER, 2)
    return g + T(670, y + 50, 'waits until you play it', 12.5, AMBER, 'middle', 600)


def _mode_fill(waits, k):
    """"Wait for each note": the notes already played are green, the one the song waits at amber."""
    if not waits or k > 2:
        return None
    return GREEN if k < 2 else AMBER


def _mode_sign(x, y, st):
    """The banner, as a small sign over the note it names."""
    return (rect(x - 22, y - 44, 44, 22, PANEL, 5, 0.95, SC[st], 1.8)
            + f'<circle cx="{f1(x - 10)}" cy="{f1(y - 33)}" r="5" fill="{SC[st]}"/>' + line(x - 1, y - 33, x + 15, y - 33, TEXT, 2, 0.8))


def fig_modes():
    W, H = 1040, 340
    g = ''
    rows = [('Off', ['The game as usual. The tab and', 'the clock can still show.']),
            ('Show the notes', ['The banner names each note as it', 'comes. The song never stops.']),
            ('Wait for each note', ['The song stops at a note you have', 'not played, and goes on when you do.'])]
    for r, (name, sub) in enumerate(rows):
        waits = r == 2
        y = 62 + r * 106
        g += T(24, y - 14, name, 18, TEXT, w=700)
        for k, ln in enumerate(sub):
            g += T(24, y + 12 + k * 19, ln, 13.5, DIM, w=400)
        xs = [350, 480, 610, 800, 930] if waits else [350, 480, 610, 740, 870]
        end = 1010
        g += _mode_song(waits, y, end)
        for k, x in enumerate(xs):
            st, f = _MODE_FRETS[k]
            g += _mode_note(x, y, st, f, _mode_fill(waits, k))
            g += line(x, y + 12, x, y + 30, '#d0d0dc', 1.2, 0.5)
            if r == 1 or (waits and k == 2):
                g += _mode_sign(x, y, st)
        if not waits:
            g += T(end, y + 50, 'the song', 12.5, DIM, 'end', 400)
    return svg(W, H, 'The three modes: Off, Show the notes, Wait for each note', g)


def fig_timing():
    W, H = 1040, 250
    g = ''
    ax, x0 = 150, 600  # the axis' y, the note's time
    px = 1.2           # pixels per millisecond
    g += rect(x0 - 300 * px, ax - 44, 300 * px, 44, GREEN, 3, 0.3) + rect(x0, ax - 44, 150 * px, 44, GREEN, 3, 0.18)
    g += arrow(70, ax, 990, ax, 0, 0, 1.3, '#d0d0dc', outline=False) + T(990, ax + 20, 'time', 12.5, DIM, 'end', 400)
    g += line(x0, ax - 72, x0, ax + 8, GOLD, 2.4)
    g += rect(x0 - 13, ax - 98, 26, 26, PANEL, 5) + rect(x0 - 13, ax - 98, 26, 26, 'none', 5, 1, SC[3], 2) + T(x0, ax - 85, 7, 14, TEXT, 'middle', 700)
    g += T(x0 + 22, ax - 85, 'the note reaches the line', 13.5, GOLD, w=600)
    g += T(x0 - 150 * px, ax - 22, 'Early notes count', 14, TEXT, 'middle', 600)
    g += T(x0 + 75 * px, ax - 22, 'Late notes count', 14, TEXT, 'middle', 600)
    for ms, lab in ((-300, '300 ms before'), (0, 'on the beat'), (150, '150 ms after')):
        g += line(x0 + ms * px, ax - 5, x0 + ms * px, ax + 5, '#d0d0dc', 1.6) + T(x0 + ms * px, ax + 22, lab, 12.5, DIM, 'middle', 400)
    sx = x0 + 150 * px
    g += rect(sx - 9, ax - 66, 7, 20, AMBER, 2) + rect(sx + 2, ax - 66, 7, 20, AMBER, 2)
    g += T(sx + 22, ax - 65, 'not played by now:', 13.5, AMBER, w=600) + T(sx + 22, ax - 47, 'the song stops and waits', 13.5, AMBER, w=600)
    g += T(x0 - 75 * px, ax + 58, 'Played anywhere in the green part: it counts, and the song does not stop.', 14, TEXT, 'middle', 400)
    return svg(W, H, 'A right note counts from 300 ms before its time to 150 ms after it', g)


# ---------------------------------------------------------------- the banner: words and fretboard
def fig_banner_words():
    W, H = 1040, 372
    x0 = 64
    g = panel(24, 46, 992, 300, SC[3])
    r1, xs, _ = rowx(x0, 100, 16, [P_txt([('Play fret 10 on ', TEXT), ('string 3 (G)', SC[3])], 28, 700),
                                  P_badge('x2', SC[3], True, 30), P_pick('down', 11)])
    g += r1
    g += T(x0, 146, [('Hand:  ', GOLD, 600), ('move UP to fret 9 (index finger there)', TEXT)], 17, w=400)
    sep = P_txt('›', 20, 400, DIM)
    g += row(x0, 262, 10, [P_txt('Then', 15, 400, DIM), P_gap(2), P_chip(12, 3), P_txt('x2', 14, 700), sep, P_chip(10, 3), sep,
                           P_chip(12, 3, 'h')])
    g += keys_line(x0, 318)
    nk = neck(640, 84, 8, 13, zone=(9, 12), used=[3], bright=[10])
    dx, dy, rx = nk.FX(10), nk.Y(3), nk.FX(12)
    g += nk.g + arrow(dx, dy, rx, dy, nk.rad, nk.rad * 0.8) + ring(nk, 3, 12) + dot(nk, 3, 10, finger=2, name='F', side='left')
    g += hand(nk, 9, 2, SC[3], True)
    g += call(1, 42, 100) + call(2, xs[1] + 22, 66, xs[1] + 22, 83) + call(3, xs[2] + 11, 66, xs[2] + 11, 86)
    g += call(4, 42, 146) + call(5, dx, 68, dx, dy - 19) + call(6, rx + 26, 68, rx + 3, dy - 14)
    g += call(7, nk.FX(12) + 52, nk.y + nk.h + 17, nk.FX(12) + 14, nk.y + nk.h + 17)
    g += call(8, 42, 262) + call(9, 42, 318)
    return svg(W, H, 'The banner in its words-and-fretboard look, for a single note', g)


def fig_banner_chord():
    W, H = 1040, 332
    x0 = 64
    g = panel(24, 24, 992, 284, GOLD)
    g += row(x0, 74, 16, [P_txt([('Play the chord  ', TEXT), ('A5', GOLD)], 28, 700), P_badge('x4', GOLD, True, 30), P_pick('down', 11)])
    g += T(x0, 116, [('A power chord', GOLD), ('   ·   notes A and E', DIM)], 17, w=400)
    g += T(x0, 150, [('string ', DIM), ('6', SC[0], 700), (' fret 5', TEXT), (' = A', DIM), ('     ', TEXT), ('5', SC[1], 700),
                     (' fret 7', TEXT), (' = E', DIM), ('     ', TEXT), ('4', SC[2], 700), (' fret 7', TEXT), (' = A', DIM)], 17, w=400)
    sep = P_txt('›', 20, 400, DIM)
    g += row(x0, 226, 10, [P_txt('Then', 15, 400, DIM), P_gap(2), P_chordchip('C5'), P_txt('x2', 14, 700), sep, P_chip(7, 1), sep,
                           P_chordchip('G5')])
    g += keys_line(x0, 282, "x = don't play that string   ")
    nk = neck(640, 50, 3, 8, rad=10.5, zone=(5, 8), used=[0, 1, 2], bright=[5, 7])
    g += nk.g
    for i in (3, 4, 5):
        g += T(nk.FX(0), nk.Y(i), 'x', 17, DIM, 'middle', 700)
    g += dot(nk, 0, 5, finger=1) + dot(nk, 1, 7, finger=3) + dot(nk, 2, 7, finger=4)
    g += hand(nk, 5, {1, 3, 4}, GOLD, True)
    g += call(1, 42, 74) + call(2, 42, 116) + call(3, 42, 150)
    g += call(4, nk.FX(0) - 34, nk.Y(5) + 30, nk.FX(0) - 7, nk.Y(5) + 7)
    return svg(W, H, 'The banner for a chord: its name, what it is, each string and fret, and its shape on the fretboard', g)


def fig_banner_lines():
    W, H = 1040, 580
    x0 = 64
    g = ''
    # 1. A technique names itself in the first words; its line says how.
    p = 20
    g += panel(24, p, 992, 168, SC[3])
    g += row(x0, p + 46, 16, [P_txt([('Slide fret 7 on ', TEXT), ('string 3 (G)', SC[3])], 26, 700), P_pick('down', 10)])
    g += T(x0, p + 88, [('Slide:  ', GOLD, 600), ('then slide UP to fret 9, keep the string pressed', TEXT)], 16, w=400)
    g += keys_line(x0, p + 142)
    nk = neck(708, p + 22, 5, 10, s=0.8, zone=(7, 10), used=[3], bright=[7])
    g += nk.g + arrow(nk.FX(7), nk.Y(3), nk.FX(9), nk.Y(3), nk.rad, nk.rad * 0.8, 0.9, SC[3]) + ring(nk, 3, 9) + dot(nk, 3, 7, finger=1)
    g += call(1, 42, p + 46)
    # 2. A long note: how long to hold it, and a countdown once it is played.
    p = 206
    g += panel(24, p, 992, 168, SC[2])
    g += row(x0, p + 46, 16, [P_txt([('Hold fret 9 on ', TEXT), ('string 4 (D)', SC[2])], 26, 700), P_pick('down', 10)])
    g += T(x0, p + 88, [('Hold:  ', GOLD, 600), ('let it ring for ', TEXT), ('870 ms', TEXT, 700)], 16, w=400)
    lab = [('Keep holding ', TEXT), ('fret 9', SC[2])]
    g += T(x0, p + 142, lab, 14, w=400)
    bx = x0 + sum(tw(t, 14, 400) for t, _ in lab) + 12
    g += rect(bx, p + 137, 150, 9, PANEL, 4.5, 1, DIM, 1) + rect(bx, p + 137, 93, 9, SC[2], 4.5) + T(bx + 160, p + 142, '540 ms', 14, TEXT, w=700)
    nk = neck(708, p + 22, 7, 12, s=0.8, zone=(7, 10), used=[2], bright=[9])
    g += nk.g + dot(nk, 2, 9, finger=3)
    g += call(2, 42, p + 46) + call(3, 42, p + 142)
    # 3. A note of a held chord shape: the chord's other fingers stay down.
    p = 392
    g += panel(24, p, 992, 168, SC[4])
    g += row(x0, p + 46, 16, [P_txt([('Play fret 1 on ', TEXT), ('string 2 (B)', SC[4])], 26, 700), P_pick('up', 10)])
    g += T(x0, p + 88, [('Hold the shape:  ', GOLD, 600), ('keep your fingers on Am, pick its strings', TEXT)], 16, w=400)
    g += T(x0, p + 112, 'one by one', 16, TEXT, w=400)
    g += keys_line(x0, p + 142)
    nk = neck(708, p + 22, 1, 6, s=0.8, zone=(1, 4), used=[4], bright=[1])
    g += nk.g + ring(nk, 1, 0, 0.6) + ring(nk, 2, 2, 0.6) + ring(nk, 3, 2, 0.6) + ring(nk, 5, 0, 0.6) + dot(nk, 4, 1, finger=1)
    g += call(4, 42, p + 46)
    return svg(W, H, 'Three banners: a slide, a long note with its countdown, and a note inside a held chord shape', g)


# ---------------------------------------------------------------- the banner: cards
def cell(x, y, pickkind=None, mark=None, tie=False, cur=False, col=TEXT, k=1.0, op=1.0):
    """One repetition in a card's list: its pick sign and its mark; a tie = not picked again. Returns (svg, width)."""
    h = 22 * k
    wm = tw(mark, 12 * k, 700) if mark else 0
    w = (8 + (12 if pickkind else 0) + (4 if pickkind and mark else 0)) * k + wm + 8 * k
    if tie:
        w = 34 * k
    q = rect(x, y - h / 2, w, h, PANEL, 4 * k, 1, col if cur else '#8a8a96', (2.4 if cur else 1.3) * k)
    if cur:
        q += rect(x, y - h / 2, w, h, col, 4 * k, 0.16)
    if tie:
        a, b = x + 9 * k, x + w - 9 * k
        q += (f'<circle cx="{f1(a)}" cy="{f1(y + 3 * k)}" r="{f1(2.6 * k)}" fill="{TEXT}"/><circle cx="{f1(b)}" cy="{f1(y + 3 * k)}" '
              f'r="{f1(2.6 * k)}" fill="{TEXT}"/><path d="M{f1(a)} {f1(y - 2 * k)} Q{f1((a + b) / 2)} {f1(y - 9 * k)} {f1(b)} {f1(y - 2 * k)}" '
              f'fill="none" stroke="{TEXT}" stroke-width="{f1(1.7 * k)}" stroke-linecap="round"/>')
    else:
        cx = x + 8 * k
        if pickkind:
            q += pick_sign(cx + 6 * k, y, 5.5 * k, pickkind, TEXT, 2 * k)
            cx += 16 * k
        if mark:
            q += T(cx, y, mark, 12 * k, TEXT, w=700)
    return (f'<g opacity="{op}">{q}</g>' if op != 1 else q), w


def card(x, y, w, st, fret, lo, finger=None, count=None, pickkind=None, hl=False, dim=False, tag=None, cells=None,
         ns=0.6, ts=14.0):
    """One card: the name on top, the repeat counter, the list of a repeat's notes, a small fretboard."""
    head = 40 * ts / 14
    cellH = 30 * ts / 14 if cells is not None else 0
    nk = neck(x + (w - (38 + 30 + 6 * 46 + 20) * ns) / 2, y + head + cellH, lo, lo + 5, s=ns, zone=(lo + 1, lo + 4), used=[st], bright=[fret])
    h = head + cellH + nk.h + 10
    g = rect(x, y, w, h, PANEL, 10, 0.93)
    if hl:
        g += rect(x, y, w, h, SC[st], 10, 0.13) + rect(x, y, w, h, 'none', 10, 1, SC[st], 4)
    else:
        g += rect(x, y, w, h, 'none', 10, 1, '#5a5a66', 1.5)
    parts = [P_txt([(f'Fret {fret}', TEXT, 700), (' on ', DIM, 400), (f'string {6 - st} ({SN[st]})', SC[st], 700)], ts, 700)]
    if pickkind:
        parts.append(P_pick(pickkind, 6 * ts / 14, TEXT, 2.2 * ts / 14))
    q = row(x + 14, y + 22 * ts / 14, 9, parts)
    if count:
        bg, bw = P_badge(count, SC[st], LIGHT[st], 20 * ts / 14)
        q += f'<g transform="translate({f1(x + w - 12 - bw)},{f1(y + 22 * ts / 14)})">{bg}</g>'
    if cells:
        cx = x + 14
        for c in cells:
            cg, cw = cell(cx, y + head + 8 * ts / 14, col=SC[st], k=ts / 14, **c)
            q += cg
            cx += cw + 5 * ts / 14
    q += nk.g + dot(nk, st, fret, finger=finger, tag=tag)
    g += f'<g opacity=".5">{q}</g>' if dim else q
    return g, h


def fig_cards():
    W = 1040
    cw, gap, x0 = 240, 12, 22
    g = ''

    def strip(y, cards, now):
        q, h = '', 0
        for i, c in enumerate(cards):
            cg, h = card(x0 + i * (cw + gap), y, cw, hl=(i == now), dim=(i < now), **c)
            q += cg
        q += rect(x0, y + h + 10, 196, 26, PANEL, 13, 0.85) + T(x0 + 16, y + h + 23, 'F6 = skip   F5 = menu', 12.5, DIM, w=400)
        return q, h

    a = dict(st=3, fret=10, lo=8, finger=2, count='x2', pickkind='down')
    b = dict(st=3, fret=12, lo=8, finger=4, pickkind='up')
    c = dict(st=2, fret=10, lo=8, finger=2, pickkind='down')
    d = dict(st=2, fret=12, lo=8, finger=4, tag='h')
    e = dict(st=3, fret=9, lo=8, finger=1, pickkind='down')
    g += call(1, 34, 30) + T(54, 30, 'The frame is on the card to play.', 15, TEXT, w=600)
    s1, h = strip(52, [a, b, c, d], 0)
    g += s1
    y2 = 52 + h + 62
    g += call(2, 34, y2 + 6) + T(54, y2 + 6, 'You played it: the frame moves on, and the card it left already holds a later note (dimmed).', 15, TEXT, w=600)
    s2, h = strip(y2 + 28, [e, b, c, d], 1)
    g += s2
    return svg(W, int(y2 + 28 + h + 52), 'The banner as cards: the cards stand still and a frame moves from one to the next', g)


def fig_card_cells():
    W, H = 1040, 318
    cells = [dict(pickkind='down', mark='PM', cur=True), dict(pickkind='up', mark='PM'), dict(pickkind='down', mark='~'),
             dict(tie=True), dict(pickkind='up', mark='/')]
    cg, _ = card(24, 30, 344, 2, 12, 9, finger=4, count='x5', pickkind='down', hl=True, tag='PM', cells=cells, ns=0.86, ts=17)
    g = cg
    rows = [(cells[0], 'Pick down, palm-muted. The frame marks the one to play now.'),
            (cells[1], 'Pick up, palm-muted.'),
            (cells[2], 'Pick down, with vibrato.'),
            (cells[3], 'A tie: not picked again, the note before keeps ringing.'),
            (cells[4], 'Pick up, then slide up to a higher fret.')]
    for i, (c, words) in enumerate(rows):
        y = 58 + i * 50
        q, _ = cell(410, y, col=SC[2], k=1.6, **c)
        g += q + T(410 + 112, y, words, 15.5, TEXT, w=400)
    return svg(W, H, 'A card for a note played five times: under its name, one cell per repetition with its pick stroke and mark', g)


# ---------------------------------------------------------------- strings, fingers, marks
def fig_strings():
    W, H = 1040, 300
    g = T(40, 30, 'STRINGS', 12, DIM, w=700) + T(640, 30, 'PICK STROKES', 12, DIM, w=700) + T(640, 156, 'FINGERS', 12, DIM, w=700)
    notes = ['the thickest, lowest sound', '', '', '', '', 'the thinnest, highest sound']
    for i in range(6):
        y = 66 + i * 40
        g += line(40, y, 230, y, SC[i], 2 + 1.1 * (5 - i))
        g += T(252, y, f'string {6 - i} ({SN[i]})', 19, SC[i], w=700)
        if notes[i]:
            g += T(252 + tw(f'string {6 - i} ({SN[i]})', 19, 700) + 16, y + 1, notes[i], 12.5, DIM, w=400)
    g += pick_sign(660, 74, 13, 'down', TEXT, 3.4) + T(690, 74, 'down stroke', 17, TEXT, w=600)
    g += pick_sign(860, 74, 13, 'up', TEXT, 3.4) + T(890, 74, 'up stroke', 17, TEXT, w=600)
    g += T(640, 112, 'A fainter sign = suggested from the rhythm, not written in the song.', 12.5, DIM, w=400)
    for k, name in enumerate(['index', 'middle', 'ring', 'little']):
        x, y = 660 + (k % 2) * 200, 196 + (k // 2) * 46
        g += f'<circle cx="{x}" cy="{y}" r="13" fill="#f5f5f5" stroke="{INK}" stroke-width="1.5"/>' + T(x, y, k + 1, 17, INK, 'middle', 700)
        g += T(x + 26, y, name, 17, TEXT, w=600)
    return svg(W, H, 'The six strings with their colours, numbers and letters; the pick stroke signs; the finger numbers', g)


def fig_marks():
    W = 1040
    items = [('h', 'Hammer-on', 'no pick: hit the fret hard with a fretting finger'),
             ('p', 'Pull-off', 'no pick: pull the finger off so the note sounds'),
             ('T', 'Tap', 'hit the fret with a finger of the picking hand'),
             ('PM', 'Palm mute', 'the picking hand rests on the strings near the bridge'),
             ('X', 'Muted note', 'touch the string without pressing it: a dull click'),
             ('PH', 'Pinch harmonic', 'the thumb grazes the string as you pick it'),
             ('TP', 'Tremolo picking', 'pick the note very fast, again and again'),
             ('S', 'Slap', 'hit the string with the side of the thumb (bass)'),
             ('Pop', 'Pop', 'hook the string with a finger and let it snap back (bass)'),
             ('/', 'Slide up', 'to a higher fret, keeping the string pressed'),
             ('\\', 'Slide down', 'to a lower fret, keeping the string pressed'),
             ('b', 'Bend', 'push the string sideways so the note goes up'),
             ('~', 'Vibrato', 'shake the note a little while it rings'),
             ('>', 'Accent', 'play it louder than the others'),
             ('tie', 'Tie', 'not picked again: the note before keeps ringing'),
             ('down', 'Down stroke', 'the pick moves towards the floor'),
             ('up', 'Up stroke', 'the pick moves towards the ceiling'),
             ('x3', 'Repeat counter', 'play it this many times; it counts down as you play')]
    g = ''
    per = (len(items) + 1) // 2
    for i, (m, name, how) in enumerate(items):
        x, y = 30 + (i // per) * 505, 40 + (i % per) * 46
        cx = x + 28
        if m == 'tie':
            g += cell(cx - 27, y, tie=True, k=1.6)[0]
        elif m in ('down', 'up'):
            g += pick_sign(cx, y, 10, m, TEXT, 3)
        elif m == 'x3':
            bg, bw = P_badge('x3', SC[3], True, 26)
            g += f'<g transform="translate({f1(cx - bw / 2)},{y})">{bg}</g>'
        else:
            wt = tw(m, 11 * 1.7, 700) + 8 * 1.7
            g += tech_tag(cx - wt / 2, y, m, 1.7)[0]
        g += T(x + 74, y - 9, name, 16, TEXT, w=700) + T(x + 74, y + 11, how, 13, DIM, w=400)
    return svg(W, 40 + per * 46 + 6, 'The marks used on the cards, the Then row and the tab, with their names', g)


# ---------------------------------------------------------------- wrong notes, messages
def fig_wrong_note():
    W, H = 1040, 300
    g = panel(24, 24, 620, 252, SC[2])
    g += T(60, 68, [('Play fret 5 on ', TEXT), ('string 4 (D)', SC[2])], 26, w=700)
    sep = P_txt('›', 20, 400, DIM)
    g += row(60, 168, 10, [P_txt('Then', 15, 400, DIM), P_gap(2), P_chip(7, 2), sep, P_chip(5, 3), sep, P_chip(7, 3)])
    g += keys_line(60, 244)
    nk = neck(338, 96, 1, 6, s=0.8, zone=(2, 5), used=[2], bright=[5])
    g += nk.g + dot(nk, 2, 5, finger=4, name='G', side='left')
    g += panel(660, 24, 356, 252, WARN)
    g += T(682, 58, '!  You played A', 21, WARN, w=700)
    g += T(682, 90, 'move DOWN 2 frets, to fret 5 on', 15.5, TEXT, w=400) + T(682, 112, 'string 4 (D)', 15.5, SC[2], w=400)
    n2 = neck(690, 130, 3, 8, s=0.74, used=[2], bright=[5])
    g += n2.g + arrow(n2.FX(7), n2.Y(2), n2.FX(5), n2.Y(2), n2.rad * 0.85, n2.rad, 0.8)
    g += xmark(n2, 2, 7, 'A') + dot(n2, 2, 5, name='G', side='left')
    g += call(1, 40, 68) + call(2, 992, 58) + call(3, 992, 100) + call(4, n2.FX(7) + 62, n2.Y(2) - 34, n2.FX(7) + 8, n2.Y(2) - 9)
    return svg(W, H, 'After a wrong note: the banner stays as it is, and a red panel beside it says how to fix it', g)


def fig_messages():
    W, H = 1040, 230

    def toast(y, text):
        w = tw(text, 19, 700) + 44
        return rect(64, y - 23, w, 46, PANEL, 10, 0.9, '#3c3c46', 1) + T(64 + w / 2, y, text, 19, TEXT, 'middle', 700)

    g = toast(72, 'Note-by-Note ON  -  F5 menu') + call(1, 36, 72)
    g += toast(158, 'string 5 (A) sounds a bit low. Tune it up a little, to A') + call(3, 36, 158)
    cx = 880
    g += panel(cx - 100, 44, 200, 142, GOLD, 3) + T(cx, 72, 'the song goes on in', 15, DIM, 'middle', 400) + T(cx, 132, 3, 76, GOLD, 'middle', 700)
    g += call(2, cx - 124, 66)
    return svg(W, H, 'Short messages: the mode switched on, the count-in after a long wait, and a string that sounds out of tune', g)


# ---------------------------------------------------------------- the tab
def fig_tab():
    W, H = 1040, 378
    bt = lambda bar, beat: bar * 0.5 + (beat - 1) * 0.125 + 0.03
    notes = [
        dict(t=bt(0, 1), st=2, f=5, mark='g', pick='down', stem=1),
        dict(t=bt(0, 1.5), st=2, f=7, mark='g', pick='up', stem=1),
        dict(t=bt(0, 2), st=3, f=5, mark='a', pick='down', stem=1),
        dict(t=bt(0, 2.5), st=3, f=7, mark='r', pick='up', stem=1),
        dict(t=bt(0, 3), st=3, f=9, next=True, tail=bt(0, 3.85), pick='down', stem=1),
        dict(t=bt(0, 4), st=3, f=7, above='h', stem=1),
        dict(t=bt(0, 4.5), st=3, f=9, slide='/', pick='up', stem=1),
        dict(t=bt(1, 1), chord=[(0, 5), (1, 7), (2, 7)], name='A5', above='PM', pick='down', stem=1),
        dict(t=bt(1, 2), st=4, f=12, bend='1/2', pick='down', stem=1),
        dict(t=bt(1, 3), st=5, f=12, run='x8', tail=bt(1, 3.8), stem=1),
        dict(t=bt(1, 4), st=2, f=7, dots=(2, 3), pick='down', stem=1),
    ]
    beams = [(bt(0, 1), bt(0, 1.5), 1), (bt(0, 2), bt(0, 2.5), 1), (bt(0, 4), bt(0, 4.5), 1)]
    beats = [b * 0.125 for b in range(8) if b % 4]
    g = rect(16, 34, 1008, 304, PANEL, 8, 0.72)
    sg, _, tx, RowY = staff(40, 40, 964, 1.5, notes, bars=[(0, 12), (0.5, 13)], beats=beats, cursor=bt(0, 3),
                            sections=[(0, 'Verse 2')], beams=beams)
    g += sg
    top, bot, bh = 40 + 52 * 1.5, 40 + 52 * 1.5 + 150, 30 * 0.46
    lane1, lane2, cy, by = top - 36 * 1.5, top - 20 * 1.5, 18, H - 18
    g += call(1, 24, RowY(5) - 26, 51, RowY(5) - 6)
    g += call(2, tx(0) + 16, cy, tx(0) + 40, lane1 - 8)
    g += call(3, tx(bt(0, 1.5)) + 40, cy, tx(bt(0, 1.5)) + 6, lane2 - 9)
    g += call(5, tx(bt(0, 3)) - 34, cy, tx(bt(0, 3)) - 2, top - 16)
    g += call(7, tx(bt(0, 4)) + 30, cy, tx(bt(0, 4)) + 5, RowY(3) - bh - 20)
    g += call(8, tx(bt(1, 1)) + 44, cy, tx(bt(1, 1)) + 12, lane1 - 7)
    g += call(9, tx(bt(1, 2)) + 62, cy, tx(bt(1, 2)) + 34, RowY(4) - bh - 14)
    g += call(4, tx(bt(0, 2)) + 31, by, tx(bt(0, 2.25)), RowY(3) + bh + 5)
    g += call(6, tx(bt(0, 3.5)), by, tx(bt(0, 3.5)), RowY(3) + 6)
    g += call(12, tx(bt(0, 4.25)) + 40, by, tx(bt(0, 4.25)), bot + 50)
    g += call(10, tx(bt(1, 3.4)), by, tx(bt(1, 3.4)), RowY(5) + 6)
    g += call(11, tx(bt(1, 4)) + 34, by, tx(bt(1, 4)) + 10, RowY(2) + bh + 14)
    return svg(W, H, 'One row of the tab with its parts numbered', g)


def fig_tab_pages():
    W, H = 1040, 372
    g = T(28, 30, 'Pages, 2 rows', 16, TEXT, w=700) + T(676, 30, 'Scrolling', 16, TEXT, w=700)
    g += rect(16, 46, 628, 308, PANEL, 8, 0.72) + rect(664, 46, 360, 170, PANEL, 8, 0.72)
    base = [(2, 5), (2, 7), (3, 5), (3, 7), (3, 9), (3, 7), (2, 7), (2, 5), (1, 7), (2, 5)]
    r1 = [dict(t=0.04 + i * 0.1, st=st, f=f, stem=1, mark='g' if i < 5 else None, next=(i == 5)) for i, (st, f) in enumerate(base)]
    nxt = [(1, 5), (1, 7), (2, 5), (2, 7), (3, 5), (3, 7), (2, 7)]
    r2 = [dict(t=0.03, st=1, f=7, stem=1, op=0.45), dict(t=0.1, st=2, f=5, stem=1, op=0.45)]
    r2 += [dict(t=0.24 + i * 0.1, st=st, f=f, stem=1) for i, (st, f) in enumerate(nxt)]
    s1, h1, tx1, _ = staff(28, 44, 604, 0.8, r1, bars=[(0, 20), (0.5, 21)], cursor=0.54)
    s2, _, tx2, _ = staff(28, 44 + h1 + 4, 604, 0.8, r2, bars=[(0.2, 22), (0.7, 23)], recap=(0, 0.14))
    g += s1 + s2
    g += call(1, tx1(0.54), 44 + h1 - 8, tx1(0.54), 44 + h1 - 30)
    g += call(2, tx2(0.07), 44 + 2 * h1 - 6, tx2(0.07), 44 + 2 * h1 - 30)
    g += call(3, tx2(0.6), 44 + 2 * h1 - 6, tx2(0.6), 44 + 2 * h1 - 30)
    sc = [dict(t=0.04 + i * 0.16, st=st, f=f, stem=1, mark='g' if i < 1 else None, next=(i == 1)) for i, (st, f) in enumerate(base[:7])]
    s3, h3, tx3, _ = staff(676, 44, 336, 0.8, sc, cursor=0.2)
    g += s3 + arrow(tx3(0.95), 44 + h3 + 6, tx3(0.3), 44 + h3 + 6, 0, 0, 1.1, GOLD, outline=False)
    g += call(4, tx3(0.2), 44 + h3 + 6)
    g += T(676, 250, 'The line stays put and the notes', 13.5, DIM, w=400) + T(676, 270, 'move to it, like the highway.', 13.5, DIM, w=400)
    return svg(W, H, 'The two ways the tab can move: pages with a moving cursor, or scrolling past a fixed line', g)


# ---------------------------------------------------------------- practice
PHRASES = [.35, .35, .5, .5, .6, .6, .7, .7, .55, .9, .9, .6, .6, .7, .7, .95, .8, .6, .6, .5, .7, .7, .45, .35]


def fig_practice_bar():
    W, H = 1040, 318
    x, w = 120, 800
    g = T(24, 30, 'In the song', 16, TEXT, w=700)
    bar, e = gamebar(x, 62, w, 46, PHRASES, {9: 0.95, 10: 0.6, 15: 0.75}, [(9, 11)], 0.3)
    g += bar + pointer(e[10] + 8, 84)
    g += T(x + w, 130, 'practising Chorus 1 (1:16 - 1:31)', 13.5, GOLD, 'end', 400)
    g += call(1, (e[3] + e[4]) / 2, 34, (e[3] + e[4]) / 2, 74)
    g += call(3, e[10], 34, e[10], 60) + call(4, e[11] + 30, 34, e[11] + 4, 56)
    g += call(2, (e[15] + e[16]) / 2, 34, (e[15] + e[16]) / 2, 100)
    g += call(5, x + w + 22, 130)
    g += T(24, 190, "On the game's pause and Riff Repeater screens", 16, TEXT, w=700)
    bar2, e = gamebar(x, 222, w, 46, PHRASES, {9: 0.95, 10: 0.6, 15: 0.75}, [(9, 11)], 0.3, loop=(8, 12))
    g += bar2
    g += T(x + w, 290, 'Note-by-Note: practising Chorus 1 (1:16 - 1:31)   ·   click a part to remove it', 13.5, GOLD, 'end', 400)
    g += call(6, e[12] + 34, 196, e[12] + 3, 218)
    return svg(W, H, "The practice bar on the game's progress bar, in the song and on the Riff Repeater screen", g)


def fig_menu():
    W, H = 760, 624

    def body(x, y, w):
        q = m_sep(x, y, w, 'Show')
        q += m_check(x, y + 28, 'The note to play (the banner)')
        q += T(x + 22, y + 56, 'Look:', 13.5, TEXT, w=400)
        r, end = m_radio(x + 66, y + 56, 'Words and fretboard', True, help_=True)
        r2, _ = m_radio(end + 30, y + 56, 'Cards', False, help_=True)
        q += r + r2
        q += m_slider(x + 22, y + 84, 'Notes shown ahead', '3', 0.6, w - 22)
        q += m_check(x + 22, y + 112, 'Show it on a fretboard')
        q += m_check(x + 22, y + 140, 'Fingers and hand position')
        q += m_check(x + 44, y + 168, 'Draw the hand under the fretboard')
        q += m_check(x, y + 196, 'Song time') + m_check(x, y + 224, 'Practice bar')
        q += m_sep(x, y + 256, w, 'Arrange')
        for k, ln in enumerate(['While this menu is open, drag the banner, the wrong-note panel, the clock or the tab to move it,',
                                'and drag its bottom-right corner to resize it. The wrong-note panel follows the banner.',
                                'The menu itself moves by its title bar.']):
            q += T(x, y + 282 + k * 19, ln, 13, TEXT, w=400)
        q += m_button(x, y + 352, 'Reset positions and sizes')[0]
        return q

    mg, at = menu_window(20, 20, 720, 584, 3, body)
    g = mg + call(1, at['mode_end'] + 26, at['mode_y']) + call(2, at['song_end'] + 24, at['song_y'])
    g += call(3, at['skip_left'] - 20, at['song_y']) + call(4, at['tabs_end'] + 20, at['tabs_y'])
    g += call(5, 36 + 26 + tw('The note to play (the banner)', 13.5, 400) + 50, at['tabs_y'] + 62)
    g += call(6, at['close_left'] - 20, at['close_y'])
    return svg(W, H, 'The Note-by-Note menu (F5), on its Screen page', g)


def fig_practice_page():
    W, H = 760, 676

    def body(x, y, w):
        q = m_sep(x, y, w, 'Trouble spots')
        for k, ln in enumerate(["Where the song waited for you most (red on the practice bar, on the game's progress bar). A note",
                                'you play on time often enough in a row (below) is cleared; on the tab, the dots under a note fill',
                                'up green each time you play it right.']):
            q += T(x, y + 26 + k * 19, ln, 13, TEXT, w=400)
        for k, (name, heat, cleared) in enumerate([('Chorus 2  2:23', 0.9, '2 of 9 notes cleared'), ('Verse 3  1:16', 0.55, '4 of 6 notes cleared'),
                                                   ('Solo  3:02', 0.3, '0 of 12 notes cleared')]):
            yy = y + 96 + k * 28
            q += T(x, yy, name, 13.5, TEXT, w=400) + rect(x + 150, yy - 6, 186, 12, BG_FRAME, 2) + rect(x + 150, yy - 6, 186 * heat, 12, '#e03d33', 2)
            bw = tw('Practising', 13, 400) + 18  # a switch: gold and "Practising" while its phrase is a practice part
            q += T(x + 350, yy, cleared, 13, '#7a7a86', w=400)
            q += (m_button(x + w, yy, 'Practising', False, 'end', GOLD, INK, bw) if k == 0 else m_button(x + w, yy, 'Practise', False, 'end', wd=bw))[0]
        q += m_button(x, y + 184, 'Practise all of them')[0]
        q += m_slider(x, y + 216, 'Clear a note after', '3 good tries in a row', 0.22, w)
        q += m_sep(x, y + 250, w, 'This time')
        q += T(x, y + 276, '41 played on time, 12 waited for, 2 skipped; 3 notes cleared; the longest wait: 6.4 s at 2:25', 13, TEXT, w=400)
        q += m_sep(x, y + 308, w, 'Practice parts')
        q += T(x, y + 334, 'Waiting only in 2:23 - 2:41', 13, TEXT, w=400)
        q += m_button(x, y + 362, 'Practise the whole song')[0]
        fg, fw = m_button(x, y + 398, "Forget this song's trouble spots")
        q += fg + T(x + fw + 10, y + 398, '(?)', 12.5, '#7a7a86', w=400)
        return q

    mg, at = menu_window(20, 20, 720, 636, 1, body)
    by = at['tabs_y'] + 34
    g = mg + call(1, 36 + 124, by + 96) + call(2, 740 - 16 - 106, by + 96) + call(3, 36 + 124, by + 216) + call(4, 36 + 110, by + 250)
    g += call(5, 36 + 150, by + 308)
    return svg(W, H, "The menu's Practice page: this song's trouble spots, how this run went, and the practice parts", g)


def fig_themes():
    W, H = 1040, 196
    themes = [('Default', '#0e0e14', '#ffffff', '#afafb9', '#ffce54', '#8c1f1f'), ('High contrast', '#000000', '#ffffff', '#e6e6e6', '#ffdd00', '#3a3a3a'),
              ('Midnight', '#0a1224', '#ebf0ff', '#8ca0be', '#ffc85a', '#1e468c'), ('Vintage', '#1c140e', '#fff4e0', '#c0aa8c', '#ffb43c', '#6e3c14'),
              ('Paper', '#f5f3eb', '#141418', '#5a5a64', '#a86e00', '#3c5a96')]
    g = ''
    for i, (name, pan, text, dim, chord, menu) in enumerate(themes):
        x = 20 + i * 202
        g += rect(x, 20, 190, 126, pan, 10, 1, SC[2], 2.2)
        g += T(x + 14, 46, [('Play fret 7 on ', text), ('string 4', SC[2])], 14, w=700)
        g += T(x + 14, 72, [('Play the chord  ', text), ('A5', chord)], 14, w=700)
        g += T(x + 14, 98, 'F6 = skip   F5 = menu', 11.5, dim, w=400)
        g += rect(x + 14, 114, 58, 18, menu, 3) + T(x + 43, 123, 'Menu', 11, '#ffffff', 'middle', 400)
        g += T(x + 95, 168, name, 15, TEXT, 'middle', 600)
    return svg(W, H, 'The five colour themes: Default, High contrast, Midnight, Vintage, Paper', g)


FIGURES = {
    'overview': fig_overview, 'modes': fig_modes, 'timing': fig_timing,
    'banner-words': fig_banner_words, 'banner-chord': fig_banner_chord, 'banner-lines': fig_banner_lines,
    'cards': fig_cards, 'card-cells': fig_card_cells, 'strings': fig_strings, 'marks': fig_marks,
    'wrong-note': fig_wrong_note, 'messages': fig_messages, 'tab': fig_tab, 'tab-pages': fig_tab_pages,
    'practice-bar': fig_practice_bar, 'menu': fig_menu, 'practice-page': fig_practice_page, 'themes': fig_themes,
}
