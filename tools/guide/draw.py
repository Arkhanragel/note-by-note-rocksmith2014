"""draw.py: the drawing pieces of the user guide's pictures.

The guide (docs/GUIDE.md) is illustrated with drawings instead of screenshots: they show every case
without needing a song that has it, and they contain no song data. The pictures are SVG text built
here from a few pieces that copy the overlay's own look (mod/nbn/overlay.cpp): the fretboard with its
dots, the banner's rows, the tab's staff, the menu window, the game's progress bar.

Text widths: an SVG shown as an image can't measure its own text, and the viewer's font may differ
from ours. So every text is measured here (Segoe UI, through Pillow) and written with that width
(textLength): rows of mixed pieces (words, a badge, a pick sign) line up on any machine.
"""
import html
import os

try:
    from PIL import ImageFont
except ImportError:  # (the widths are then estimated)
    ImageFont = None

# String colours as in the mod (0 = the thickest string, low E): the game's highway colours.
SC = ['#e83434', '#f0ce28', '#3484f2', '#f68822', '#3ec44e', '#b658e4']
SN = ['E', 'A', 'D', 'G', 'B', 'e']
LIGHT = [False, True, False, True, True, False]  # dark digits on these
# The "Default" theme (theme.cpp).
PANEL, TEXT, DIM, GOLD, WARN, INK, MENU = '#0e0e14', '#ffffff', '#afafb9', '#ffce54', '#ff6e5a', '#141418', '#8c1f1f'
GREEN, AMBER, RED = '#3caf5a', '#e19628', '#d73c32'  # the tab's marks: on time, waited for, skipped
FONT = "'Segoe UI','Helvetica Neue',Arial,sans-serif"

_FILES = {400: 'segoeui.ttf', 600: 'seguisb.ttf', 700: 'segoeuib.ttf'}
_fonts = {}


def tw(text, size, w=600):
    """Width of `text` at this font size and weight."""
    if ImageFont is not None:
        try:
            if w not in _fonts:
                _fonts[w] = ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:\\Windows'), 'Fonts', _FILES[w]), 100)
            return _fonts[w].getlength(text) * size / 100.0
        except OSError:
            pass
    return len(text) * size * 0.54


def f1(v):
    return ('%.1f' % v).rstrip('0').rstrip('.')


def esc(t):
    return html.escape(str(t), quote=True).replace(' ', '&#160;')


def T(x, y, text, size, fill=TEXT, anchor='start', w=600, op=1):
    """Text centred on the line y. `text`: a string, or pieces [(text, fill[, weight])] in a row."""
    segs = text if isinstance(text, list) else [(str(text), fill)]
    segs = [(s[0], s[1], s[2] if len(s) > 2 else w) for s in segs]
    total = sum(tw(s[0], size, s[2]) for s in segs)
    x0 = x
    if anchor == 'middle':
        x0 = x - total / 2
    elif anchor == 'end':
        x0 = x - total
    out = ''
    for t, col, wt in segs:
        wd = tw(t, size, wt)
        pin = f' textLength="{f1(wd)}" lengthAdjust="spacingAndGlyphs"' if len(t) > 1 else ''
        opa = f' opacity="{op}"' if op != 1 else ''
        out += (f'<text x="{f1(x0)}" y="{f1(y + size * 0.35)}" font-size="{f1(size)}" font-weight="{wt}" '
                f'fill="{col}"{opa}{pin}>{esc(t)}</text>')
        x0 += wd
    return out


# ---------------------------------------------------------------- rows of pieces
# A piece is (svg drawn from x = 0, centred on y = 0; its width).

def P_txt(text, size, w=600, fill=TEXT, op=1):
    segs = text if isinstance(text, list) else [(text, fill)]
    return T(0, 0, text, size, fill, w=w, op=op), sum(tw(s[0], size, s[2] if len(s) > 2 else w) for s in segs)


def pick_sign(cx, cy, half, kind, col, thick):
    """The pick stroke's sign, as in printed music: a bracket open at the bottom = down, a V = up."""
    if kind == 'down':
        pts = [(cx - half, cy + half), (cx - half, cy - half), (cx + half, cy - half), (cx + half, cy + half)]
    else:
        pts = [(cx - half, cy - half), (cx, cy + half), (cx + half, cy - half)]
    p = ' '.join(f'{f1(a)},{f1(b)}' for a, b in pts)
    return f'<polyline points="{p}" fill="none" stroke="{col}" stroke-width="{f1(thick)}" stroke-linejoin="miter"/>'


def P_pick(kind, half, col=TEXT, thick=3):
    return pick_sign(half, 0, half, kind, col, thick), 2 * half


def P_badge(text, col, dark, h):
    """The repeat counter: "x4" in a pill."""
    fs = h * 0.62
    w = max(h * 1.5, tw(text, fs, 700) + h * 0.8)
    return (f'<rect x="0" y="{f1(-h / 2)}" width="{f1(w)}" height="{f1(h)}" rx="{f1(h / 2)}" fill="{col}"/>'
            + T(w / 2, 0, text, fs, INK if dark else '#fff', 'middle', 700)), w


def tech_tag(x, y, t, s=1.0):
    """A technique's mark in a small white tag (x, y = its left, its middle)."""
    fs = 11 * s
    w = tw(t, fs, 700) + 8 * s
    h = 15 * s
    return (f'<rect x="{f1(x)}" y="{f1(y - h / 2)}" width="{f1(w)}" height="{f1(h)}" rx="{f1(4 * s)}" fill="#f5f5f5" '
            f'stroke="{INK}" stroke-width="{f1(1.2 * s)}"/>' + T(x + w / 2, y, t, fs, INK, 'middle', 700)), w


def P_chip(fret, st, tag=None, sz=30):
    """A step of the banner's "Then" row: the fret in a square of its string's colour, the string's letter under it."""
    g = (f'<rect x="0" y="{f1(-sz / 2)}" width="{sz}" height="{sz}" rx="7" fill="{SC[st]}"/>'
         + T(sz / 2, 0, fret, sz * 0.55, INK if LIGHT[st] else '#fff', 'middle', 700)
         + T(sz / 2, sz / 2 + 10, SN[st], 11, SC[st], 'middle', 700))
    if tag:
        g += tech_tag(sz - 10, -sz / 2 - 2, tag)[0]
    return g, sz + (6 if tag else 0)


def P_gap(w):
    return '', w


def rowx(x, y, gap, parts):
    """The pieces one after the other from x, centred on y. Returns (svg, each piece's x, the end's x)."""
    g, xs = '', []
    for svg_, w in parts:
        xs.append(x)
        if svg_:
            g += f'<g transform="translate({f1(x)},{f1(y)})">{svg_}</g>'
        x += w + gap
    return g, xs, x - gap


def row(x, y, gap, parts):
    return rowx(x, y, gap, parts)[0]


# ---------------------------------------------------------------- boxes, callouts
def panel(x, y, w, h, stroke, sw=2.5, rx=14, fill=PANEL, fop=0.92):
    return (f'<rect x="{f1(x)}" y="{f1(y)}" width="{f1(w)}" height="{f1(h)}" rx="{rx}" fill="{fill}" '
            f'fill-opacity="{fop}" stroke="{stroke}" stroke-width="{sw}"/>')


def rect(x, y, w, h, fill, rx=0, op=1, stroke=None, sw=1):
    st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ''
    opa = f' opacity="{op}"' if op != 1 else ''
    return f'<rect x="{f1(x)}" y="{f1(y)}" width="{f1(w)}" height="{f1(h)}" rx="{f1(rx)}" fill="{fill}"{opa}{st}/>'


def line(x1, y1, x2, y2, col, sw=1.0, op=1, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    opa = f' opacity="{op}"' if op != 1 else ''
    return (f'<line x1="{f1(x1)}" y1="{f1(y1)}" x2="{f1(x2)}" y2="{f1(y2)}" stroke="{col}" stroke-width="{f1(sw)}"'
            f'{opa}{d} stroke-linecap="round"/>')


def call(n, x, y, tx=None, ty=None):
    """A numbered mark (explained in the guide's text under the picture), with a line to what it names."""
    g = ''
    if tx is not None:
        g += line(x, y, tx, ty, GOLD, 1.5, 0.9) + f'<circle cx="{f1(tx)}" cy="{f1(ty)}" r="2.6" fill="{GOLD}"/>'
    g += f'<circle cx="{f1(x)}" cy="{f1(y)}" r="11" fill="{GOLD}" stroke="{INK}" stroke-width="1.5"/>'
    return g + T(x, y, n, 13, INK, 'middle', 700)


def arrow(ax, ay, bx, by, ra=0, rb=0, s=1.0, col='#fff', bend=0, outline=True):
    """An arrow from a to b, starting ra from a and ending rb before b (bend curves it)."""
    import math
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy)
    if L < ra + rb + 10 * s:
        return ''
    ux, uy = dx / L, dy / L
    mx, my = (ax + bx) / 2 - uy * bend, (ay + by) / 2 + ux * bend
    d1, d2 = math.hypot(mx - ax, my - ay), math.hypot(bx - mx, by - my)
    sx, sy = ax + (mx - ax) / d1 * (ra + 3 * s), ay + (my - ay) / d1 * (ra + 3 * s)
    vx, vy = (bx - mx) / d2, (by - my) / d2
    ex, ey, hl = bx - vx * (rb + 4 * s), by - vy * (rb + 4 * s), 8 * s
    tx, ty = ex - vx * hl, ey - vy * hl
    path = f'M{f1(sx)} {f1(sy)} Q{f1(mx)} {f1(my)} {f1(tx)} {f1(ty)}'
    head = f'{f1(ex)},{f1(ey)} {f1(tx - vy * hl * 0.6)},{f1(ty + vx * hl * 0.6)} {f1(tx + vy * hl * 0.6)},{f1(ty - vx * hl * 0.6)}'
    g = ''
    if outline:
        g += (f'<path d="{path}" fill="none" stroke="{PANEL}" stroke-width="{f1(5.5 * s)}" stroke-linecap="round" opacity=".9"/>'
              f'<polygon points="{head}" fill="{PANEL}" stroke="{PANEL}" stroke-width="{f1(3 * s)}" stroke-linejoin="round"/>')
    return g + (f'<path d="{path}" fill="none" stroke="{col}" stroke-width="{f1(2.2 * s)}" stroke-linecap="round"/>'
                f'<polygon points="{head}" fill="{col}"/>')


def svg(w, h, label, inner):
    """A whole picture: a dark "game screen" behind the drawing, so it reads on a light or a dark page."""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" font-family="{FONT}" '
            f'role="img" aria-label="{html.escape(label, quote=True)}">'
            f'<defs><linearGradient id="nbn-bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1d1a30"/>'
            f'<stop offset="1" stop-color="#09090f"/></linearGradient></defs>'
            f'<rect width="{w}" height="{h}" rx="10" fill="url(#nbn-bg)"/>{inner}</svg>\n')


# ---------------------------------------------------------------- the fretboard
class Neck:
    """A piece of the neck, measured and drawn by neck(): .g = its svg, .FX(fret) and .Y(string) = where a
    fret and a string are, .w and .h = its size."""


def _neck_wood(n, gap, zone):
    """The wood, the hand's frets shaded (zone), inlays, fret wires and the nut (or a faint edge when the
    window starts higher up)."""
    s, lo, hi, top, bottom = n.s, n.lo, n.hi, n.yT, n.yB
    mid = (n.Y(0) + n.Y(5)) / 2
    g = rect(n.L, top, n.R - n.L, bottom - top, '#000', 4 * s, 0.45)
    if zone:
        a, b = max(lo, zone[0]), min(hi, zone[1])
        g += rect(n.L + (a - lo) * n.cell, top, (b - a + 1) * n.cell, bottom - top, '#fff', 3 * s, 0.1)
    for f in range(lo, hi + 1):
        k, cx = f % 12, n.FX(f)
        if k == 0:
            for dy in (-gap, gap):
                g += f'<circle cx="{f1(cx)}" cy="{f1(mid + dy)}" r="{f1(4.5 * s)}" fill="#fff" opacity=".15"/>'
        elif k in (3, 5, 7, 9):
            g += f'<circle cx="{f1(cx)}" cy="{f1(mid)}" r="{f1(4.5 * s)}" fill="#fff" opacity=".15"/>'
        fx = n.L + (f - lo + 1) * n.cell
        g += line(fx, top, fx, bottom, DIM, 1.6 * s, 0.5)
    if lo == 1:
        return g + line(n.L, top, n.L, bottom, TEXT, 5 * s, 0.85)
    return g + line(n.L, top, n.L, bottom, DIM, 1.6 * s, 0.35)


def _neck_strings(n, start, name_x, used):
    """The strings in their colours from x = start (the ones in use brighter), with their names centred on
    name_x (None = no names)."""
    g = ''
    for i in range(6):
        on = i in used
        g += line(start, n.Y(i), n.R, n.Y(i), SC[i], (1.3 + 0.45 * (5 - i)) * n.s, 1 if on else 0.45)
        if name_x is not None:
            g += T(name_x, n.Y(i), f'{6 - i} {SN[i]}', 14 * n.s, SC[i], 'middle', 400, 1 if on else 0.55)
    return g


def _neck_numbers(n, bright):
    """The fret numbers under the neck, the ones in `bright` bold."""
    g = ''
    for f in range(n.lo, n.hi + 1):
        b = f in bright
        g += T(n.FX(f), n.yB + 13 * n.s, f, 15 * n.s, TEXT if b else DIM, 'middle', 700 if b else 400, 1 if b else 0.75)
    return g


def neck(x, y, lo, hi, s=1.0, gap=22, cell=46, rad=14, names=True, tail=20, zone=None, used=(), bright=()):
    """A piece of the neck as the banner draws it: thickest string on top, strings in their colours,
    fret numbers below, the hand's frets shaded (zone). Returns a Neck: .g (svg), .FX(fret), .Y(string)..."""
    n = Neck()
    cells = hi - lo + 1
    gap, cell, rad = gap * s, cell * s, rad * s
    name_w, open_w, tail_w, top = (38 if names else 4) * s, 30 * s, tail * s, 18 * s
    left = x + name_w + open_w
    n.x, n.y, n.s, n.rad, n.cell, n.lo, n.hi = x, y, s, rad, cell, lo, hi
    n.L, n.R = left, left + cells * cell
    n.Y = lambda i: y + top + i * gap
    n.FX = lambda f: x + name_w + open_w * 0.5 if f == 0 else left + (f - lo + 0.5) * cell
    n.yT, n.yB = n.Y(0) - 9 * s, n.Y(5) + 9 * s
    n.w, n.h = name_w + open_w + cells * cell + tail_w, top + 5 * gap + 10 * s + 22 * s
    n.g = (_neck_wood(n, gap, zone) + _neck_strings(n, left - open_w * 0.55, x + name_w * 0.5 if names else None, used)
           + _neck_numbers(n, bright))
    return n


def finger_badge(cx, cy, r, s, n, op=1):
    bx, by, br = cx - r * 0.72, cy + r * 0.72, max(6 * s, r * 0.52)
    return (f'<g opacity="{op}"><circle cx="{f1(bx)}" cy="{f1(by)}" r="{f1(br)}" fill="#f5f5f5" stroke="{INK}" '
            f'stroke-width="{f1(1.3 * s)}"/>{T(bx, by, n, br * 1.45, INK, "middle", 700)}</g>')


def name_tag(cx, cy, r, s, letter, col, side='right'):
    """The note's name beside its dot ("F")."""
    w, h = 22 * s, 22 * s
    x = cx + r + 5 * s if side == 'right' else cx - r - 5 * s - w
    return (rect(x, cy - h / 2, w, h, PANEL, 6 * s, 1, col, 1.6 * s) + T(x + w / 2, cy, letter, 14 * s, col, 'middle', 700))


def dot(nk, st, f, finger=None, tag=None, name=None, side='right', op=1):
    """The note to play: a dot in the string's colour with the fret number inside."""
    cx, cy, r, s = nk.FX(f), nk.Y(st), nk.rad, nk.s
    g = (f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(r + 3 * s)}" fill="{SC[st]}" opacity=".27"/>'
         f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(r)}" fill="{SC[st]}"/>'
         + T(cx, cy, f, r * (1.25 if f < 10 else 1.0), INK if LIGHT[st] else '#fff', 'middle', 700))
    if finger:
        g += finger_badge(cx, cy, r, s, finger)
    if tag:
        g += tech_tag(cx + r * 0.45, cy - r - 5 * s, tag, s)[0]
    if name:
        g += name_tag(cx, cy, r, s, name, SC[st], side)
    return f'<g opacity="{op}">{g}</g>' if op != 1 else g


def ring(nk, st, f, op=1.0, k=0.8, label=True):
    """A fainter ring: the note that comes next, or the other fingers of a held chord shape."""
    cx, cy, r, s = nk.FX(f), nk.Y(st), nk.rad * k, nk.s
    g = (f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(r)}" fill="{PANEL}"/>'
         f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(r)}" fill="{SC[st]}" fill-opacity="{f1(0.24 * op)}" stroke="{SC[st]}" '
         f'stroke-width="{f1(2 * s)}" stroke-opacity="{op}"/>')
    if label:
        g += T(cx, cy, f, r * (1.15 if f < 10 else 0.95), TEXT, 'middle', 700, op)
    return g


def xmark(nk, st, f, name=None, side='right'):
    """Where a wrong note was played: a red X, with the note's name."""
    cx, cy, r, s = nk.FX(f), nk.Y(st), nk.rad * 0.85, nk.s
    k = r * 0.45
    g = (f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(r)}" fill="{PANEL}" stroke="{WARN}" stroke-width="{f1(2.2 * s)}"/>'
         + line(cx - k, cy - k, cx + k, cy + k, WARN, 2.6 * s) + line(cx - k, cy + k, cx + k, cy - k, WARN, 2.6 * s))
    if name:
        g += name_tag(cx, cy, r, s, name, WARN, side)
    return g


def hand(nk, first, active=None, col=TEXT, light=False):
    """The hand under the fretboard: four fingers over the frets they cover, the playing one in colour."""
    s, y = nk.s, nk.y + nk.h + 2 * nk.s
    g = rect(nk.FX(first) - 16 * s, y + 24 * s, nk.cell * 3 + 32 * s, 7 * s, '#fff', 3.5 * s, 0.25)
    for k in range(4):
        cx, on = nk.FX(first + k), (k + 1 in active if isinstance(active, (set, list, tuple)) else k + 1 == active)
        top, h = (y, 30 * s) if on else (y + 7 * s, 23 * s)
        g += rect(cx - 9 * s, top, 18 * s, h, col if on else '#3a3a44', 7 * s, 1, '#fff' if on else DIM, 1.4 * s)
        ink = INK if light else '#fff'
        g += T(cx, top + 10 * s, k + 1, 11 * s, ink if on else DIM, 'middle', 700)
    return g


# ---------------------------------------------------------------- the tab's staff
class _Staff:
    """A row of the tab, measured: where its strings, its lanes and its times are (see staff)."""

    def __init__(self, x, y, w, s, names):
        self.s = s
        self.gap = 20 * s
        self.left, self.right = x + (24 * s if names else 4 * s), x + w
        self.top = y + 52 * s
        self.lane1, self.lane2 = self.top - 36 * s, self.top - 20 * s  # chord names, sections, bar numbers; pick signs
        self.bot = self.top + 5 * self.gap
        self.bh = self.gap * 0.46  # half a fret box's height

    def row_y(self, st):
        return self.top + (5 - st) * self.gap

    def tx(self, t):
        return self.left + 16 * self.s + t * (self.right - self.left - 32 * self.s)


def _staff_lines(sf, x, names, beats, bars, recap):
    """The strings (with their letters), the beat lines and the bar lines with their numbers; under them
    the end of the page before, repeated (dimmed) at the start of a page."""
    s, top, bot, tx = sf.s, sf.top, sf.bot, sf.tx
    g = ''
    if recap:
        g += rect(tx(recap[0]) - 12 * s, top - 12 * s, tx(recap[1]) - tx(recap[0]) + 12 * s, 5 * sf.gap + 24 * s, '#fff', 4 * s, 0.07)
    for i in range(6):
        g += line(sf.left, sf.row_y(i), sf.right, sf.row_y(i), SC[i], 1.2 * s, 0.5)
        if names:
            g += T(x + 9 * s, sf.row_y(i), SN[i], 11 * s, SC[i], 'middle', 600, 0.9)
    for t in beats:
        g += line(tx(t), top - 6 * s, tx(t), bot + 6 * s, '#fff', 1 * s, 0.13)
    for t, num in bars:
        g += line(tx(t), top - 8 * s, tx(t), bot + 8 * s, '#fff', 1.6 * s, 0.55)
        if num is not None:
            g += T(tx(t) - 5 * s, sf.lane1, num, 9 * s, '#dcdce6', 'end', 400, 0.8)
    return g


def _staff_labels(sf, sections, brackets, ghost, cursor):
    """The sections' names, a held chord shape's name with a bracket over its notes, and the cursors."""
    s, top, bot, tx = sf.s, sf.top, sf.bot, sf.tx
    g = ''
    for t, name in sections:
        g += T(tx(t) + 4 * s, sf.lane1, name, 9.5 * s, GOLD, 'start', 700)
    for t0, t1, name in brackets:
        yb = sf.lane1
        wn = tw(name, 10 * s, 700)
        g += T(tx(t0), yb, name, 10 * s, GOLD, 'middle', 700)
        g += line(tx(t0) + wn / 2 + 5 * s, yb, tx(t1), yb, GOLD, 1.4 * s, 0.8) + line(tx(t1), yb, tx(t1), yb + 6 * s, GOLD, 1.4 * s, 0.8)
    if ghost is not None:
        g += line(tx(ghost), top - 12 * s, tx(ghost), bot + 12 * s, '#fff', 2 * s, 0.35)
    if cursor is not None:
        g += line(tx(cursor), top - 12 * s, tx(cursor), bot + 12 * s, '#fff', 2.4 * s)
    return g


def _fret_box(sf, nt, st, f, nx):
    """A note's box on its string: its tail, the box (coloured once the song passed it, framed in white
    when it is the note to play), the fret and a repeat's "x8". Returns (svg, half the box's width)."""
    s, bh, yy = sf.s, sf.bh, sf.row_y(st)
    label = str(f)
    run = nt.get('run')
    bw = (tw(label, 11.5 * s, 700) + (tw(run, 9 * s, 400) + 3 * s if run else 0)) / 2 + 5 * s
    q = ''
    if nt.get('tail') is not None:
        q += rect(nx, yy - 3 * s, sf.tx(nt['tail']) - nx, 6 * s, SC[st], 3 * s, 0.6)
    q += rect(nx - bw, yy - bh, 2 * bw, 2 * bh, PANEL, 4 * s)
    mark = {'g': GREEN, 'a': AMBER, 'r': RED}.get(nt.get('mark'))
    if mark:
        q += rect(nx - bw, yy - bh, 2 * bw, 2 * bh, mark, 4 * s, 0.62)
    if nt.get('next'):
        q += rect(nx - bw - 2 * s, yy - bh - 2 * s, 2 * bw + 4 * s, 2 * bh + 4 * s, 'none', 5 * s, 1, '#fff', 2.4 * s)
    else:
        q += rect(nx - bw, yy - bh, 2 * bw, 2 * bh, 'none', 4 * s, 1, SC[st], 1.7 * s)
    if run:
        lx = nx - bw + 5 * s
        q += T(lx, yy, label, 11.5 * s, TEXT, 'start', 700) + T(lx + tw(label, 11.5 * s, 700) + 3 * s, yy + 0.5 * s, run, 9 * s, DIM, 'start', 400)
    else:
        q += T(nx, yy, label, 11.5 * s, TEXT, 'middle', 700)
    return q, bw


def _box_marks(sf, nt, nx, yy, bw):
    """What the tab writes around a box: a slide, a bend with its steps, a harmonic's angle brackets, and
    under it the dots of a trouble note (played right, of those needed)."""
    s, bh = sf.s, sf.bh
    q = ''
    after = nx + bw + 2 * s
    if nt.get('slide'):
        up = nt['slide'] == '/'
        q += line(after, yy + (bh * 0.8 if up else -bh * 0.8), after + 8 * s, yy - (bh * 0.8 if up else -bh * 0.8), TEXT, 2 * s)
    if nt.get('bend'):
        ax, tp = after + 5 * s, yy - bh - 4 * s
        q += line(ax, yy - 2 * s, ax, tp + 4 * s, TEXT, 1.8 * s)
        q += f'<polygon points="{f1(ax)},{f1(tp - 2 * s)} {f1(ax - 4 * s)},{f1(tp + 5 * s)} {f1(ax + 4 * s)},{f1(tp + 5 * s)}" fill="{TEXT}"/>'
        q += T(ax + 4 * s, tp - 3 * s, nt['bend'], 9 * s, TEXT, 'start', 700)
    if nt.get('harm'):
        for side in (-1, 1):
            ex = nx + side * (bw + 3 * s)
            q += line(ex + side * 5 * s, yy - bh * 0.7, ex, yy, TEXT, 1.7 * s) + line(ex, yy, ex + side * 5 * s, yy + bh * 0.7, TEXT, 1.7 * s)
    if nt.get('dots'):
        got, need = nt['dots']
        for k in range(need):
            cx, cy = nx - (need - 1) * 4.6 * s + k * 9.2 * s, yy + bh + 6.5 * s
            q += f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(4.4 * s)}" fill="{PANEL}"/>'
            q += (f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(3.3 * s)}" fill="#46c864"/>' if k < got else
                  f'<circle cx="{f1(cx)}" cy="{f1(cy)}" r="{f1(2.9 * s)}" fill="none" stroke="#fff" stroke-width="{f1(1.4 * s)}" opacity=".8"/>')
    return q


def _staff_note(sf, nt):
    """A note or a chord of the row (see staff for its keys)."""
    s, bh = sf.s, sf.bh
    nx, op = sf.tx(nt['t']), nt.get('op', 1)
    strs = nt.get('chord') or [(nt['st'], nt['f'])]
    q = ''
    if len(strs) > 1:
        q += line(nx, sf.row_y(max(a for a, _ in strs)), nx, sf.row_y(min(a for a, _ in strs)), GOLD, 2 * s, 0.7)
    if nt.get('name'):
        q += T(nx, sf.lane1, nt['name'], 10 * s, GOLD, 'middle', 700)
    for st, f in strs:
        box, bw = _fret_box(sf, nt, st, f, nx)
        q += box + _box_marks(sf, nt, nx, sf.row_y(st), bw)
    if nt.get('above'):
        ty_ = sf.row_y(max(a for a, _ in strs)) - bh - 7 * s
        wa = tw(nt['above'], 9 * s, 700)
        q += rect(nx - wa / 2 - 2 * s, ty_ - 6 * s, wa + 4 * s, 12 * s, PANEL, 3 * s, 0.8) + T(nx, ty_, nt['above'], 9 * s, TEXT, 'middle', 700)
    if nt.get('pick'):
        q += pick_sign(nx, sf.lane2, 4.6 * s, nt['pick'], '#dcdce6', 1.8 * s)
    if nt.get('stem'):
        q += line(nx, sf.bot + 16 * s, nx, sf.bot + 32 * s, '#dcdce6', 1.5 * s, 0.9)
    return f'<g opacity="{op}">{q}</g>' if op != 1 else q


def staff(x, y, w, s, notes, bars=(), beats=(), cursor=None, ghost=None, sections=(), beams=(), names=True,
          recap=None, brackets=()):
    """A row of the tab: thinnest string on top (like printed tab). Times run 0..1 from left to right.
    notes: dicts with t, st (string, 0 = thickest), f (fret) and optionally: mark ('g' on time, 'a' waited
    for, 'r' skipped), tail (until this time), above ("PM", "h"...), slide ('/' or '\\'), bend ("1/2"),
    harm, pick ('down' / 'up'), next (the note to play), dots (played right, needed), run ("x8"),
    chord ([(st, f)...] with name), op (faded), stem (1 = a stem; beams are given apart).
    Returns (svg, height, tx, RowY)."""
    sf = _Staff(x, y, w, s, names)
    g = _staff_lines(sf, x, names, beats, bars, recap) + _staff_labels(sf, sections, brackets, ghost, cursor)
    for nt in notes:
        g += _staff_note(sf, nt)
    for t0, t1, lvl in beams:
        yy = sf.bot + 32 * s - (lvl - 1) * 5 * s
        g += line(sf.tx(t0), yy, sf.tx(t1), yy, '#dcdce6', 2.6 * s, 0.9)
    return g, 52 * s + 5 * sf.gap + 40 * s, sf.tx, sf.row_y


# ---------------------------------------------------------------- the game's progress bar
def gamebar(x, y, w, h, phrases, heat=None, parts=(), now=0.0, loop=None):
    """The game's progress bar (a block per phrase, taller = harder) with Note-by-Note's strip low in it:
    red where the song waited most, the practice parts in gold with their handles."""
    heat = heat or {}
    g = rect(x - 6, y - 4, w + 12, h + 8, '#000', 4, 0.55)
    n = len(phrases)
    edges = [x + w * i / n for i in range(n + 1)]
    for i, ph in enumerate(phrases):
        bh_ = (h - 14) * ph
        played = edges[i + 1] <= x + w * now
        g += rect(edges[i] + 1, y + h - 14 - bh_, edges[i + 1] - edges[i] - 2, bh_, '#b017d6' if played else '#4a2466', 1, 1)
    sy, sh = y + h - 12, 8
    g += rect(x, sy, w, sh, PANEL, 4, 0.75)
    for i, a in heat.items():
        g += rect(edges[i], sy, edges[i + 1] - edges[i], sh, '#e13c32', 0, a)
    for e in edges[1:-1]:
        g += line(e, sy + 2, e, sy + sh - 2, DIM, 1, 0.5)
    g += rect(x, sy, w * now, sh, '#fff', 4, 0.2) + line(x + w * now, sy - 3, x + w * now, sy + sh + 3, '#fff', 2)
    if loop:  # the game's own Riff Repeater selection
        for e, d in ((edges[loop[0]], 1), (edges[loop[1]], -1)):
            g += f'<path d="M{f1(e + d * 6)} {f1(y - 6)} H{f1(e)} V{f1(y + h + 6)} H{f1(e + d * 6)}" fill="none" stroke="#e6e6ee" stroke-width="2.5"/>'
    for a, b in parts:
        xa, xb = edges[a], edges[b]
        g += rect(xa, y, xb - xa, h, GOLD, 3, 0.27) + rect(xa, y, xb - xa, h, 'none', 3, 1, GOLD, 2)
        for e in (xa, xb):
            g += rect(e - 3, y - 4, 6, h + 8, GOLD, 2) + line(e, y, e, y + h, PANEL, 1.2)
    return g, edges


def pointer(x, y, s=1.0):
    return (f'<polygon points="{f1(x)},{f1(y)} {f1(x)},{f1(y + 18 * s)} {f1(x + 12 * s)},{f1(y + 13 * s)}" fill="#fff" '
            f'stroke="#000" stroke-width="{f1(1.5 * s)}" stroke-linejoin="round"/>')


# ---------------------------------------------------------------- the menu window
BG_FRAME = '#24242b'


def m_check(x, y, label, on=True, dis=False, help_=True):
    op = 0.45 if dis else 1
    g = rect(x, y - 9, 18, 18, MENU if on else BG_FRAME, 3)
    if on:
        g += f'<path d="M{f1(x + 4)} {f1(y)} l3.5 4 l6.5 -8" fill="none" stroke="#fff" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>'
    g += T(x + 26, y, label, 13.5, TEXT, w=400)
    if help_:
        g += T(x + 26 + tw(label, 13.5, 400) + 10, y, '(?)', 12.5, '#7a7a86', w=400)
    return f'<g opacity="{op}">{g}</g>'


def m_radio(x, y, label, on, size=13.5, w=400, help_=False):
    g = f'<circle cx="{f1(x + 9)}" cy="{f1(y)}" r="9" fill="{BG_FRAME}"/>'
    if on:
        g += f'<circle cx="{f1(x + 9)}" cy="{f1(y)}" r="5.5" fill="#fff"/>'
    g += T(x + 25, y, label, size, TEXT, w=w)
    end = x + 25 + tw(label, size, w)
    if help_:
        g += T(end + 10, y, '(?)', 12.5, '#7a7a86', w=400)
        end += 10 + tw('(?)', 12.5, 400)
    return g, end


def m_slider(x, y, label, value, frac, w, labelW=150):
    tw_ = w - labelW - 26
    gx = x + labelW + (tw_ - 22) * frac
    return (T(x, y, label, 13.5, TEXT, w=400) + rect(x + labelW, y - 10, tw_, 20, BG_FRAME, 3)
            + rect(gx, y - 8, 22, 16, '#c06a6a', 3) + T(x + labelW + tw_ / 2, y, value, 13, TEXT, 'middle', 400)
            + T(x + w - 18, y, '(?)', 12.5, '#7a7a86', w=400))


def m_sep(x, y, w, label):
    lw = tw(label, 13.5, 400)
    return line(x, y, x + 8, y, '#52525f', 1.2) + T(x + 14, y, label, 13.5, TEXT, w=400) + line(x + 22 + lw, y, x + w, y, '#52525f', 1.2)


def m_button(x, y, label, dis=False, anchor='start', fill='#771c1c', ink=TEXT, wd=None):
    wd = wd or tw(label, 13, 400) + 18
    if anchor == 'end':
        x -= wd
    return f'<g opacity="{0.5 if dis else 1}">' + rect(x, y - 11, wd, 22, fill, 3) + T(x + wd / 2, y, label, 13, ink, 'middle', 400) + '</g>', wd


def menu_window(x, y, w, h, active, body, song='Song notes read from the game (guitar, 25 levels)', mode=2):
    """The F5 menu: title bar, the mode, the song's line and Skip, the pages' tabs, a page, the footer.
    Returns (svg, places): places = x / y of the parts, for the callouts."""
    g = rect(x, y, w, h, '#0f0f14', 6, 1, '#3c3c46', 1) + rect(x, y, w, 26, MENU, 6) + rect(x, y + 14, w, 12, MENU)
    g += T(x + 10, y + 13, 'Note-by-Note', 13.5, TEXT, w=400)
    g += line(x + w - 19, y + 8, x + w - 9, y + 18, TEXT, 1.4) + line(x + w - 19, y + 18, x + w - 9, y + 8, TEXT, 1.4)
    px, py = x + 16, y + 50
    for i, lab in enumerate(['Off', 'Show the notes', 'Wait for each note']):
        rg, end = m_radio(px, py, lab, i == mode, 15, 700, help_=(i == 2))
        g += rg
        px = end + 22
    mode_end = px - 22
    g += T(x + 16, y + 80, song, 13, '#73d973', w=400)
    bg, bw = m_button(x + w - 16, y + 80, 'Skip this note (F6)', True, 'end')
    g += bg
    tx_, ty_ = x + 16, y + 110
    tabs = {}
    for i, lab in enumerate(['Playing', 'Practice', 'Tab', 'Screen', 'Colours', 'Game']):
        wd = tw(lab, 13.5, 400) + 16
        g += rect(tx_, ty_ - 12, wd, 24, MENU if i == active else '#4d1111', 3, 1 if i == active else 0.85)
        g += T(tx_ + wd / 2, ty_, lab, 13.5, TEXT, 'middle', 400)
        tabs[lab] = tx_ + wd / 2
        tx_ += wd + 4
    g += line(x + 16, ty_ + 12, x + w - 16, ty_ + 12, MENU, 1.2)
    g += body(x + 16, ty_ + 34, w - 32)
    fy = y + h - 58
    g += line(x + 16, fy, x + w - 16, fy, '#52525f', 1)
    g += T(x + 16, fy + 20, 'Saved automatically. The song is held while this menu is open.', 12.5, '#7a7a86', w=400)
    cg, cw = m_button(x + w - 16, fy + 20, 'Close (F5)', False, 'end')
    g += cg
    g += T(x + 16, fy + 44, 'Thanks to RS_ASIO, Rocksmith2014.NET, MinHook and Dear ImGui. Not affiliated with Ubisoft.', 10, '#7a7a86', w=400)
    return g, {'mode_y': y + 50, 'mode_end': mode_end, 'song_y': y + 80, 'song_end': x + 16 + tw(song, 13, 400),
               'skip_left': x + w - 16 - bw, 'tabs_y': ty_, 'tabs': tabs, 'tabs_end': tx_ - 4,
               'close_left': x + w - 16 - cw, 'close_y': fy + 20}
