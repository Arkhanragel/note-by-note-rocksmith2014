"""build_guide.py: makes the user guide's pictures and its web-page version.

    python tools/guide/build_guide.py                    the pictures: docs/guide/<name>.svg
    python tools/guide/build_guide.py --html <file>      also the guide as one web page (pictures inside,
                                                         opens without internet: for the release zip)
    python tools/guide/build_guide.py --fragment <file>  the same page without the <html>/<head> wrapper

The guide's text is docs/GUIDE.md (read on GitHub as it is); the web page is made from that same file,
so there is one text to keep up to date. Needs Pillow (text widths; see draw.py).
"""
import argparse
import html
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from figures import FIGURES  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(ROOT, 'docs')
PICS = os.path.join(DOCS, 'guide')


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ---------------------------------------------------------------- GUIDE.md -> HTML
# Only what the guide uses: headings, paragraphs, lists, tables, pictures, quotes, bold, code, links.

def slug(title):
    """A heading's anchor, made the way GitHub makes it."""
    t = re.sub(r'[^\w\- ]', '', title.lower())
    return t.replace(' ', '-')


def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', t)
    t = re.sub(r'(?<![\w*])\*([^*]+)\*(?![\w*])', r'<em>\1</em>', t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', t)
    return t


def to_html(md, pictures):
    """The guide's body, and its table of contents [(level, title, anchor)]."""
    out, toc = [], []
    lines = md.split('\n')
    i = 0
    para = []

    def flush():
        if para:
            out.append('<p>' + inline(' '.join(para)) + '</p>')
            para.clear()

    while i < len(lines):
        ln = lines[i]
        m = re.match(r'(#{1,3}) (.+)', ln)
        pic = re.match(r'!\[([^\]]*)\]\(guide/([\w\-]+)\.svg\)', ln)
        if m:
            flush()
            lvl, title = len(m.group(1)), m.group(2)
            if lvl > 1:
                toc.append((lvl, title, slug(title)))
            out.append(f'<h{lvl} id="{slug(title)}">{inline(title)}</h{lvl}>')
        elif pic:
            flush()
            art = re.sub(r' (width|height)="\d+"', '', pictures[pic.group(2)], count=2)
            out.append(f'<figure class="pic w{pic.group(2)}">{art}</figure>')
        elif ln.startswith('|'):
            flush()
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip().strip('|').split('|')])
                i += 1
            i -= 1
            head, body = rows[0], rows[2:]
            t = '<div class="table"><table><thead><tr>' + ''.join(f'<th>{inline(c)}</th>' for c in head) + '</tr></thead><tbody>'
            for r in body:
                t += '<tr>' + ''.join(f'<td>{inline(c)}</td>' for c in r) + '</tr>'
            out.append(t + '</tbody></table></div>')
        elif re.match(r'(\d+\.|-) ', ln):
            flush()
            ordered = ln[0].isdigit()
            items = []
            while i < len(lines) and (re.match(r'(\d+\.|-) ', lines[i]) or (lines[i].startswith('   ') and lines[i].strip())):
                if re.match(r'(\d+\.|-) ', lines[i]):
                    items.append(re.sub(r'^(\d+\.|-) ', '', lines[i]))
                else:
                    items[-1] += ' ' + lines[i].strip()
                i += 1
            i -= 1
            tag = 'ol' if ordered else 'ul'
            out.append(f'<{tag}>' + ''.join(f'<li>{inline(it)}</li>' for it in items) + f'</{tag}>')
        elif ln.startswith('> '):
            flush()
            quote = []
            while i < len(lines) and lines[i].startswith('>'):
                quote.append(lines[i][1:].strip())
                i += 1
            i -= 1
            out.append('<blockquote><p>' + inline(' '.join(quote)) + '</p></blockquote>')
        elif not ln.strip():
            flush()
        else:
            para.append(ln.strip())
        i += 1
    flush()
    return '\n'.join(out), toc


PAGE_HEAD = '''<title>Note-by-Note Guide</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Lexend:wght@500;600;700&family=Source+Sans+3:wght@400;600&display=swap">
<style>
/* Layout: a manual. A contents column that stays in view beside one reading column; the pictures are
   dark "game screens" in both themes, wider than the text so their small print stays readable. */
:root {
  --bg: #f6f5f9; --surface: #ffffff; --fg: #1d1b26; --dim: #5f5c6e; --line: #dcd9e6; --accent: #b0481a; --code: #ece9f3;
  --display: 'Lexend', 'Segoe UI', system-ui, sans-serif;
  --body: 'Source Sans 3', 'Segoe UI', system-ui, sans-serif;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { --bg: #0f0e15; --surface: #17161f; --fg: #eceaf3; --dim: #a5a2b6; --line: #2d2b3a; --accent: #f2a65a; --code: #23212e; color-scheme: dark; }
}
:root[data-theme="dark"] { --bg: #0f0e15; --surface: #17161f; --fg: #eceaf3; --dim: #a5a2b6; --line: #2d2b3a; --accent: #f2a65a; --code: #23212e; color-scheme: dark; }
body { background: var(--bg); color: var(--fg); font-family: var(--body); font-size: 17px; line-height: 1.55; }
.page { max-width: 1240px; margin: 0 auto; padding-inline: 20px; padding-block: 36px 72px; display: grid; grid-template-columns: 230px minmax(0, 1fr); gap: 48px; align-items: start; }
nav { position: sticky; top: calc(env(safe-area-inset-top, 0px) + 20px); max-height: calc(100vh - 40px); overflow-y: auto; font-size: 0.92rem; display: flex; flex-direction: column; gap: 2px; }
nav .t { font-family: var(--display); font-weight: 700; font-size: 0.78rem; letter-spacing: 0.09em; text-transform: uppercase; color: var(--dim); margin-bottom: 8px; }
nav a { color: var(--fg); text-decoration: none; padding: 3px 0; }
nav a.l3 { padding-left: 14px; color: var(--dim); font-size: 0.88rem; }
nav a:hover, nav a:focus-visible { color: var(--accent); }
main { min-width: 0; display: flex; flex-direction: column; gap: 14px; }
h1, h2, h3 { font-family: var(--display); text-wrap: balance; margin: 0; line-height: 1.2; }
h1 { font-size: clamp(1.9rem, 4.5vw, 2.7rem); font-weight: 700; }
h2 { font-size: 1.55rem; font-weight: 600; margin-top: 34px; padding-top: 26px; border-top: 1px solid var(--line); }
h3 { font-size: 1.12rem; font-weight: 600; margin-top: 16px; }
p, ul, ol, blockquote { margin: 0; max-width: 72ch; }
ul, ol { padding-left: 1.3em; display: flex; flex-direction: column; gap: 5px; }
a { color: var(--accent); }
a:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
code { background: var(--code); padding: 1px 5px; border-radius: 4px; font-size: 0.9em; }
blockquote { border-left: 3px solid var(--accent); padding-left: 14px; color: var(--dim); }
figure.pic { margin: 6px 0; overflow-x: auto; border-radius: 10px; }
figure.pic svg { display: block; width: 100%; height: auto; min-width: 640px; }
figure.wmenu svg, figure.wpractice-page svg { max-width: 760px; min-width: 560px; }
.table { overflow-x: auto; max-width: 100%; }
table { border-collapse: collapse; font-size: 0.95rem; min-width: 480px; }
th, td { text-align: left; vertical-align: top; padding: 7px 14px 7px 0; border-bottom: 1px solid var(--line); }
th { font-family: var(--display); font-weight: 600; font-size: 0.78rem; letter-spacing: 0.07em; text-transform: uppercase; color: var(--dim); }
td:first-child { white-space: nowrap; font-weight: 600; }
@media (max-width: 860px) {
  .page { grid-template-columns: minmax(0, 1fr); gap: 20px; }
  nav { position: static; max-height: none; border: 1px solid var(--line); border-radius: 8px; padding: 14px 16px; background: var(--surface); }
  td:first-child { white-space: normal; }
}
</style>
'''


def page(md, pictures):
    body, toc = to_html(md, pictures)
    nav = '<nav aria-label="Contents"><div class="t">Contents</div>' + ''.join(
        f'<a class="l{lvl}" href="#{a}">{html.escape(t)}</a>' for lvl, t, a in toc if lvl == 2) + '</nav>'
    return PAGE_HEAD + f'<div class="page">{nav}<main>{body}</main></div>\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--html')
    ap.add_argument('--fragment')
    ap.add_argument('--only', help='one picture, for a quick look')
    a = ap.parse_args()
    pictures = {}
    for name, fn in FIGURES.items():
        if a.only and name != a.only:
            continue
        pictures[name] = fn()
        write(os.path.join(PICS, name + '.svg'), pictures[name])
    print(f'{len(pictures)} pictures -> {PICS}')
    if a.html or a.fragment:
        with open(os.path.join(DOCS, 'GUIDE.md'), encoding='utf-8') as f:
            frag = page(f.read(), pictures)
        if a.fragment:
            write(a.fragment, frag)
        if a.html:
            write(a.html, '<!doctype html>\n<html lang="en"><head><meta charset="utf-8">'
                          '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
                          + frag.replace('<div class="page">', '</head><body style="margin:0">\n<div class="page">', 1) + '</body></html>\n')


if __name__ == '__main__':
    main()
