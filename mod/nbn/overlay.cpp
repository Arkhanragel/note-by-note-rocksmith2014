// overlay.cpp: draws Note-by-Note's banner, messages and menu inside the game (see overlay.h).
//
// Steps:
//  1. InstallThread: wait for d3d9.dll, create a throw-away Direct3D device just to learn where
//     d3d9.dll's Present/Reset functions are (every device of the same kind shares that code), then
//     hook them with MinHook. The game's code isn't touched (VMProtect forbids it); d3d9.dll's is fine.
//  2. HkPresent runs on the game's render thread right before each frame is shown. The first call
//     sets up Dear ImGui for the game's device and window; every call draws our things on top.
//  3. HkReset: when the game resets the device (resolution change, alt-tab in fullscreen), ImGui's
//     GPU objects must be released before and recreated after, as with any D3D9 app.
//  4. HkWndProc: we subclass the game window. While OUR menu is open, keyboard/mouse messages go to
//     ImGui and are hidden from the game; otherwise everything passes through untouched.
#include "overlay.h"

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "log.h"
#include "music.h"

// Declared (commented out) in imgui_impl_win32.h so it doesn't drag <windows.h> into the header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nbn::overlay {
namespace {

// ------------------------------------------------------------------ state shared with the main loop
struct Shared {
    std::mutex m;
    View view;
    Settings settings;
    std::string toast;
    DWORD toastUntil = 0;
    bool skipRequest = false;
    bool forgetRequest = false;  // the menu's "Forget this song's trouble spots"
    int calibrationRequest = 0;  // 1 = start, 2 = cancel (TakeCalibrationRequest)
    std::vector<Range> ranges;  // the practice parts (song seconds), sorted; empty = the whole song
} g;
std::atomic<bool> g_menuOpen{false};

// The mouse, for the practice bar (it works without the menu). The window hook records what it sees;
// the render thread does the dragging. The bar's box (client pixels) says which clicks are ours.
std::atomic<int> g_mouseX{-10000}, g_mouseY{-10000};
std::atomic<bool> g_mouseLeft{false};    // left button held after a press on the bar
std::atomic<DWORD> g_mouseMoved{0};      // when the mouse last moved (the pointer shows for a moment)
std::atomic<int> g_barL{0}, g_barT{0}, g_barR{0}, g_barB{0};  // 0 x 0 = no bar
std::atomic<bool> g_clearDrawRanges{false};  // a new song: the render thread forgets its parts
bool OverBar(int x, int y) { return g_barR > g_barL && x >= g_barL && x <= g_barR && y >= g_barT && y <= g_barB; }

// Copies the layout fields (positions and sizes) only, so a drag never undoes a menu change.
void CopyLayout(const Settings& from, Settings* to) {
    to->bannerX = from.bannerX;
    to->bannerY = from.bannerY;
    to->bannerSize = from.bannerSize;
    to->clockX = from.clockX;
    to->clockY = from.clockY;
    to->clockSize = from.clockSize;
    to->tabX = from.tabX;
    to->tabY = from.tabY;
    to->tabWidth = from.tabWidth;
    to->tabSize = from.tabSize;
    to->mistakeX = from.mistakeX;
    to->mistakeY = from.mistakeY;
    to->mistakeSize = from.mistakeSize;
}

// ------------------------------------------------------------------ movable parts
// The parts the player can drag while the menu is open. Each Draw* function records where it drew
// its part (render thread only); the next frame's mouse handling hit-tests those boxes.
enum Part { kBanner, kClock, kTab, kMistake, kParts };
const char* kPartName[kParts] = {"Banner", "Clock", "Tab", "Wrong note"};
struct Box {
    ImVec2 p0, p1;
    bool drawn = false;  // drawn this frame
};
Box g_box[kParts];

struct Drag {
    int part = -1;       // -1 = no drag
    bool resize = false; // grabbed by the corner
    ImVec2 mouse0;       // where the drag started
    Box box0;            // the part's box then
    Settings st0;        // and the settings then
} g_drag;

// Where a part of size w x h goes, given the top-left corner the settings ask for: always fully on
// screen, on whole pixels.
ImVec2 Place(float x, float y, float w, float h, ImVec2 ds) {
    return ImVec2(std::floor(std::max(0.0f, std::min(ds.x - w, x))), std::floor(std::max(0.0f, std::min(ds.y - h, y))));
}

// ------------------------------------------------------------------ render-thread state
using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
PresentFn oPresent = nullptr;  // "o" = the original function (MinHook's trampoline)
ResetFn oReset = nullptr;
void* g_presentAddr = nullptr;
void* g_resetAddr = nullptr;

std::recursive_mutex g_imgui;  // ImGui isn't thread-safe: render thread + window thread take this
std::atomic<bool> g_ready{false};  // ImGui set up for the game's device/window
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<bool> g_shutdownDone{false};
std::atomic<int> g_inFlight{0};  // threads currently inside one of our hooks (for a safe unload)
HANDLE g_installThread = nullptr;

IDirect3DDevice9* g_dev = nullptr;
HWND g_hwnd = nullptr;
std::atomic<HWND> g_gameWindow{nullptr};  // g_hwnd for other threads (GameWindow())
WNDPROC g_oldWndProc = nullptr;
bool g_unicodeWnd = true;
ImFont* g_fontBold = nullptr;
ImFont* g_fontUi = nullptr;
ImGuiStyle g_baseStyle;
float g_styleScale = 0.0f;
bool g_menuWasOpen = false;
long g_frames = 0;

struct InFlight {
    InFlight() { ++g_inFlight; }
    ~InFlight() { --g_inFlight; }
};

// ------------------------------------------------------------------ friendly note wording
// Strings are numbered like the charts: 0 = the thickest (low E). The player counts from the thick
// side too ("3rd string" = D), and Rocksmith colours each string on the highway.
const ImU32 kStringColor[6] = {IM_COL32(232, 52, 52, 255),  IM_COL32(240, 206, 40, 255),  IM_COL32(52, 132, 242, 255),
                               IM_COL32(246, 136, 34, 255), IM_COL32(62, 196, 78, 255),   IM_COL32(182, 88, 228, 255)};
const char* kStringName[6] = {"E", "A", "D", "G", "B", "e"};  // bass uses the first four

// Strings are named by number and letter, "string 3 (D)", drawn in the string's highway colour (the
// colour names were hard to tell apart). 1 = the thickest, or the usual 1 = the thinnest (setting).
int StringNumber(const Settings& st, int s, int n) { return st.stringsFromThick ? s + 1 : n - s; }
std::string StringLabel(const Settings& st, int s, int n) {
    return "string " + std::to_string(StringNumber(st, s, n)) + " (" + kStringName[s] + ")";
}

// ------------------------------------------------------------------ colours (theme.h)
// This frame's theme colours (render thread only; Frame() fills them from the settings). The string
// colours above are not part of a theme.
ImU32 g_pal[theme::kSlots];

// A theme colour with alpha a (0..255): how see-through each thing is stays with the drawing code.
ImU32 Col(theme::Slot slot, int a = 255) {
    return (g_pal[slot] & 0x00FFFFFF) | ((ImU32)std::max(0, std::min(255, a)) << 24);
}

void LoadPalette(const Settings& st) {
    for (int i = 0; i < theme::kSlots; ++i) {
        const uint32_t rgb = Color(st, (theme::Slot)i);
        g_pal[i] = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
    }
}

// ------------------------------------------------------------------ calm fades (no flashing)
// Parts that come and go (the banner, messages) never pop in or out: their opacity moves towards
// shown/hidden at a limited speed. A bright panel appearing and disappearing several times a second
// is a flash, and repeated flashes (more than 3 a second, WCAG 2.3) can trigger seizures in people with
// photosensitive epilepsy; in a fast passage the banner used to do exactly that at every note.
struct Fade {
    float alpha = 0;  // 0 = hidden .. 1 = fully shown
    void Step(bool show, float dt, float inSeconds, float outSeconds) {
        alpha = show ? std::min(1.0f, alpha + dt / inSeconds) : std::max(0.0f, alpha - dt / outSeconds);
    }
};

// Multiplies the opacity of everything drawn into dl since vertex vtx0 by a (0..1).
void FadeFrom(ImDrawList* dl, int vtx0, float a) {
    if (a >= 1.0f) return;
    for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
        ImU32& c = dl->VtxBuffer[i].col;
        const ImU32 alpha = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f);
        c = (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    }
}

struct Seg {  // a piece of text in one colour
    std::string text;
    ImU32 col;
};

float SegsWidth(ImFont* f, float size, const std::vector<Seg>& segs) {
    float w = 0;
    for (const auto& s : segs) w += f->CalcTextSizeA(size, FLT_MAX, 0, s.text.c_str()).x;
    return w;
}

// The "what went wrong" line (hint.h), in the banner's colours. A soft red "!" in front.
std::vector<Seg> HintSegs(const hint::Line& line) {
    std::vector<Seg> out;
    if (line.empty()) return out;
    out.push_back({"!  ", Col(theme::kWarning)});
    for (const auto& h : line)
        out.push_back({h.text, h.color >= 0 && h.color < 6 ? kStringColor[h.color] : (h.color == hint::kGrey ? Col(theme::kTextDim) : Col(theme::kText))});
    return out;
}

// The same, one line per piece of advice (hint.cpp joins them with a grey " · "), so two pieces of
// advice make the banner taller instead of twice as wide. Later lines are indented under the first.
std::vector<std::vector<Seg>> HintLines(const hint::Line& line) {
    std::vector<std::vector<Seg>> out;
    const std::vector<Seg> all = HintSegs(line);
    for (size_t i = 0; i < all.size(); ++i) {
        if (i == 0 || all[i].text == "   \xC2\xB7   ") {
            out.push_back({});
            if (i > 0) {
                out.back().push_back({"      ", all[i].col});
                continue;
            }
        }
        out.back().push_back(all[i]);
    }
    return out;
}

float LinesWidth(ImFont* f, float size, const std::vector<std::vector<Seg>>& lines) {
    float w = 0;
    for (const auto& l : lines) w = std::max(w, SegsWidth(f, size, l));
    return w;
}

void DrawSegs(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, const std::vector<Seg>& segs) {
    for (const auto& s : segs) {
        dl->AddText(f, size, pos, s.col, s.text.c_str());
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0, s.text.c_str()).x;
    }
}

// "Hand: move UP to fret 7 (index finger there)" when the hand's anchor jumps 2 frets or more from
// the note before (setting bannerHand). Small moves are just "one finger per fret" and aren't said.
std::vector<std::vector<Seg>> HandLines(const View& v, const Settings& st) {
    std::vector<std::vector<Seg>> out;
    if (!st.bannerHand || v.anchorFret <= 0 || v.handFrom <= 0 || std::abs(v.anchorFret - v.handFrom) < 2) return out;
    const std::string how = std::string("move ") + (v.anchorFret > v.handFrom ? "UP" : "DOWN") + " to fret " +
                            std::to_string(v.anchorFret) + " (index finger there)";
    out.push_back({{"Hand:  ", Col(theme::kChord)}, {how, Col(theme::kText)}});
    return out;
}

// The banners' box: top-centre from the settings (S = screen scale), kept on screen.
ImVec2 BannerPlace(const Settings& st, float S, float w, float h, ImVec2 ds) {
    return Place(ds.x * 0.5f + st.bannerX * S - w * 0.5f, st.bannerY * S, w, h, ds);
}

// The banners' small tabs follow the tab's settings: which string is on top (tabThickTop) and, when
// left-handed (tabMirror), the string names on the right of the lines instead of the left.
// MiniTabRow: the row of a string, 0 = top. MiniTab: x of the names and of the lines' left end, for a
// small tab whose box starts at tx and is labelW + tabW wide.
int MiniTabRow(const Settings& st, int str, int n) { return st.tabThickTop ? str : n - 1 - str; }
struct MiniTab {
    float labelX, lineL;
    MiniTab(const Settings& st, float tx, float labelW, float tabW, float s)
        : labelX(st.tabMirror ? tx + tabW + 8 * s : tx), lineL(st.tabMirror ? tx : tx + labelW) {}
};

// ------------------------------------------------------------------ the banner's fretboard
// A piece of the neck, seen like the highway: strings across in their colours, frets numbered below.
// The note(s) to play are dots in the string's colour with the fret number inside (read like tab),
// labelled with the note's name; after a wrong note, a red X where it was probably played (hint.h
// guesses the spot: the pitch is heard, the string isn't) with an arrow to where it should be.
// Measure first (the banner needs the size), then Draw.

// The banner's window of frets, kept from one note to the next so the fretboard doesn't redraw itself
// at every note (only its dots change): it moves only when a note doesn't fit, and then just enough,
// like a camera following the hand, with a fret of room past the note where it can. From the nut
// while the notes are low enough. mn..mx: the frets the picture must show (-1 = none: only open
// strings, the window stays where it was). lo..hi: the window.
void KeptWindow(int mn, int mx, int* lo, int* hi) {
    constexpr int kCells = 6;  // frets shown (more only for a note that needs them, e.g. a long slide)
    static int s_lo = 1, s_cells = kCells;
    if (mx >= 0) {
        const int cells = std::max(kCells, mx - mn + 1);
        const bool fits = cells == s_cells && mn >= s_lo && mx <= s_lo + s_cells - 1;
        if (!fits) {
            int l;
            if (mx <= cells) l = 1;                                                // low: from the nut
            else if (mx > s_lo + s_cells - 1) l = std::max(mx - cells + 1, std::min(mx + 2 - cells, mn - 1));  // up
            else l = std::max(mx - cells + 1, mn - 1);                             // down
            s_lo = std::max(1, std::min(25 - cells, l));
            s_cells = cells;
        }
    }
    *lo = s_lo;
    *hi = s_lo + s_cells - 1;
}

struct NeckPic {
    struct Dot {
        int string, fret, midi;
        bool likely = true;  // marks: the advice's guess (X + name + arrow); false = same note elsewhere (faint X)
    };
    int n = 6;                 // strings
    bool chord = false;
    bool flats = false;        // note names with flats (the chord's name has them)
    std::vector<Dot> dots;     // what to play
    std::vector<Dot> marks;    // where wrong notes were played (inside the window only)
    std::vector<Dot> shape;    // a note inside a held chord shape: the chord's other strings (drawn faintly)
    int lo = 1, hi = 5;        // fret cells shown (1 = the first fret, with the nut on its left)
    technique::Technique tech; // single notes: the technique (a slide's end, a bend)
    int slideEnd = -1;         // the fret a slide goes to (-1 = none)
    bool slidePitched = true;  // false = an unpitched slide (it just fades): drawn fainter
    bool vibrato = false;      // the note (or a linked one) has vibrato: a small wave above the dot
    bool hand = false;         // show fingers and the hand's zone (setting bannerHand)
    int anchor = 0, anchorW = 0;  // the hand's zone: frets anchor .. anchor + anchorW - 1
    int fingers[6] = {-1, -1, -1, -1, -1, -1};
    uint32_t dotTech[6] = {};  // each dot's technique bits (a chord's own palm mute / mute / accent included)
    int strSlide[6] = {-1, -1, -1, -1, -1, -1};  // chords: where each string's slide ends (-1 = none)
    bool strPitched[6] = {true, true, true, true, true, true};
    float s = 1, gap = 0, cell = 0, rad = 0, nameW = 0, openW = 0, tailW = 0, top = 0, w = 0, h = 0;
    float stripH = 0;          // the hand drawn under the fret numbers (0 = none)

    // banner: the banner's picture, which stays the same from one note to the next: it keeps its window
    // of frets (KeptWindow), its shape (string spacing, open column) and draws the hand under the neck
    // (its room kept even when the song doesn't say where the hand is). The mistake panel's picture
    // fits each wrong note instead.
    NeckPic(const View& v, const Settings& st, float scale, bool banner = false) : s(scale) {
        n = v.bass ? 4 : 6;
        hand = st.bannerHand;
        if (hand) {
            anchor = v.anchorFret;
            anchorW = v.anchorWidth;
            std::copy(std::begin(v.fingers), std::end(v.fingers), fingers);
        }
        chord = v.chord;
        if (chord) {
            flats = music::UsesFlats(v.chordName);
            for (int i = 0; i < n; ++i)
                if (v.frets[i] >= 0) dots.push_back({i, v.frets[i], v.notes[i]});
        } else {
            dots.push_back({std::max(0, std::min(n - 1, v.string)), std::max(0, v.fret), v.midi});
            if (v.inShape)
                for (int i = 0; i < n; ++i)
                    if (i != v.string && v.shapeFrets[i] >= 0) shape.push_back({i, v.shapeFrets[i], -1});
        }
        // The window: the fretted notes with a fret of room, at least 5 frets, from the nut when they
        // are low on the neck. A wrong note joins it if it isn't too far (else only the text says it);
        // the same note's other spots too, up to a wider window (the frets get narrower).
        int mn = 99, mx = -1;
        for (const auto& d : dots) if (d.fret > 0) { mn = std::min(mn, d.fret); mx = std::max(mx, d.fret); }
        for (const auto& d : shape) if (d.fret > 0) { mn = std::min(mn, d.fret); mx = std::max(mx, d.fret); }
        if (!chord) {  // a slide (of the note, or of a note linked after it): where it ends is in the picture
            tech = v.tech;
            std::vector<technique::Link> chain = v.chain;
            if (chain.empty()) chain.push_back({v.tech, v.fret});
            for (const auto& l : chain) {
                const int end = (l.tech.mask & technique::kSlide) ? l.tech.slideTo
                              : (l.tech.mask & technique::kUnpitchedSlide) ? l.tech.slideUnpitchTo : -1;
                if (end < 0) continue;
                slideEnd = end;
                slidePitched = (l.tech.mask & technique::kSlide) != 0;
                break;
            }
            for (const auto& l : chain) vibrato = vibrato || (l.tech.mask & technique::kVibrato);
            if (slideEnd > 0) { mn = std::min(mn, slideEnd); mx = std::max(mx, slideEnd); }
            if (!dots.empty()) dotTech[dots[0].string] = v.tech.mask;
        } else {  // a chord: each string that slides (a double stop sliding down together)
            const uint32_t whole = (v.tech.mask & (technique::kPalmMute | technique::kAccent)) |
                                   ((v.tech.mask & technique::kChordMute) ? technique::kMute : 0);
            for (const auto& d : dots) dotTech[d.string] = v.strings[d.string].mask | whole;
            for (const auto& d : dots) {
                const technique::Technique& t = v.strings[d.string];
                const int end = (t.mask & technique::kSlide) ? t.slideTo : (t.mask & technique::kUnpitchedSlide) ? t.slideUnpitchTo : -1;
                if (end < 0 || end == d.fret) continue;
                strSlide[d.string] = end;
                strPitched[d.string] = (t.mask & technique::kSlide) != 0;
                if (end > 0) { mn = std::min(mn, end); mx = std::max(mx, end); }
            }
        }
        if (anchor > 0 && mx >= 0) {  // the hand's zone is part of the picture (when it's near the notes)
            const int a = std::min(mn, anchor), b = std::max(mx, anchor + anchorW - 1);
            if (b - a <= 9) { mn = a; mx = std::min(24, b); }
        }
        for (const bool likely : {true, false}) {
            for (const auto& m : v.heardAt) {
                if (m.likely != likely || m.string < 0 || m.string >= n || m.fret < 0) continue;
                const int a = m.fret > 0 ? std::min(mn, m.fret) : mn, b = std::max(mx, m.fret);
                if (mx >= 0 && b - (a <= 4 ? 1 : a) > (likely ? 9 : 12)) continue;
                if (m.fret > 0) { mn = a; mx = b; }
                marks.push_back({m.string, m.fret, m.midi, m.likely});
            }
        }
        if (banner) {
            KeptWindow(mx < 0 ? -1 : mn, mx, &lo, &hi);
        } else {
            if (mx < 0) mn = mx = 1;
            lo = mn <= 4 ? 1 : mn - 1;
            hi = std::min(24, std::max(lo + 4, mx + 1));
        }

        // The open-string column only when something is open, or a chord has "x" strings to mark (the
        // banner always has it, so its picture keeps one shape).
        bool open = banner || lo == 1 || chord;
        for (const auto& d : dots) open = open || d.fret == 0;
        for (const auto& d : shape) open = open || d.fret == 0;
        for (const auto& m : marks) open = open || m.fret == 0;
        // The banner: the same string spacing, top and tail for notes and chords (only the dots change).
        gap = (banner ? 22 : chord ? 24 : 20) * s;
        rad = (chord ? 11.5f : 14) * s;
        cell = std::max(34.0f, 276.0f / std::max(6, hi - lo + 1)) * s;  // 6 frets or fewer: 46
        nameW = 38 * s;  // "3 D": the string's number and letter
        openW = (open ? 30 : 8) * s;
        tailW = (banner || chord ? 34 : 30) * s;  // chords: a column with each string's note name; notes: the name tag
        top = banner ? 18 * s : rad + 4 * s;
        w = nameW + openW + (hi - lo + 1) * cell + tailW;
        h = top + (n - 1) * gap + 10 * s + 22 * s;  // + the fret numbers
        if (banner && hand && st.bannerFingers) {
            stripH = 40 * s;
            h += stripH;
        }
    }

    // The hand under the neck, seen from the player's side: four fingers standing on a palm, each over
    // the fret it covers (index on the anchor fret, one finger per fret, spread over a wider zone). A
    // finger playing now is filled with its string's colour and shows its number (1 = index .. 4 =
    // little); the others are faint. Their lengths follow a real hand. y0 = just under the fret numbers.
    // mir: left-handed (the frets go left, as in Draw).
    void DrawHand(ImDrawList* dl, ImVec2 p0, float y0, bool mir) const {
        auto X = [&](float x) { return mir ? p0.x + p0.x + w - x : x; };
        const int span = std::max(4, anchorW);
        if (anchor <= 0 || anchor > hi || anchor + span - 1 < lo) return;  // not known, or off the picture
        const float palmH = 10 * s, y1 = y0 + stripH - 4 * s, base = y1 - palmH * 0.5f;
        const float fw = std::min(cell * 0.6f, 17 * s);
        const float len[4] = {0.84f, 1.0f, 0.92f, 0.7f};  // index .. little
        const ImU32 skin = Col(theme::kText, 45), skinLine = Col(theme::kTextDim, 140);
        float xs[4];
        int used[4] = {-1, -1, -1, -1};  // the string each finger plays now (-1 = none)
        for (int k = 0; k < 4; ++k) {
            int f = anchor + (int)std::lround(k * (span - 1) / 3.0);
            for (const auto& d : dots)
                if (d.fret > 0 && fingers[d.string] == k + 1) {  // it plays this note: over its fret
                    f = d.fret;
                    used[k] = d.string;
                    break;
                }
            xs[k] = FretX(p0.x, std::max(lo, std::min(hi, f)));
        }
        // Fingers too close (two notes on one fret, played by two fingers: a power chord's top notes)
        // were drawn on top of each other. Keep them in the hand's order, at least a finger apart: each
        // group that is too close is spread evenly around its own centre (then checked again, in case it
        // now touches the next one).
        const float minGap = fw + 3 * s;
        bool moved = true;
        for (int pass = 0; moved && pass < 8; ++pass) {  // (4 fingers: a few passes at most)
            moved = false;
            for (int a = 0; a < 4;) {
                int b = a;  // the group a..b: each finger closer than minGap to the one before
                while (b + 1 < 4 && xs[b + 1] - xs[b] < minGap - 0.01f) ++b;
                if (b > a) {
                    float mid = 0;
                    for (int k = a; k <= b; ++k) mid += xs[k];
                    mid /= (b - a + 1);
                    for (int k = a; k <= b; ++k) xs[k] = mid + (k - a - (b - a) * 0.5f) * minGap;
                    moved = true;
                }
                a = b + 1;
            }
        }
        for (float& x : xs) x = X(x);
        const float pl = std::min(xs[0], xs[3]) - fw * 0.5f - 3 * s, pr = std::max(xs[0], xs[3]) + fw * 0.5f + 3 * s;
        dl->AddRectFilled(ImVec2(pl, y1 - palmH), ImVec2(pr, y1), skin, palmH * 0.5f);
        dl->AddRect(ImVec2(pl, y1 - palmH), ImVec2(pr, y1), skinLine, palmH * 0.5f, 0, 1.2f * s);
        const float nfs = std::min(15 * s, fw * 0.95f);
        for (int k = 0; k < 4; ++k) {
            const bool on = used[k] >= 0;
            const float tip = base - (base - y0) * len[k];
            const ImVec2 a(xs[k] - fw * 0.5f, tip), b(xs[k] + fw * 0.5f, base);
            dl->AddRectFilled(a, b, on ? kStringColor[used[k]] : skin, fw * 0.5f);
            dl->AddRect(a, b, on ? Col(theme::kText, 230) : skinLine, fw * 0.5f, 0, (on ? 1.8f : 1.2f) * s);
            const std::string t = std::to_string(k + 1);
            const ImVec2 ts = g_fontBold->CalcTextSizeA(nfs, FLT_MAX, 0, t.c_str());
            // Dark digits on the light string colours (yellow, orange, green), white on the others.
            const bool light = on && (used[k] == 1 || used[k] == 3 || used[k] == 4);
            const ImU32 tc = !on ? Col(theme::kTextDim, 170) : light ? IM_COL32(20, 20, 24, 255) : IM_COL32(255, 255, 255, 255);
            dl->AddText(on ? g_fontBold : g_fontUi, nfs, ImVec2(xs[k] - ts.x * 0.5f, tip + fw * 0.5f - ts.y * 0.5f + 1 * s), tc, t.c_str());
        }
    }

    // Centre x of fret f (0 = the open column), before mirroring.
    float FretX(float x0, int f) const {
        if (f == 0) return x0 + nameW + openW * 0.5f;
        return x0 + nameW + openW + (f - lo + 0.5f) * cell;
    }

    void Draw(ImDrawList* dl, const Settings& st, ImVec2 p0) const {
        const bool mir = st.tabMirror;
        auto X = [&](float x) { return mir ? p0.x + p0.x + w - x : x; };  // left-handed: frets go left
        auto Y = [&](int str) { return p0.y + top + MiniTabRow(st, str, n) * gap; };
        const float neckL = p0.x + nameW + openW, neckR = neckL + (hi - lo + 1) * cell;
        const float yTop = Y(st.tabThickTop ? 0 : n - 1), yBot = Y(st.tabThickTop ? n - 1 : 0);
        const float tiny = 18 * s, label = 20 * s;

        // Wood, inlays, fret wires, the nut (or a faint edge when the window starts higher up).
        const ImVec2 w0(std::min(X(neckL), X(neckR)), yTop - 9 * s), w1(std::max(X(neckL), X(neckR)), yBot + 9 * s);
        dl->AddRectFilled(w0, w1, IM_COL32(0, 0, 0, 90), 4 * s);
        // The hand's zone: the frets its four fingers cover (index on the anchor fret), lightly shaded.
        if (anchor > 0 && anchorW > 0) {
            const int a = std::max(lo, anchor), b = std::min(hi, anchor + anchorW - 1);
            if (a <= b) {
                const float xa = X(neckL + (a - lo) * cell), xb = X(neckL + (b - lo + 1) * cell);
                dl->AddRectFilled(ImVec2(std::min(xa, xb), yTop - 9 * s), ImVec2(std::max(xa, xb), yBot + 9 * s),
                                  Col(theme::kText, 26), 3 * s);
            }
        }
        const float midY = (yTop + yBot) * 0.5f;
        for (int f = lo; f <= hi; ++f) {
            const int k = f % 12;
            const ImU32 inlay = IM_COL32(255, 255, 255, 38);
            if (k == 0) {
                dl->AddCircleFilled(ImVec2(X(FretX(p0.x, f)), midY - gap), 4.5f * s, inlay);
                dl->AddCircleFilled(ImVec2(X(FretX(p0.x, f)), midY + gap), 4.5f * s, inlay);
            } else if (k == 3 || k == 5 || k == 7 || k == 9) {
                dl->AddCircleFilled(ImVec2(X(FretX(p0.x, f)), midY), 4.5f * s, inlay);
            }
            const float fx = X(neckL + (f - lo + 1) * cell);
            dl->AddLine(ImVec2(fx, yTop - 9 * s), ImVec2(fx, yBot + 9 * s), Col(theme::kTextDim, 130), 1.6f * s);
        }
        if (lo == 1) dl->AddLine(ImVec2(X(neckL), yTop - 9 * s), ImVec2(X(neckL), yBot + 9 * s), Col(theme::kText, 220), 5 * s);
        else dl->AddLine(ImVec2(X(neckL), yTop - 9 * s), ImVec2(X(neckL), yBot + 9 * s), Col(theme::kTextDim, 90), 1.6f * s);

        // Strings in their colours (thicker for the low ones), names at the side.
        auto used = [&](int str) {
            for (const auto& d : dots) if (d.string == str) return true;
            for (const auto& d : shape) if (d.string == str) return true;
            for (const auto& m : marks) if (m.string == str) return true;
            return false;
        };
        for (int str = 0; str < n; ++str) {
            const float y = Y(str);
            const bool on = used(str);
            const ImU32 c = on ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (120u << 24));
            dl->AddLine(ImVec2(X(neckL - openW * 0.55f), y), ImVec2(X(neckR), y), c, (1.3f + 0.45f * (n - 1 - str)) * s);
            const std::string name = std::to_string(StringNumber(st, str, n)) + " " + kStringName[str];
            const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, name.c_str());
            dl->AddText(g_fontUi, tiny, ImVec2(X(p0.x + nameW * 0.5f) - ns.x * 0.5f, y - ns.y * 0.5f), c, name.c_str());
        }

        // Fret numbers under the neck; the ones in use brighter.
        const float numY = yBot + 11 * s;
        auto number = [&](int f, bool bright) {
            const std::string t = std::to_string(f);
            const ImVec2 ts = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, t.c_str());
            dl->AddText(bright ? g_fontBold : g_fontUi, tiny, ImVec2(X(FretX(p0.x, f)) - ts.x * 0.5f, numY), bright ? Col(theme::kText) : Col(theme::kTextDim, 170), t.c_str());
        };
        bool anyOpen = false;
        for (const auto& d : dots) anyOpen = anyOpen || d.fret == 0;
        for (int f = lo; f <= hi; ++f) {
            bool bright = false;
            for (const auto& d : dots) bright = bright || d.fret == f;
            number(f, bright);
        }
        if (anyOpen) number(0, true);
        if (stripH > 0) DrawHand(dl, p0, numY + tiny + 4 * s, mir);

        // A small name tag ("G#") beside a dot, on a dark background so it reads over the strings.
        auto tag = [&](ImVec2 c, bool left, const std::string& t, ImU32 col) {
            const ImVec2 ts = g_fontBold->CalcTextSizeA(label, FLT_MAX, 0, t.c_str());
            const float x = left ? c.x - rad - 12 * s - ts.x : c.x + rad + 12 * s;
            const ImVec2 a(x - 5 * s, c.y - ts.y * 0.5f - 1 * s), b(x + ts.x + 5 * s, c.y + ts.y * 0.5f + 1 * s);
            dl->AddRectFilled(a, b, Col(theme::kPanel, 240), 6 * s);
            dl->AddRect(a, b, col, 6 * s, 0, 1.5f * s);
            dl->AddText(g_fontBold, label, ImVec2(x, c.y - ts.y * 0.5f), col, t.c_str());
        };

        // The wrong notes first (under the dots), fading in; an arrow from each to the right spot.
        const float ma = MarksAlpha();
        const ImU32 red = Col(theme::kWarning, (int)(255 * ma));
        for (const auto& m : marks) {
            if (dots.empty()) break;
            const ImVec2 c(X(FretX(p0.x, m.fret)), Y(m.string));
            if (!m.likely) {  // the same note elsewhere: a faint X, no name or arrow
                const ImU32 faint = Col(theme::kWarning, (int)(150 * ma));
                dl->AddCircleFilled(c, rad * 0.75f, Col(theme::kPanel, (int)(200 * ma)));
                dl->AddCircle(c, rad * 0.75f, faint, 0, 1.8f * s);
                const float k = rad * 0.35f;
                dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), faint, 2.5f * s);
                dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), faint, 2.5f * s);
                continue;
            }
            // The dot it should have been: the one on the same string (chord), or the note.
            const Dot* to = &dots[0];
            for (const auto& d : dots) if (d.string == m.string) to = &d;
            const ImVec2 t(X(FretX(p0.x, to->fret)), Y(to->string));
            const float dx = t.x - c.x, dy = t.y - c.y, len = std::sqrt(dx * dx + dy * dy);
            if (len > rad * 2.2f) {
                const ImVec2 u(dx / len, dy / len);
                const ImVec2 a(c.x + u.x * (rad + 3 * s), c.y + u.y * (rad + 3 * s));
                const ImVec2 b(t.x - u.x * (rad + 4 * s), t.y - u.y * (rad + 4 * s));
                const ImU32 ac = Col(theme::kText, (int)(210 * ma));
                const float hl = 9 * s;
                dl->AddLine(a, ImVec2(b.x - u.x * hl * 0.8f, b.y - u.y * hl * 0.8f), ac, 2.5f * s);
                dl->AddTriangleFilled(b, ImVec2(b.x - u.x * hl - u.y * hl * 0.6f, b.y - u.y * hl + u.x * hl * 0.6f),
                                      ImVec2(b.x - u.x * hl + u.y * hl * 0.6f, b.y - u.y * hl - u.x * hl * 0.6f), ac);
            }
            dl->AddCircleFilled(c, rad * 0.9f, Col(theme::kPanel, (int)(235 * ma)));
            dl->AddCircle(c, rad * 0.9f, red, 0, 2.5f * s);
            const float k = rad * 0.45f;
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), red, 3.5f * s);
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), red, 3.5f * s);
            // Its name on the side away from the arrow (single notes; chords name the notes at the end).
            if (!chord && m.midi >= 0) tag(c, dx > 0, music::NoteName(m.midi, flats), red);
        }

        // Techniques of a single note. A slide: a dashed line along the string to a ring at the fret
        // where it ends (solid ring = a slide to that note; faint = an unpitched slide that just fades).
        // A bend: an arrow above the dot with how many steps ("1/2", "1").
        auto drawSlide = [&](ImVec2 c, int slideEnd, bool pitched, ImU32 sc) {
                const ImVec2 e(X(FretX(p0.x, slideEnd)), c.y);
                const float dir = e.x > c.x ? 1.0f : -1.0f;
                // Light dashes with a dark outline, so the arrow stands out on any string colour (a light
                // arrow alone vanished on the yellow string).
                const ImU32 lc = pitched ? Col(theme::kText, 240) : Col(theme::kText, 190);
                const ImU32 dark = Col(theme::kPanel, 235);
                const float hx = e.x - dir * (rad * 0.8f + 3 * s), hl = 9 * s;
                // A solid dark band first, under the whole arrow (the string doesn't show between the dashes).
                dl->AddLine(ImVec2(c.x + dir * (rad + 2 * s), c.y), ImVec2(hx - dir * hl * 0.5f, c.y), dark, 3 * s + 3.2f * s);
                for (int pass = 0; pass < 2; ++pass) {
                    const ImU32 col = pass ? lc : dark;
                    const float grow = pass ? 0 : 1.6f * s;
                    for (float x = c.x + dir * (rad + 4 * s); dir * (e.x - dir * (rad * 0.8f + 10 * s) - x) > 0; x += dir * 10 * s)
                        dl->AddLine(ImVec2(x - dir * grow, c.y), ImVec2(x + dir * (6 * s + grow), c.y), col, 3 * s + 2 * grow);
                    dl->AddTriangleFilled(ImVec2(hx + dir * grow, c.y), ImVec2(hx - dir * (hl + grow), c.y - hl * 0.6f - grow),
                                          ImVec2(hx - dir * (hl + grow), c.y + hl * 0.6f + grow), col);
                }
                dl->AddCircleFilled(e, rad * 0.8f, Col(theme::kPanel, 235));
                dl->AddCircle(e, rad * 0.8f, pitched ? sc : ((sc & 0x00FFFFFF) | (140u << 24)), 0, 2.5f * s);
                const std::string t = std::to_string(slideEnd);
                const float efs = 18 * s;
                const ImVec2 ts = g_fontBold->CalcTextSizeA(efs, FLT_MAX, 0, t.c_str());
                dl->AddText(g_fontBold, efs, ImVec2(e.x - ts.x * 0.5f, e.y - ts.y * 0.5f), pitched ? Col(theme::kText) : Col(theme::kTextDim), t.c_str());
        };
        if (chord)
            for (const auto& d : dots)
                if (strSlide[d.string] >= 0) drawSlide(ImVec2(X(FretX(p0.x, d.fret)), Y(d.string)), strSlide[d.string], strPitched[d.string], kStringColor[d.string]);
        if (!chord && !dots.empty()) {
            const Dot& d = dots[0];
            const ImVec2 c(X(FretX(p0.x, d.fret)), Y(d.string));
            if (slideEnd >= 0 && slideEnd != d.fret) drawSlide(c, slideEnd, slidePitched, kStringColor[d.string]);
            if (vibrato) {  // "~~" above the dot, like tab (left of a bend's arrow)
                const float wy = c.y - rad - 9 * s, w0 = c.x - 13 * s, amp = 3 * s;
                ImVec2 pts[13];
                for (int i = 0; i < 13; ++i)
                    pts[i] = ImVec2(w0 + i * (18 * s / 12), wy + amp * std::sin(i * 3.14159f / 3));
                dl->AddPolyline(pts, 13, Col(theme::kText, 235), 0, 2.2f * s);
            }
            if ((tech.mask & technique::kBend) && tech.bend > 0.1f) {
                const float top = c.y - rad - 4 * s, len = 16 * s;
                const ImU32 bc = Col(theme::kText, 235);
                dl->AddLine(ImVec2(c.x, top), ImVec2(c.x, top - len + 5 * s), bc, 2.5f * s);
                dl->AddTriangleFilled(ImVec2(c.x, top - len - 2 * s), ImVec2(c.x - 5 * s, top - len + 6 * s), ImVec2(c.x + 5 * s, top - len + 6 * s), bc);
                const std::string t = technique::BendLabel(tech.bend);
                const float bfs = 17 * s;
                const ImVec2 ts = g_fontBold->CalcTextSizeA(bfs, FLT_MAX, 0, t.c_str());
                const ImVec2 tp(c.x + 7 * s, top - len - ts.y * 0.5f);
                dl->AddRectFilled(ImVec2(tp.x - 3 * s, tp.y), ImVec2(tp.x + ts.x + 3 * s, tp.y + ts.y), Col(theme::kPanel, 230), 4 * s);
                dl->AddText(g_fontBold, bfs, tp, bc, t.c_str());
            }
        }

        // A held chord shape: its other strings as faint rings (the fingers stay there while this string
        // is picked), with their fret numbers.
        for (const auto& d : shape) {
            const ImVec2 c(X(FretX(p0.x, d.fret)), Y(d.string));
            const ImU32 sc = kStringColor[d.string] & 0x00FFFFFF;
            dl->AddCircleFilled(c, rad * 0.8f, sc | (60u << 24));
            dl->AddCircle(c, rad * 0.8f, sc | (190u << 24), 0, 2 * s);
            const std::string t = std::to_string(d.fret);
            const ImVec2 ts = g_fontBold->CalcTextSizeA(16 * s, FLT_MAX, 0, t.c_str());
            dl->AddText(g_fontBold, 16 * s, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), Col(theme::kText, 170), t.c_str());
        }

        // The notes to play: a dot in the string's colour with the fret number, like tab.
        const float fs = (chord ? 19 : 23) * s;
        for (const auto& d : dots) {
            const ImVec2 c(X(FretX(p0.x, d.fret)), Y(d.string));
            const ImU32 sc = kStringColor[d.string];
            // The dot: a circle, or a diamond for a harmonic (the highway draws harmonics as diamonds);
            // an accent gets a bright ring.
            const uint32_t tq = dotTech[d.string];
            if (tq & (technique::kHarmonic | technique::kPinchHarmonic)) {
                const float dr = rad * 1.25f;
                dl->AddQuadFilled(ImVec2(c.x, c.y - dr - 3 * s), ImVec2(c.x + dr + 3 * s, c.y), ImVec2(c.x, c.y + dr + 3 * s),
                                  ImVec2(c.x - dr - 3 * s, c.y), (sc & 0x00FFFFFF) | (70u << 24));
                dl->AddQuadFilled(ImVec2(c.x, c.y - dr), ImVec2(c.x + dr, c.y), ImVec2(c.x, c.y + dr), ImVec2(c.x - dr, c.y), sc);
            } else {
                dl->AddCircleFilled(c, rad + 3 * s, (sc & 0x00FFFFFF) | (70u << 24));  // soft glow (steady)
                dl->AddCircleFilled(c, rad, sc);
            }
            if (tq & technique::kAccent) dl->AddCircle(c, rad + 5 * s, Col(theme::kText, 230), 0, 2.2f * s);
            // Other techniques: a small tag at the top-right, in tab words.
            std::string techTag;
            auto add = [&](const char* t) { techTag += (techTag.empty() ? "" : " ") + std::string(t); };
            if (tq & technique::kMute) add("X");
            if (tq & technique::kPalmMute) add("PM");
            if (tq & technique::kHammerOn) add("H");
            if (tq & technique::kPullOff) add("P");
            if (tq & technique::kTap) add("T");
            if (tq & technique::kSlap) add("S");
            if (tq & technique::kPluck) add("Pop");
            if (tq & technique::kTremolo) add("tr");
            if (tq & technique::kPinchHarmonic) add("PH");
            if (!techTag.empty()) {
                const float tfs = (chord ? 13 : 15) * s;
                const ImVec2 tsz = g_fontBold->CalcTextSizeA(tfs, FLT_MAX, 0, techTag.c_str());
                const ImVec2 a(c.x + rad * 0.45f, c.y - rad - tsz.y * 0.55f), b(a.x + tsz.x + 6 * s, a.y + tsz.y);
                dl->AddRectFilled(a, b, IM_COL32(245, 245, 245, 255), 4 * s);
                dl->AddRect(a, b, IM_COL32(20, 20, 24, 255), 4 * s, 0, 1.2f * s);
                dl->AddText(g_fontBold, tfs, ImVec2(a.x + 3 * s, a.y), IM_COL32(20, 20, 24, 255), techTag.c_str());
            }
            const std::string t = std::to_string(d.fret);
            const ImVec2 ts = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str());
            // Dark digits on the light string colours (yellow, green, orange), white on the others.
            const bool light = d.string == 1 || d.string == 3 || d.string == 4;
            dl->AddText(g_fontBold, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), light ? IM_COL32(20, 20, 24, 255) : IM_COL32(255, 255, 255, 255), t.c_str());
            // The finger: a small badge at the dot's lower corner (1 = index .. 4 = little, T = thumb).
            if (hand && fingers[d.string] >= 0) {
                const std::string fg = fingers[d.string] == 0 ? "T" : std::to_string(fingers[d.string]);
                const float br = rad * 0.52f, bfs = (chord ? 13 : 15) * s;
                const ImVec2 bc(c.x - rad * 0.72f, c.y + rad * 0.72f);
                dl->AddCircleFilled(bc, br, IM_COL32(245, 245, 245, 255));
                dl->AddCircle(bc, br, IM_COL32(20, 20, 24, 255), 0, 1.5f * s);
                const ImVec2 fs2 = g_fontBold->CalcTextSizeA(bfs, FLT_MAX, 0, fg.c_str());
                dl->AddText(g_fontBold, bfs, ImVec2(bc.x - fs2.x * 0.5f, bc.y - fs2.y * 0.5f), IM_COL32(20, 20, 24, 255), fg.c_str());
            }
            if (!chord && d.midi >= 0) {
                // Name tag on the right, unless a wrong note sits there on the same string.
                bool busyRight = false;
                for (const auto& m : marks) busyRight = busyRight || (m.string == d.string && X(FretX(p0.x, m.fret)) > c.x);
                if (slideEnd >= 0 && X(FretX(p0.x, slideEnd)) > c.x) busyRight = true;  // the slide's arrow is there
                tag(c, busyRight, music::NoteName(d.midi), sc);
            }
        }
        // Chords: each string's note name in a column after the neck; "x" at the nut = don't play it.
        if (chord) {
            for (int str = 0; str < n; ++str) {
                const float y = Y(str);
                const Dot* d = nullptr;
                for (const auto& dd : dots) if (dd.string == str) d = &dd;
                if (!d) {
                    const ImVec2 xs = g_fontBold->CalcTextSizeA(label, FLT_MAX, 0, "x");
                    dl->AddText(g_fontBold, label, ImVec2(X(FretX(p0.x, 0)) - xs.x * 0.5f, y - xs.y * 0.5f), Col(theme::kTextDim), "x");
                    continue;
                }
                if (d->midi < 0) continue;
                const std::string t = music::NoteName(d->midi, flats);
                const ImVec2 ts = g_fontBold->CalcTextSizeA(label, FLT_MAX, 0, t.c_str());
                dl->AddText(g_fontBold, label, ImVec2(X(neckR + tailW * 0.55f) - ts.x * 0.5f, y - ts.y * 0.5f), kStringColor[str], t.c_str());
            }
        }
    }

    // The red X's fade in (0.2 s) each time they change, so a new wrong note never pops in.
    float MarksAlpha() const {
        if (marks.empty()) return 1;  // (a picture without X's mustn't restart the fade of one with them)
        static std::string s_key;
        static double s_since = 0;
        std::string key;
        for (const auto& m : marks) key += std::to_string(m.string) + "," + std::to_string(m.fret) + "," + std::to_string(m.midi) + ";";
        const double now = ImGui::GetTime();
        if (key != s_key) {
            s_key = key;
            s_since = now;
        }
        return (float)std::min(1.0, (now - s_since) / 0.2);
    }
};

// The repeat counter's swell when its number goes down (a note of the run played): 0 = still .. 1 = the
// biggest, over 0.3 s. Only its size changes, never its brightness (no flash, even in a fast run).
// left: the counter's number, 0 = no counter (so a new run never starts with a swell).
float RepeatPulse(int left) {
    static int s_left = 0;
    static double s_at = -10;
    const double now = ImGui::GetTime();
    if (left > 0 && left < s_left) s_at = now;
    s_left = left;
    const double p = (now - s_at) / 0.3;
    return p >= 1 ? 0.0f : (float)std::sin(3.14159265 * p);
}

// "x5" in a pill of colour col (the string's, gold for a chord), centred on c (h = its height at rest);
// swells by `pulse`. light: col is a light colour (dark digits on it).
void DrawRepeatBadge(ImDrawList* dl, ImVec2 c, float h, int left, ImU32 col, bool light, float pulse, float s) {
    const float k = 1.0f + 0.3f * pulse, fs = h * 0.78f * k;
    const std::string text = "x" + std::to_string(left);
    const ImVec2 ts = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
    const ImVec2 half(std::max(ts.x * 0.5f + 12 * s * k, h * 0.5f * k), h * 0.5f * k);
    if (pulse > 0) {  // a soft halo while it swells
        const float g = 5 * s * pulse;
        dl->AddRectFilled(ImVec2(c.x - half.x - g, c.y - half.y - g), ImVec2(c.x + half.x + g, c.y + half.y + g),
                          (col & 0x00FFFFFF) | ((ImU32)(90 * pulse) << 24), half.y + g);
    }
    dl->AddRectFilled(ImVec2(c.x - half.x, c.y - half.y), ImVec2(c.x + half.x, c.y + half.y), col, half.y);
    dl->AddText(g_fontBold, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                light ? IM_COL32(20, 20, 24, 255) : IM_COL32(255, 255, 255, 255), text.c_str());
}

// The banners' box has ONE size (only the player's banner size changes it), the same for notes and
// chords, so it never jumps from one note to the next: the text block on the left shrinks to fit when
// a note has a lot to say, the picture sits centred in its own slot on the right (a wide window of
// frets is shrunk to fit it). Sizes in 1080p px, times the banner's scale. (It used to fit its text:
// it changed width and height at nearly every note.)
std::vector<std::vector<Seg>> WrapSegs(ImFont* f, float size, const std::vector<Seg>& segs, float maxW);  // (below)

// Lines no wider than maxW: a longer one wraps onto the next line (word by word).
std::vector<std::vector<Seg>> WrapLines(ImFont* f, float size, const std::vector<std::vector<Seg>>& lines, float maxW) {
    std::vector<std::vector<Seg>> out;
    for (const auto& l : lines)
        for (auto& w : WrapSegs(f, size, l, maxW)) out.push_back(std::move(w));
    return out;
}

// (The text block was 560 wide, room for the longest technique line: a short note left a wide gap before
// the fretboard. 480 fits the first line with a repeat counter; longer lines under it wrap instead.)
constexpr float kBannerTextW = 480, kBannerPicW = 380, kBannerInnerH = 170, kBannerPad = 24, kBannerSep = 34;
constexpr float kBannerHandH = 32;  // more height when the fretboard shows the hand under it (bannerHand)

struct BannerBox {
    ImVec2 p0, p1;
    float k = 1;           // the text's scale (1 = its normal size)
    ImVec2 text;           // top-left of the text block: always the box's top, so "Play ..." never moves when
                           // lines come and go under it
    float keysY = 0;       // top of the last line ("F9 = skip  F8 = menu"), pinned to the box's bottom
    ImVec2 pic;            // top-left of the picture's slot
    float picW = 0, picH = 0;
};

// textW, textH: the text block at its normal size (s = the banner's scale); keysH: its last line's
// height (normal size).
BannerBox BannerLayout(const Settings& st, float S, float s, float textW, float textH, float keysH, ImVec2 ds) {
    const float tw = kBannerTextW * s, ih = (kBannerInnerH + (st.bannerNeck && st.bannerHand && st.bannerFingers ? kBannerHandH : 0)) * s;
    const float pad = kBannerPad * s;
    BannerBox b;
    b.picW = kBannerPicW * s;
    b.picH = ih;
    const float w = pad + tw + kBannerSep * s + b.picW + pad, h = pad + ih + pad;
    b.p0 = BannerPlace(st, S, w, h, ds);
    b.p1 = ImVec2(b.p0.x + w, b.p0.y + h);
    b.k = std::min({1.0f, tw / std::max(1.0f, textW), ih / std::max(1.0f, textH)});
    b.text = ImVec2(b.p0.x + pad, b.p0.y + pad);
    b.keysY = b.p0.y + pad + ih - keysH * b.k;
    b.pic = ImVec2(b.p0.x + pad + tw + kBannerSep * s, b.p0.y + pad);
    return b;
}

// The banner's fretboard at scale s, shrunk if it doesn't fit the picture's slot.
NeckPic FitNeck(const View& v, const Settings& st, float s, const BannerBox& b) {
    NeckPic neck(v, st, s, true);
    const float f = std::min({1.0f, b.picW / std::max(1.0f, neck.w), b.picH / std::max(1.0f, neck.h)});
    return f < 1 ? NeckPic(v, st, s * f, true) : neck;
}

// "870 ms": a hold time for the banner.
std::string Millis(double s) { return std::to_string((int)std::lround(std::max(0.0, s) * 1000.0)) + " ms"; }

// The banner's line about holding the note ("Hold:  let it ring for 870 ms"); empty for a short note.
std::vector<Seg> HoldWords(const View& v) {
    if (v.sustain < kHoldMinS) return {};
    return {{"Hold:  ", Col(theme::kChord)}, {"let it ring for ", Col(theme::kText)}, {Millis(v.sustain), Col(theme::kHighlight)}};
}

// A pick stroke's sign, as in printed music: a bracket open at the bottom = down stroke, a V = up stroke.
// c = its centre, half = half its width and height. (Drawn: the UI font has no such glyphs.)
void DrawPickSign(ImDrawList* dl, ImVec2 c, float half, float thick, int pick, ImU32 col) {
    if (pick == 0) {
        const ImVec2 pts[4] = {ImVec2(c.x - half, c.y + half), ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y - half),
                               ImVec2(c.x + half, c.y + half)};
        dl->AddPolyline(pts, 4, col, 0, thick);
    } else if (pick == 1) {
        const ImVec2 pts[3] = {ImVec2(c.x - half, c.y - half), ImVec2(c.x, c.y + half), ImVec2(c.x + half, c.y - half)};
        dl->AddPolyline(pts, 3, col, 0, thick);
    }
}

// The countdown of a held note just played, on the banner's last line instead of the keys: "Keep
// holding fret 9", a bar that empties, "540 ms". Runs on the song's clock (it stops when the song
// does). Returns false when no note is being held now.
bool DrawHoldCountdown(ImDrawList* dl, const View& v, const Settings& st, ImVec2 pos, float size, float s) {
    if (v.holdFrom < 0 || v.holdLen < kHoldMinS || v.songTime < v.holdFrom - 0.05) return false;
    const double left = v.holdFrom + v.holdLen - v.songTime;
    if (left <= 0) return false;
    const int n = v.bass ? 4 : 6;
    const int str = std::max(0, std::min(n - 1, v.holdString));
    const ImU32 col = v.holdName.empty() ? kStringColor[str] : Col(theme::kChord);
    std::vector<Seg> label = {{"Keep holding ", Col(theme::kText)}};
    if (!v.holdName.empty()) label.push_back({v.holdName, col});
    else label.push_back({v.holdFret == 0 ? StringLabel(st, str, n) + " open" : "fret " + std::to_string(v.holdFret), col});
    DrawSegs(dl, g_fontUi, size, pos, label);
    const float x0 = pos.x + SegsWidth(g_fontUi, size, label) + 12 * s, barW = 150 * s, h = size * 0.5f;
    const float y0 = pos.y + size * 0.3f, f = (float)std::min(1.0, left / v.holdLen);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW, y0 + h), Col(theme::kPanel, 255), h * 0.5f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW * f, y0 + h), col, h * 0.5f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + barW, y0 + h), Col(theme::kTextDim, 160), h * 0.5f, 0, 1 * s);
    dl->AddText(g_fontBold, size, ImVec2(x0 + barW + 10 * s, pos.y), Col(theme::kText), Millis(left).c_str());
    return true;
}

// The "waiting" banner: what to play in words (+ colour), and as a piece of fretboard or a tiny tab.
// S = screen scale (height / 1080); sizes also follow the player's banner size.
void DrawBanner(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    const float s = S * st.bannerSize / 100.0f;
    const int n = v.bass ? 4 : 6;
    const int i = v.string < 0 ? 0 : (v.string >= n ? n - 1 : v.string);
    const ImU32 col = kStringColor[i];
    const float big = 46 * s, mid = 26 * s, tiny = 20 * s;

    // How to play it (slide, bend, hammer-on...), from the song: "Slide: then slide UP to fret 9 ..."
    // Several steps (a note linked into the next ones: a vibrato that ends in a slide) are numbered.
    const auto steps = v.chain.empty() ? technique::Describe(v.tech, v.fret) : technique::Sequence(v.chain);
    // The first line starts with what to do: the first technique ("Slide fret 9 on string 3"), "Hold" for a
    // long plain note, else "Play".
    std::string verb = "Play ";
    if (!steps.empty()) verb = (steps[0].name == "Muted" ? std::string("Mute") : steps[0].name) + " ";
    else if (v.sustain >= kHoldMinS) verb = "Hold ";
    char fret[32];
    std::snprintf(fret, sizeof(fret), "fret %d", v.fret);
    std::vector<Seg> line1;
    if (v.fret == 0) line1 = {{verb, Col(theme::kText)}, {StringLabel(st, i, n), col}, {" open", Col(theme::kText)}};
    else line1 = {{verb, Col(theme::kText)}, {fret, Col(theme::kText)}, {" on ", Col(theme::kText)}, {StringLabel(st, i, n), col}};

    // The note's name, only with the small tab (the fretboard's dot has it in a tag).
    std::vector<Seg> line2;
    if (v.midi >= 0 && !st.bannerNeck) line2.push_back({"note " + music::NoteName(v.midi), Col(theme::kTextDim)});
    const std::vector<Seg> line3 = {{v.fret == 0 ? "(no finger on the neck)   " : "", Col(theme::kTextDim)}, {"F9 = skip   F8 = menu", Col(theme::kTextDim)}};
    const std::vector<std::vector<Seg>> linesH = WrapLines(g_fontUi, mid, HintLines(v.hint), kBannerTextW * s);
    // How to play it (slide, bend, hammer-on...), from the song: "Slide: then slide UP to fret 9 ..."
    // Several steps (a note linked into the next ones: a vibrato that ends in a slide) are numbered.
    std::vector<std::vector<Seg>> linesT = HandLines(v, st);
    for (size_t k = 0; k < steps.size(); ++k) {
        const std::string num = steps.size() > 1 ? std::to_string(k + 1) + ".  " : "";
        linesT.push_back({{num + steps[k].name + ":  ", Col(theme::kChord)}, {steps[k].how, Col(theme::kText)}});
    }
    if (v.inShape)  // a note of a held chord shape (the faint rings on the fretboard)
        linesT.push_back({{"Hold the shape:  ", Col(theme::kChord)},
                          {"keep your fingers on " + (v.shapeName.empty() ? std::string("the chord") : v.shapeName) +
                               ", pick its strings one by one", Col(theme::kText)}});
    if (const auto hold = HoldWords(v); !hold.empty()) linesT.push_back(hold);
    linesT = WrapLines(g_fontUi, mid, linesT, kBannerTextW * s);  // a long one takes two lines (the box has room)

    // A quick repeat of this note: "x5" after the first line, counting down as they're played. Room is
    // kept for the run's biggest number, so the banner doesn't change width as it counts.
    const bool repeat = v.repeatTotal >= 2 && v.repeatLeft >= 1;
    const float repH = big * 0.9f, repGap = 18 * s;
    const std::string repWide = "x" + std::to_string(std::max(v.repeatTotal, v.repeatLeft));
    const float repW = repeat ? repGap + std::max(g_fontBold->CalcTextSizeA(repH * 0.78f, FLT_MAX, 0, repWide.c_str()).x + 24 * s, repH) : 0;
    const float line1W = SegsWidth(g_fontBold, big, line1);
    // The pick stroke's sign after them (setting tabPicks), the tab's sign.
    const float pickGap = 20 * s, pickHalf = big * 0.26f, pickW = st.tabPicks ? pickGap + 2 * pickHalf : 0;

    const float textW = std::max({line1W + repW + pickW, SegsWidth(g_fontUi, mid, line2), LinesWidth(g_fontUi, mid, linesT),
                                  LinesWidth(g_fontUi, mid, linesH), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + (line2.empty() ? 0 : mid + 10 * s) + (linesT.size() + linesH.size()) * (mid + 10 * s) + tiny;

    const BannerBox b = BannerLayout(st, S, s, textW, textH, tiny, ds);
    const ImVec2 p0 = b.p0, p1 = b.p1;
    g_box[kBanner] = {p0, p1, true};

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 222), 14 * s);
    dl->AddRect(p0, p1, (col & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 14 * s, 0, 3.5f * s);

    // The text, at the size that fits the box (k).
    const float k = b.k, ks = s * k;
    ImVec2 t = b.text;
    DrawSegs(dl, g_fontBold, big * k, t, line1);
    const float swell = RepeatPulse(repeat ? v.repeatLeft : 0);
    if (repeat)
        DrawRepeatBadge(dl, ImVec2(t.x + (line1W + repGap + (repW - repGap) * 0.5f) * k, t.y + big * k * 0.55f), repH * k,
                        v.repeatLeft, col, i == 1 || i == 3 || i == 4, swell, ks);  // (yellow, orange, green: light)
    if (st.tabPicks && v.pick >= 0)
        DrawPickSign(dl, ImVec2(t.x + (line1W + repW + pickGap + pickHalf) * k, t.y + big * k * 0.52f), pickHalf * k, 3.5f * ks,
                     v.pick, Col(theme::kText));
    t.y += (big + 8 * s) * k;
    if (!line2.empty()) {
        DrawSegs(dl, g_fontUi, mid * k, t, line2);
        t.y += (mid + 10 * s) * k;
    }
    for (const auto& lt : linesT) {
        DrawSegs(dl, g_fontUi, mid * k, t, lt);
        t.y += (mid + 10 * s) * k;
    }
    for (const auto& lh : linesH) {
        DrawSegs(dl, g_fontUi, mid * k, t, lh);
        t.y += (mid + 10 * s) * k;
    }
    if (!DrawHoldCountdown(dl, v, st, ImVec2(t.x, b.keysY), tiny * k, ks)) DrawSegs(dl, g_fontUi, tiny * k, ImVec2(t.x, b.keysY), line3);

    // Picture: a piece of fretboard, or a small tab (thinnest string on top, like tab and sheet music),
    // centred in its slot.
    if (st.bannerNeck) {
        const NeckPic neck = FitNeck(v, st, s, b);
        neck.Draw(dl, st, ImVec2(b.pic.x + (b.picW - neck.w) * 0.5f, b.pic.y + (b.picH - neck.h) * 0.5f));
        return;
    }
    const float gap = 17 * s, tabW = 190 * s, labelW = 22 * s;
    const float tabH = gap * (n - 1);
    const float tx = b.pic.x + (b.picW - labelW - tabW) * 0.5f, ty = b.pic.y + (b.picH - tabH) * 0.5f;
    const MiniTab mt(st, tx, labelW, tabW, s);
    for (int str = 0; str < n; ++str) {
        const float y = ty + MiniTabRow(st, str, n) * gap;
        const bool target = str == i;
        const ImU32 c = target ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (110u << 24));
        const char* name = kStringName[str];
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, name);
        dl->AddText(g_fontUi, tiny, ImVec2(mt.labelX, y - ns.y * 0.5f), c, name);
        dl->AddLine(ImVec2(mt.lineL, y), ImVec2(mt.lineL + tabW, y), c, target ? 4 * s : 2 * s);
    }
    char num[8];
    std::snprintf(num, sizeof(num), "%d", v.fret);
    const ImVec2 bc(mt.lineL + tabW * 0.5f, ty + MiniTabRow(st, i, n) * gap);
    const float fs = 24 * s;
    const ImVec2 nsz = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, num);
    const float rad = std::max(nsz.x, nsz.y) * 0.5f + 7 * s;
    dl->AddCircleFilled(bc, rad, Col(theme::kPanel, 255));
    dl->AddCircle(bc, rad, col, 0, 3 * s);
    dl->AddText(g_fontBold, fs, ImVec2(bc.x - nsz.x * 0.5f, bc.y - nsz.y * 0.5f), Col(theme::kText), num);
}

// The "waiting" banner for a chord: its name, each string to play in its colour with its fret, and
// the chord shape as a tab (a bubble per played string, "x" = don't play that string).
void DrawChordBanner(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    const float s = S * st.bannerSize / 100.0f;
    const int n = v.bass ? 4 : 6;
    const ImU32 gold = Col(theme::kChord);
    const float big = 46 * s, mid = 26 * s, tiny = 20 * s;

    std::vector<int> notes;  // lowest string first
    for (int i = 0; i < n; ++i) if (v.frets[i] >= 0 && v.notes[i] >= 0) notes.push_back(v.notes[i]);
    const bool flats = music::UsesFlats(v.chordName);

    std::vector<Seg> line1;
    if (!v.chordName.empty()) line1 = {{"Play the chord  ", Col(theme::kText)}, {v.chordName, gold}};
    else line1 = {{"Play these strings together", Col(theme::kText)}};

    // What the chord is, in words: "B power chord  -  notes B and F#".
    const std::string meaning = music::ChordMeaning(v.chordName, notes);
    std::vector<Seg> lineM;
    if (!meaning.empty()) lineM.push_back({meaning, gold});
    if (!notes.empty()) lineM.push_back({(meaning.empty() ? "notes " : "   \xC2\xB7   notes ") + music::NoteList(notes, flats), Col(theme::kTextDim)});

    // "string 1 open = E    2 fret 2 = B ..." from the thickest string; more than 3 strings take two rows
    // (one row of 6 was too wide for the banner's box, the text had to shrink a lot).
    int played = 0;
    for (int i = 0; i < n; ++i) played += v.frets[i] >= 0;
    const int perRow = played > 3 ? (played + 1) / 2 : 3;
    std::vector<std::vector<Seg>> lines2;
    int put = 0;
    for (int i = 0; i < n; ++i) {
        if (v.frets[i] < 0) continue;
        // (the second row starts under the first one's numbers: the same "string " but invisible)
        if (put % perRow == 0) lines2.push_back({{"string ", put ? IM_COL32(0, 0, 0, 0) : Col(theme::kTextDim)}});
        else lines2.back().push_back({"     ", Col(theme::kText)});
        ++put;
        auto& line2 = lines2.back();
        line2.push_back({std::to_string(StringNumber(st, i, n)), kStringColor[i]});
        line2.push_back({v.frets[i] == 0 ? " open" : " fret " + std::to_string(v.frets[i]), Col(theme::kText)});
        if (v.notes[i] >= 0) line2.push_back({" = " + music::NoteName(v.notes[i], flats), Col(theme::kTextDim)});
    }
    const std::vector<Seg> line3 = {{played < n ? "x = don't play that string   " : "", Col(theme::kTextDim)}, {"F9 = skip   F8 = menu", Col(theme::kTextDim)}};
    const std::vector<std::vector<Seg>> linesH = WrapLines(g_fontUi, mid, HintLines(v.hint), kBannerTextW * s);
    // How to play it (palm mute, accent, a slide of the whole chord...).
    std::vector<std::vector<Seg>> linesT = HandLines(v, st);
    const auto steps = technique::Describe(v.tech, v.techFret);
    for (size_t k = 0; k < steps.size(); ++k) {
        const std::string num = steps.size() > 1 ? std::to_string(k + 1) + ".  " : "";
        linesT.push_back({{num + steps[k].name + ":  ", gold}, {steps[k].how, Col(theme::kText)}});
    }
    if (const auto hold = HoldWords(v); !hold.empty()) linesT.push_back(hold);
    linesT = WrapLines(g_fontUi, mid, linesT, kBannerTextW * s);

    // A quick repeat of this chord (a power chord strummed again and again): "x5" after the first line,
    // counting down, as for single notes.
    const bool repeat = v.repeatTotal >= 2 && v.repeatLeft >= 1;
    const float repH = big * 0.9f, repGap = 18 * s;
    const std::string repWide = "x" + std::to_string(std::max(v.repeatTotal, v.repeatLeft));
    const float repW = repeat ? repGap + std::max(g_fontBold->CalcTextSizeA(repH * 0.78f, FLT_MAX, 0, repWide.c_str()).x + 24 * s, repH) : 0;
    const float line1W = SegsWidth(g_fontBold, big, line1);
    const float pickGap = 20 * s, pickHalf = big * 0.26f, pickW = st.tabPicks ? pickGap + 2 * pickHalf : 0;

    const float textW = std::max({line1W + repW + pickW, SegsWidth(g_fontUi, mid, lineM), LinesWidth(g_fontUi, mid, lines2),
                                  LinesWidth(g_fontUi, mid, linesT),
                                  LinesWidth(g_fontUi, mid, linesH), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + (lineM.empty() ? 0 : mid + 8 * s) + (lines2.size() + linesT.size() + linesH.size()) * (mid + 10 * s) + tiny;

    const BannerBox b = BannerLayout(st, S, s, textW, textH, tiny, ds);
    const ImVec2 p0 = b.p0, p1 = b.p1;
    g_box[kBanner] = {p0, p1, true};

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 222), 14 * s);
    dl->AddRect(p0, p1, (gold & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 14 * s, 0, 3.5f * s);

    // The text, at the size that fits the box (k).
    const float k = b.k;
    ImVec2 t = b.text;
    DrawSegs(dl, g_fontBold, big * k, t, line1);
    const float swell = RepeatPulse(repeat ? v.repeatLeft : 0);
    if (repeat)
        DrawRepeatBadge(dl, ImVec2(t.x + (line1W + repGap + (repW - repGap) * 0.5f) * k, t.y + big * k * 0.55f), repH * k,
                        v.repeatLeft, gold, true, swell, s * k);
    if (st.tabPicks && v.pick >= 0)
        DrawPickSign(dl, ImVec2(t.x + (line1W + repW + pickGap + pickHalf) * k, t.y + big * k * 0.52f), pickHalf * k, 3.5f * s * k,
                     v.pick, Col(theme::kText));
    t.y += (big + 8 * s) * k;
    if (!lineM.empty()) {
        DrawSegs(dl, g_fontUi, mid * k, t, lineM);
        t.y += (mid + 8 * s) * k;
    }
    for (const auto& l2 : lines2) {
        DrawSegs(dl, g_fontUi, mid * k, t, l2);
        t.y += (mid + 10 * s) * k;
    }
    for (const auto& lt : linesT) {
        DrawSegs(dl, g_fontUi, mid * k, t, lt);
        t.y += (mid + 10 * s) * k;
    }
    for (const auto& lh : linesH) {
        DrawSegs(dl, g_fontUi, mid * k, t, lh);
        t.y += (mid + 10 * s) * k;
    }
    if (!DrawHoldCountdown(dl, v, st, ImVec2(t.x, b.keysY), tiny * k, s * k)) DrawSegs(dl, g_fontUi, tiny * k, ImVec2(t.x, b.keysY), line3);

    // Picture, centred in its slot.
    if (st.bannerNeck) {
        const NeckPic neck = FitNeck(v, st, s, b);
        neck.Draw(dl, st, ImVec2(b.pic.x + (b.picW - neck.w) * 0.5f, b.pic.y + (b.picH - neck.h) * 0.5f));
        return;
    }
    // Tab picture, thinnest string on top; wider string spacing than the single-note tab so a bubble
    // fits on every string (shrunk to the slot's height: 6 strings are a little taller than it).
    const float ps = s * std::min(1.0f, b.picH / (34 * s * (n - 1) + 30 * s));
    const float gap = 34 * ps, tabW = 150 * ps, labelW = 22 * ps, tiny2 = 20 * ps;
    const float tabH = gap * (n - 1);
    const float tx = b.pic.x + (b.picW - labelW - tabW) * 0.5f, ty = b.pic.y + (b.picH - tabH) * 0.5f;
    const MiniTab mt(st, tx, labelW, tabW, ps);
    const float bx = mt.lineL + tabW * 0.5f, fs = 20 * ps;
    for (int str = 0; str < n; ++str) {
        const float y = ty + MiniTabRow(st, str, n) * gap;
        const bool on = v.frets[str] >= 0;
        const ImU32 c = on ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (90u << 24));
        const char* name = kStringName[str];
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny2, FLT_MAX, 0, name);
        dl->AddText(g_fontUi, tiny2, ImVec2(mt.labelX, y - ns.y * 0.5f), c, name);
        dl->AddLine(ImVec2(mt.lineL, y), ImVec2(mt.lineL + tabW, y), c, on ? 4 * ps : 2 * ps);
        const std::string label = on ? std::to_string(v.frets[str]) : "x";
        const ImVec2 lsz = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
        if (on) {
            const float rad = std::min(gap * 0.48f, std::max(lsz.x, lsz.y) * 0.5f + 4 * ps);
            dl->AddCircleFilled(ImVec2(bx, y), rad, Col(theme::kPanel, 255));
            dl->AddCircle(ImVec2(bx, y), rad, kStringColor[str], 0, 3 * ps);
        }
        dl->AddText(g_fontBold, fs, ImVec2(bx - lsz.x * 0.5f, y - lsz.y * 0.5f), on ? Col(theme::kText) : Col(theme::kTextDim), label.c_str());
    }
}

// ------------------------------------------------------------------ the mistake panel
// After a wrong note: what went wrong and where, in its own panel BESIDE the banner, so the banner
// (what to play) never changes while the song waits. A header ("You played C"), the advice wrapped to
// the panel's width, and the same piece of fretboard as the banner with a red X where the note was
// played and an arrow to the right spot.

// Advice pieces as wrapped lines no wider than maxW (word by word; " · " between two pieces of
// advice starts a new line).
std::vector<std::vector<Seg>> WrapSegs(ImFont* f, float size, const std::vector<Seg>& segs, float maxW) {
    std::vector<std::vector<Seg>> lines(1);
    float x = 0;
    auto put = [&](const std::string& t, ImU32 col) {
        if (!lines.back().empty() && lines.back().back().col == col) lines.back().back().text += t;
        else lines.back().push_back({t, col});
    };
    for (const auto& sg : segs) {
        if (sg.text == "   \xC2\xB7   ") {  // hint.cpp's separator between two pieces of advice
            lines.emplace_back();
            x = 0;
            continue;
        }
        size_t i = 0;
        while (i < sg.text.size()) {  // a word and the spaces after it
            size_t j = sg.text.find(' ', i);
            j = j == std::string::npos ? sg.text.size() : sg.text.find_first_not_of(' ', j);
            if (j == std::string::npos) j = sg.text.size();
            std::string word = sg.text.substr(i, j - i);
            const std::string bare = word.substr(0, word.find_last_not_of(' ') + 1);
            if (x > 0 && x + f->CalcTextSizeA(size, FLT_MAX, 0, bare.c_str()).x > maxW) {
                lines.emplace_back();
                x = 0;
            }
            if (x == 0) word.erase(0, word.find_first_not_of(' '));
            if (!word.empty()) {
                put(word, sg.col);
                x += f->CalcTextSizeA(size, FLT_MAX, 0, word.c_str()).x;
            }
            i = j;
        }
    }
    return lines;
}

// Where the panel goes: beside the banner (b0, b1 = its box), moved by the player's offset. Without
// an offset, on the banner's right, else its left, else under it.
ImVec2 MistakePlace(const Settings& st, float S, float w, float h, ImVec2 ds, ImVec2 b0, ImVec2 b1) {
    const float side = 16 * S;
    ImVec2 p(b1.x + side + st.mistakeX * S, b0.y + st.mistakeY * S);
    if (st.mistakeX == 0 && st.mistakeY == 0) {
        if (p.x + w > ds.x - 8 * S) p.x = b0.x - side - w;
        if (p.x < 8 * S) p = ImVec2((b0.x + b1.x - w) * 0.5f, b1.y + side);
    }
    return Place(p.x, p.y, w, h, ds);
}

// b0, b1: the banner's box.
void DrawMistakePanel(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds, ImVec2 b0, ImVec2 b1) {
    if (v.hint.empty()) return;
    const float s = S * st.mistakeSize / 100.0f;
    const float head = 30 * s, mid = 24 * s, pad = 20 * s;
    const ImU32 red = Col(theme::kWarning);

    // Header: "You played C" or "Out of tune?" (the advice's first, grey piece without its dash), or for a
    // chord "Not quite" (its advice starts with a grey "Fix:", dropped). The rest is the advice.
    std::string header = v.chord ? "Not quite" : "Wrong note";
    std::vector<Seg> body;
    for (size_t k = 0; k < v.hint.size(); ++k) {
        const auto& h = v.hint[k];
        if (k == 0 && h.color == hint::kGrey) {
            const size_t dash = h.text.find("  -  ");
            if (h.text.rfind("You played", 0) == 0 || h.text.rfind("Out of tune", 0) == 0) header = h.text.substr(0, dash);
            if (dash != std::string::npos || h.text.rfind("Fix:", 0) == 0) continue;
        }
        body.push_back({h.text, h.color >= 0 && h.color < 6 ? kStringColor[h.color] : (h.color == hint::kGrey ? Col(theme::kTextDim) : Col(theme::kText))});
    }

    const NeckPic neck(v, st, s);
    const bool pic = st.bannerNeck && !neck.marks.empty();
    const float innerW = std::max(pic ? neck.w : 0.0f, 380 * s);
    const auto lines = WrapSegs(g_fontUi, mid, body, innerW);
    const float w = pad + innerW + pad;
    const float h = pad + head + 8 * s + lines.size() * (mid + 8 * s) + (pic ? 6 * s + neck.h : 0) + pad;
    const ImVec2 p0 = MistakePlace(st, S, w, h, ds, b0, b1);
    const ImVec2 p1(p0.x + w, p0.y + h);
    g_box[kMistake] = {p0, p1, true};

    // Fades in (0.2 s) when the advice changes, like the X's.
    static std::string s_key;
    static double s_since = 0;
    const std::string key = hint::Text(v.hint);
    if (key != s_key) {
        s_key = key;
        s_since = ImGui::GetTime();
    }
    const int vtx0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 222), 14 * s);
    dl->AddRect(p0, p1, red, 14 * s, 0, 3 * s);
    ImVec2 t(p0.x + pad, p0.y + pad);
    DrawSegs(dl, g_fontBold, head, t, {{"!  ", red}, {header, red}});
    t.y += head + 8 * s;
    for (const auto& l : lines) {
        DrawSegs(dl, g_fontUi, mid, t, l);
        t.y += mid + 8 * s;
    }
    if (pic) neck.Draw(dl, st, ImVec2(p0.x + (w - neck.w) * 0.5f, t.y + 6 * s));
    FadeFrom(dl, vtx0, (float)std::min(1.0, (ImGui::GetTime() - s_since) / 0.2));
}

// The banner, calm (no flashing): shown while the song waits, and while it plays towards the next stop
// (it then already names that note), so from one note to the next only its colours, text and dots change,
// never the box. (It used to show the next note only 1.5 s ahead: in stop-and-go playing it faded out and
// in between most notes.) It goes away only after kBridgeMs with nothing to show (the end of the notes,
// outside the practised part), fading out, and fades in when it comes back. Returns true if it drew
// something.
bool DrawCalmBanner(ImDrawList* dl, const View& v, const Settings& st, bool on, float S, ImVec2 ds) {
    constexpr DWORD kBridgeMs = 700;  // a gap shorter than this never hides the banner
    static Fade s_fade;
    static View s_note;  // the note fields it shows (kept while it fades out)
    static bool s_have = false;
    static DWORD s_lastWanted = 0;
    const DWORD now = GetTickCount();
    const bool next = v.upcoming && v.nextWaitTime >= 0;
    const bool want = on && v.inSong && (v.waiting || next);
    if (want) {  // only the banner's fields (not the tab's note lists)
        s_note.bass = v.bass;
        s_note.string = v.string;
        s_note.fret = v.fret;
        s_note.midi = v.midi;
        s_note.sustain = v.sustain;
        s_note.pick = v.pick;
        s_note.inShape = v.inShape;
        s_note.shapeName = v.shapeName;
        std::copy(std::begin(v.shapeFrets), std::end(v.shapeFrets), s_note.shapeFrets);
        s_note.repeatLeft = v.repeatLeft;
        s_note.repeatTotal = v.repeatTotal;
        s_note.tech = v.tech;
        s_note.techFret = v.techFret;
        std::copy(std::begin(v.strings), std::end(v.strings), s_note.strings);
        s_note.anchorFret = v.anchorFret;
        s_note.anchorWidth = v.anchorWidth;
        s_note.handFrom = v.handFrom;
        std::copy(std::begin(v.fingers), std::end(v.fingers), s_note.fingers);
        s_note.chain = v.chain;
        s_note.chord = v.chord;
        s_note.chordName = v.chordName;
        std::copy(std::begin(v.frets), std::end(v.frets), s_note.frets);
        std::copy(std::begin(v.notes), std::end(v.notes), s_note.notes);
        s_note.hint = v.hint;
        s_note.heardAt = v.heardAt;
        s_have = true;
        s_lastWanted = now;
    }
    {  // the hold countdown runs on the song's clock, whatever the banner shows
        s_note.songTime = v.songTime;
        s_note.holdFrom = v.holdFrom;
        s_note.holdLen = v.holdLen;
        s_note.holdFret = v.holdFret;
        s_note.holdString = v.holdString;
        s_note.holdName = v.holdName;
    }
    const bool keep = on && v.inSong && s_have && now - s_lastWanted < kBridgeMs;
    s_fade.Step(want || keep, ImGui::GetIO().DeltaTime, 0.2f, 0.4f);
    if (!s_have || s_fade.alpha <= 0) return false;
    const int vtx0 = dl->VtxBuffer.Size;
    // The banner shows only what to play; a wrong note goes to the mistake panel beside it.
    View plain = s_note;
    plain.hint.clear();
    plain.heardAt.clear();
    if (s_note.chord) DrawChordBanner(dl, plain, st, S, ds);
    else DrawBanner(dl, plain, st, S, ds);
    DrawMistakePanel(dl, s_note, st, S, ds, g_box[kBanner].p0, g_box[kBanner].p1);
    FadeFrom(dl, vtx0, s_fade.alpha);
    return true;
}

// ------------------------------------------------------------------ the practice bar
// Like a video editor's timeline, on the game's own progress bar (the same length and place): drag on
// it with the mouse (no menu needed) to mark parts of the song. The mod waits only inside them, and
// the tab shades them. Drag on an empty stretch = a new part; drag a part's end = move it; click a
// part (without dragging) = remove it. Ends snap to phrase starts nearby; overlapping parts merge.
// (Not the right button: the game opens its pause screen with it, whatever the window hook does.)
// The game hides the mouse pointer in a song, so the bar draws one for a moment after it moves.
std::vector<Range> g_drawRanges;  // the parts, for drawing (render thread)

// The section of the song at time t ("Chorus 2"), "" = none known.
std::string SectionAt(const View& v, double t) {
    std::string name;
    for (const auto& [start, n] : v.sections) {
        if (start > t + 0.05) break;
        name = n;
    }
    return name;
}

// The game's progress bar, measured on a 3440x1440 screen: its picture is 16:9, centred and scaled by
// height; 4.8% to 11.7% of its height. It is a straight time axis from song time 0 at 10.4% of the
// width to the song's end at 89.7%, but the game only draws it from the SECOND phrase on (the first
// one is the intro before the notes), so it starts further right in songs with a long intro.
// (Measured 2026-10-02 from the phrase edges: Ode to Joy, first phrase 10.0-12.56 s, and the Seven
// Nation Army remix, 0-33.22 s, both put time 0 at x 706 of 3440. The first version took the bar's
// left end as the first phrase's start: in the remix a part landed ~20 s off the game's selection.)
struct GameBar {
    float zero, x1, top, bottom;  // x of song time 0 and of the song's end
    explicit GameBar(ImVec2 ds) {
        float gw = ds.y * 16.0f / 9.0f, gh = ds.y, gx = (ds.x - gw) * 0.5f, gy = 0;
        if (gw > ds.x) { gw = ds.x; gh = ds.x * 9.0f / 16.0f; gx = 0; gy = (ds.y - gh) * 0.5f; }
        zero = gx + 0.104f * gw;
        x1 = gx + 0.897f * gw;
        top = gy + 0.048f * gh;
        bottom = gy + 0.117f * gh;
    }
};

// Sorted, overlapping parts merged.
void Tidy(std::vector<Range>* r) {
    std::sort(r->begin(), r->end());
    std::vector<Range> out;
    for (const auto& p : *r) {
        if (p.second - p.first < 0.3) continue;
        if (!out.empty() && p.first <= out.back().second) out.back().second = std::max(out.back().second, p.second);
        else out.push_back(p);
    }
    *r = out;
}

void DrawPracticeBar(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    if (!st.showPracticeBar || !v.inSong || v.songLength <= 1 || v.songTime < 0) {
        g_barL = g_barT = g_barR = g_barB = 0;
        return;
    }
    const GameBar gb(ds);
    const double t1 = v.songLength;  // the axis: 0 .. t1 from gb.zero to gb.x1
    const double t0 = v.phraseStarts.size() > 1 ? v.phraseStarts[1] : 0.0;  // where the game's bar starts
    const float span = gb.x1 - gb.zero;
    auto X = [&](double t) { return gb.zero + (float)(std::max(t0, std::min(t1, t)) / t1) * span; };
    auto T = [&](float x) { return std::max(t0, std::min(t1, (double)(x - gb.zero) / span * t1)); };
    const float x0 = X(t0), x1 = gb.x1;
    const float h = 10 * S, y1 = gb.bottom - 16 * S, y0 = y1 - h;  // our strip: low in the game's bar

    // The mouse in drawing coordinates (the window's client area may be scaled to the back buffer).
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const float sx = rc.right > 0 ? ds.x / rc.right : 1, sy = rc.bottom > 0 ? ds.y / rc.bottom : 1;
    const ImVec2 mouse(g_mouseX * sx, g_mouseY * sy);
    // The clickable box: the whole height of the game's bar, in client pixels for the window hook.
    g_barL = (int)(x0 / sx) - 4;
    g_barR = (int)(x1 / sx) + 4;
    g_barT = (int)(gb.top / sy);
    g_barB = (int)(gb.bottom / sy);
    const bool hover = mouse.x >= x0 - 4 && mouse.x <= x1 + 4 && mouse.y >= gb.top && mouse.y <= gb.bottom;

    // The phrase at song time t: its start and end (the next phrase's start, or the song's end).
    auto phraseAt = [&](double t) {
        Range r(t0, t1);
        for (size_t i = 0; i < v.phraseStarts.size(); ++i) {
            if (v.phraseStarts[i] > t) break;
            r.first = v.phraseStarts[i];
            r.second = i + 1 < v.phraseStarts.size() ? v.phraseStarts[i + 1] : t1;
        }
        return r;
    };
    bool anyHeat = false;
    for (float hgt : v.phraseHeat) anyHeat = anyHeat || hgt > 0;

    auto snap = [&](double t) {  // to a phrase start within 10 px
        double best = t;
        float bestD = 10 * S;
        for (double p : v.phraseStarts) {
            const float d = std::abs(X(p) - X(t));
            if (d < bestD) { bestD = d; best = p; }
        }
        return best;
    };

    // Dragging (render thread; the hook only reports the button).
    static bool s_down = false, s_dragged = false;
    static int s_part = -1;      // the part being changed or clicked (-1 = a new one)
    static int s_end = 0;        // 0 = a new part / a click, 1 = moving its start, 2 = moving its end
    static double s_anchor = 0;
    static float s_pressX = 0;
    std::vector<Range> parts = g_drawRanges;
    const bool down = g_mouseLeft;
    if (down && !s_down) {  // pressed: on an end, inside a part, or on an empty stretch
        s_pressX = mouse.x;
        s_dragged = false;
        s_part = -1;
        s_end = 0;
        for (size_t i = 0; i < parts.size() && s_end == 0; ++i) {
            if (std::abs(mouse.x - X(parts[i].first)) < 9 * S) { s_part = (int)i; s_end = 1; }
            else if (std::abs(mouse.x - X(parts[i].second)) < 9 * S) { s_part = (int)i; s_end = 2; }
            else if (mouse.x > X(parts[i].first) && mouse.x < X(parts[i].second)) s_part = (int)i;
        }
        s_anchor = snap(T(mouse.x));
    }
    if (down && std::abs(mouse.x - s_pressX) > 5 * S) s_dragged = true;
    bool changed = false;
    if (down && s_dragged) {
        const double t = snap(T(mouse.x));
        if (s_end == 1 && s_part >= 0) { parts[s_part].first = std::min(t, parts[s_part].second - 0.3); changed = true; }
        else if (s_end == 2 && s_part >= 0) { parts[s_part].second = std::max(t, parts[s_part].first + 0.3); changed = true; }
        else if (std::abs(t - s_anchor) >= 0.3) {
            // A new part: the last one in the list while it's being dragged.
            const Range r(std::min(t, s_anchor), std::max(t, s_anchor));
            if (s_end != 3) { parts.push_back(r); s_end = 3; s_part = (int)parts.size() - 1; }
            else parts[s_part] = r;
            changed = true;
        }
    }
    if (!down && s_down) {
        if (!s_dragged && s_part >= 0 && s_end == 0) { parts.erase(parts.begin() + s_part); changed = true; }  // a click on a part
        else if (!s_dragged && s_part < 0 && s_end == 0 && T(mouse.x) >= t0) parts.push_back(phraseAt(T(mouse.x)));  // on a phrase
        Tidy(&parts);
        changed = true;
        s_end = 0;
        s_part = -1;
    }
    s_down = down;
    if (changed && parts != g_drawRanges) {
        g_drawRanges = parts;
        std::vector<Range> tidy = parts;
        Tidy(&tidy);
        std::lock_guard<std::mutex> lk(g.m);
        g.ranges = tidy;
    }

    // Drawing. The parts: shaded over the game's bar (its whole height), with handles; our strip low
    // in the bar: phrase ticks, what has been played, the "now" line.
    for (const auto& p : parts) {
        const float xa = X(p.first), xb = X(p.second);
        dl->AddRectFilled(ImVec2(xa, gb.top), ImVec2(xb, gb.bottom), Col(theme::kChord, 70), 3 * S);
        dl->AddRect(ImVec2(xa, gb.top), ImVec2(xb, gb.bottom), Col(theme::kChord, 255), 3 * S, 0, 2 * S);
        for (float x : {xa, xb}) {  // the handles
            dl->AddRectFilled(ImVec2(x - 3 * S, gb.top - 4 * S), ImVec2(x + 3 * S, gb.bottom + 4 * S), Col(theme::kChord, 255), 2 * S);
            dl->AddLine(ImVec2(x, gb.top), ImVec2(x, gb.bottom), Col(theme::kPanel, 255), 1.2f * S);
        }
    }
    if (hover || !parts.empty() || anyHeat) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(theme::kPanel, 170), h * 0.5f);
        // Trouble spots: each phrase in red, stronger where the player stopped more (stats.h).
        for (size_t i = 0; i < v.phraseHeat.size() && i < v.phraseStarts.size(); ++i) {
            if (v.phraseHeat[i] <= 0) continue;
            const double b = i + 1 < v.phraseStarts.size() ? v.phraseStarts[i + 1] : t1;
            const int alpha = 70 + (int)(170 * v.phraseHeat[i]);
            dl->AddRectFilled(ImVec2(X(v.phraseStarts[i]), y0), ImVec2(X(b), y1), IM_COL32(225, 60, 50, alpha));
        }
        for (double p : v.phraseStarts) dl->AddLine(ImVec2(X(p), y0 + 2 * S), ImVec2(X(p), y1 - 2 * S), Col(theme::kTextDim, 120), 1 * S);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(X(v.songTime), y1), Col(theme::kText, 50), h * 0.5f);
        dl->AddLine(ImVec2(X(v.songTime), y0 - 3 * S), ImVec2(X(v.songTime), y1 + 3 * S), Col(theme::kText, 230), 2 * S);
    }

    // Words under the bar: the parts, or how to use the bar (while the mouse is on it).
    auto mmss = [](double t) {
        char buf[16];
        const int x = (int)std::max(0.0, t);
        std::snprintf(buf, sizeof(buf), "%d:%02d", x / 60, x % 60);
        return std::string(buf);
    };
    std::string text;
    const std::string partSec = parts.size() == 1 ? SectionAt(v, parts[0].first) : "";
    if (parts.size() == 1)
        text = "practising " + (partSec.empty() ? "" : partSec + " (") + mmss(parts[0].first) + " - " + mmss(parts[0].second) +
               (partSec.empty() ? "" : ")");
    else if (parts.size() > 1) text = "practising " + std::to_string(parts.size()) + " parts";
    const std::string mouseSec = hover ? SectionAt(v, T(mouse.x)) : "";  // the section under the mouse
    if (hover)
        text += std::string(text.empty() ? "" : "   \xC2\xB7   ") + (mouseSec.empty() ? "" : mouseSec + "   \xC2\xB7   ") +
                (anyHeat ? "red: where you stopped most   " : "") +
                "click: practise a phrase   drag: add a part   click a part: remove it   drag an end: change it";
    if (!text.empty()) {
        const float fs = 17 * S;
        const ImVec2 ts = g_fontUi->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
        const ImVec2 p(x1 - ts.x, gb.bottom + 6 * S);
        dl->AddRectFilled(ImVec2(p.x - 6 * S, p.y - 1 * S), ImVec2(x1 + 2 * S, p.y + ts.y + 1 * S), Col(theme::kPanel, 210), 4 * S);
        dl->AddText(g_fontUi, fs, p, parts.empty() ? Col(theme::kTextDim) : Col(theme::kChord), text.c_str());
    }
    // The mouse pointer (the game hides the real one): for 2 s after it moves, or while dragging.
    if (down || GetTickCount() - g_mouseMoved < 2000) {
        const ImVec2 m = mouse;
        const ImVec2 p1(m.x, m.y + 18 * S), p2(m.x + 12 * S, m.y + 13 * S);
        dl->AddTriangleFilled(ImVec2(m.x - 1.5f * S, m.y - 2 * S), ImVec2(p1.x - 1.5f * S, p1.y + 2 * S), ImVec2(p2.x + 2 * S, p2.y + 1 * S), IM_COL32(0, 0, 0, 200));
        dl->AddTriangleFilled(m, p1, p2, IM_COL32(255, 255, 255, 240));
    }
}

// The count-in after a long wait: a big number (beats left before the song goes on) under the banner.
// It changes once a beat (at most ~3 a second at 180 bpm; never a flash: it only changes its digit).
void DrawCountIn(ImDrawList* dl, const View& v, float S, ImVec2 ds) {
    if (v.countIn <= 0) return;
    const float s = S;
    const std::string num = std::to_string(v.countIn), sub = "the song goes on in";
    const float big = 96 * s, subSize = 22 * s, pad = 16 * s;
    const ImVec2 ns = g_fontBold->CalcTextSizeA(big, FLT_MAX, 0, num.c_str());
    const ImVec2 ss = g_fontUi->CalcTextSizeA(subSize, FLT_MAX, 0, sub.c_str());
    const float w = std::max(ns.x, ss.x) + pad * 2, h = ss.y + ns.y + pad * 2;
    // Under the banner if it's up, else at its default place.
    const float cx = g_box[kBanner].drawn ? (g_box[kBanner].p0.x + g_box[kBanner].p1.x) * 0.5f : ds.x * 0.5f;
    const float top = g_box[kBanner].drawn ? g_box[kBanner].p1.y + 12 * s : 140 * s;
    const ImVec2 p0 = Place(cx - w * 0.5f, top, w, h, ds), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 225), 14 * s);
    dl->AddRect(p0, p1, Col(theme::kChord, 200), 14 * s, 0, 3 * s);
    dl->AddText(g_fontUi, subSize, ImVec2(p0.x + (w - ss.x) * 0.5f, p0.y + pad * 0.7f), Col(theme::kTextDim), sub.c_str());
    dl->AddText(g_fontBold, big, ImVec2(p0.x + (w - ns.x) * 0.5f, p0.y + pad * 0.7f + ss.y), Col(theme::kChord), num.c_str());
}

// The song clock, top-left by default: "1:23 / 4:28   Chorus 2" (the section playing). Small and quiet,
// the game's HUD stays readable.
void DrawClock(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    const float s = S * st.clockSize / 100.0f;
    auto mmss = [](double t) {
        const int x = (int)std::max(0.0, t);
        char b[16];
        std::snprintf(b, sizeof(b), "%d:%02d", x / 60, x % 60);
        return std::string(b);
    };
    const std::string text = mmss(v.songTime) + (v.songLength > 0 ? "  /  " + mmss(v.songLength) : "");
    const float size = 26 * s, padX = 14 * s, padY = 6 * s;
    const ImVec2 ts = g_fontBold->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const std::string sec = v.section.empty() ? "" : "   " + v.section;
    const float secW = sec.empty() ? 0 : g_fontBold->CalcTextSizeA(size, FLT_MAX, 0, sec.c_str()).x;
    const float w = ts.x + secW + 2 * padX, h = ts.y + 2 * padY;
    const ImVec2 p0 = Place(st.clockX * S, st.clockY * S, w, h, ds);
    const ImVec2 p1(p0.x + w, p0.y + h);
    g_box[kClock] = {p0, p1, true};
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 170), 8 * s);
    dl->AddText(g_fontBold, size, ImVec2(p0.x + padX, p0.y + padY), Col(theme::kText, 230), text.c_str());
    if (!sec.empty()) dl->AddText(g_fontBold, size, ImVec2(p0.x + padX + ts.x, p0.y + padY), Col(theme::kChord, 230), sec.c_str());
}

// ---- The scrolling tab: DrawTab() and its parts ----------------------------------------------------

// One item on the tab: a note or chord. With "spread" on, a fast repeat of the same fret on the same
// string (4+ notes, each within kRunGap of the previous one) becomes ONE item drawn as "12 x8" (x = how
// many are still to play), so a tremolo-like run doesn't fill the tab.
struct TabItem {
    const TabNote* note;  // the (first) note: frets, chord name, ignore
    double time, last;    // first and last note's time (the same unless it's a run)
    double end;           // when the last note stops ringing
    int count, left;      // notes in the run / still to play (1 / 1 for a plain note)
    float half;           // half the width it takes on screen (widest fret box or chord name)
};

constexpr double kRunGap = 0.12;   // 16th notes at 125 bpm or faster
constexpr double kMaxZoom = 6.0;   // the most the tab zooms in for a fast passage
constexpr double kMinNote = 0.75;  // a fast passage first shrinks the notes down to this (of the player's size)
constexpr int kMaxRows = 4;

// Fast passages: first the notes shrink (down to kMinNote of the player's size), so the tab keeps its
// speed; only past that does it zoom (move faster). need = the zoom needed at the player's note size.
double TabShrink(double need) { return std::max(kMinNote, std::min(1.0, 1.0 / need)); }
double TabZoom(double need) { return std::max(1.0, need * TabShrink(need)); }

// The tab's geometry and the layout of the staff being drawn (Layout()), shared by the parts below.
struct TabStaff {
    ImDrawList* dl = nullptr;
    float s = 1;               // the tab's own size: string gap, text
    int n = 6;                 // strings (4 on bass)
    float gap = 0, top = 0;    // between two strings; above the staff (chord names, bar numbers)
    float rowH = 0;            // one staff with its lanes
    float stemLen = 0, tiny = 0;
    float lineL = 0, lineR = 0, labelX = 0;  // where the strings start and end, where their names start
    bool thickTop = false;     // thickest string on top (tabThickTop)
    bool mirror = false;       // left-handed (tabMirror): time runs right to left
    float dir = 1;             // +1: later notes are to the right; -1: to the left
    float pxPerS = 1;          // speed at zoom 1
    float baseNote = 1;        // the player's note size (menu)
    // The staff being drawn, set by Layout():
    float nsz = 1, fs = 0;     // size of the fret numbers (of the normal size) and their font size
    double zoom = 1;
    double originT = 0;        // time -> x: x = originX + (t - originT) * pxPerS * zoom
    float originX = 0;
    float staffY = 0;          // top of the staff (of its row)

    // Sets up one staff: its zoom need (note size + zoom), the song time at originX, its top.
    void Layout(double need, double t, float y) {
        nsz = baseNote * (float)TabShrink(need);
        fs = 21 * s * nsz;
        zoom = TabZoom(need);
        originT = t;
        staffY = y;
    }
    // String -> y: thinnest on top (printed tab), or thickest on top. Code that needs the staff's top
    // or bottom line uses TopY()/BotY(), not a particular string.
    float RowY(int str) const { return staffY + top + (thickTop ? str : n - 1 - str) * gap; }
    float TopY() const { return staffY + top; }
    float BotY() const { return staffY + top + (n - 1) * gap; }
    // Time -> x. All the timing (cursor start, pages, zoom) is worked out left to right as usual; only
    // this flips the result when left-handed, so drawing code must not assume "later = further right"
    // (it uses dir).
    float TimeX(double t) const {
        const float x = originX + (float)((t - originT) * pxPerS * zoom);
        return mirror ? lineL + lineR - x : x;
    }
    // Half the width of a fret box: the number, and for a run the small "x8" after it.
    float BoxHalf(const std::string& fret, const std::string& run) const {
        const ImVec2 ls = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, fret.c_str());
        float bw = std::max(ls.x, ls.y * 0.8f);
        if (!run.empty()) bw += 3 * s * nsz + g_fontUi->CalcTextSizeA(tiny * 0.85f * nsz, FLT_MAX, 0, run.c_str()).x;
        return bw * 0.5f + 5 * s * nsz;
    }
};

// A run's small "x8" (how many are still to play); empty for a plain note.
std::string RunText(const TabItem& it) {
    return it.count > 1 ? "x" + std::to_string(it.left > 0 ? it.left : it.count) : std::string();
}

// Where the cursor ("now" line) is drawn. holdT: the note the song waits for, or will stop at next if
// it isn't played (-1 = none). The cursor never passes holdT: the song stops a little past that note
// (chords: up to 200 ms while the chord detector decides), and a cursor following the song there had
// to jump back onto the note. So it stops ON the note as the song reaches it, and once the note is
// played or skipped it catches up with the song at 2.5x speed (a 0.2 s gap closes in ~0.13 s)
// instead of jumping forward. A seek or a new song moves it at once.
double TabCursor(double now, double holdT) {
    const double want = holdT >= 0 ? std::min(now, holdT) : now;
    static double s_cursorT = -1e9;
    const double frameDt = std::min(0.1, (double)ImGui::GetIO().DeltaTime);
    // A seek, a new song (big jump either way) or a small step back: follow it at once.
    if (want > s_cursorT + 1.0 || want < s_cursorT) s_cursorT = want;
    else s_cursorT = std::min(want, s_cursorT + frameDt * 2.5);
    return s_cursorT;
}

// The View's notes as tab items (runs merged when spread is on). nextFrom: notes from this time on are
// still to play (a run's "x8" counts them).
std::vector<TabItem> TabItems(const View& v, const TabStaff& tab, bool spread, double nextFrom) {
    auto sameFret = [](const TabNote& a, const TabNote& b) {
        return !a.chord && !b.chord && a.ignore == b.ignore && std::equal(std::begin(a.frets), std::end(a.frets), b.frets);
    };
    std::vector<TabItem> items;
    for (size_t i = 0; i < v.tab.size();) {
        size_t j = i + 1;
        if (spread)
            while (j < v.tab.size() && sameFret(v.tab[j], v.tab[i]) && v.tab[j].time - v.tab[j - 1].time <= kRunGap) ++j;
        if (j - i < 4) j = i + 1;  // 2 or 3 quick repeats stay separate notes
        TabItem it{&v.tab[i], v.tab[i].time, v.tab[j - 1].time, v.tab[j - 1].time + v.tab[j - 1].sustain, (int)(j - i), 0, 0};
        for (size_t k = i; k < j; ++k) it.left += v.tab[k].time >= nextFrom;
        const std::string run = RunText(it);
        for (int str = 0; str < tab.n; ++str)
            if (it.note->frets[str] >= 0) it.half = std::max(it.half, tab.BoxHalf(std::to_string(it.note->frets[str]), run));
        if (it.note->chord && !it.note->name.empty())
            it.half = std::max(it.half, g_fontBold->CalcTextSizeA(tab.tiny, FLT_MAX, 0, it.note->name.c_str()).x * 0.5f + 2 * tab.s);
        items.push_back(it);
        i = j;
    }
    return items;
}

// Time -> x uses ONE speed for the whole tab at any moment, so every note moves at the same speed and
// the spacing stays exactly proportional to time (the rhythm reads true). Spread: the tab zooms in
// when the notes coming up are too close to read. This is the zoom the gaps between from and to need:
// both half widths + a small gap, over the gap's length, at most kMaxZoom (1 = none).
// (Tried before: stretching each gap on its own / a speed that varies along the tab: the notes sped
// up and slowed down as they moved.)
double ZoomNeed(const std::vector<TabItem>& items, const TabStaff& tab, bool spread, double from, double to) {
    double need = 1.0;
    if (!spread) return need;
    for (size_t i = 1; i < items.size(); ++i) {
        const double a = items[i - 1].time, dt = items[i].time - a;
        if (dt <= 0 || items[i].time < from || a > to) continue;
        const double px = (items[i - 1].half + items[i].half + 4 * tab.s) / tab.pxPerS;
        need = std::max(need, std::min(kMaxZoom, px / dt));
    }
    return need;
}

// What goes on each row: page k (0 = the cursor's page) starts at t[k] (the song time at originX) with
// zoom need need[k], on row row[k]; its first part repeats the previous page up to recapEnd[k] (drawn
// dimmed). Scrolling and single pages: one page.
struct TabPages {
    int count = 1;
    double t[kMaxRows] = {};
    double need[kMaxRows] = {1, 1, 1, 1};
    double recapEnd[kMaxRows] = {-1e9, -1e9, -1e9, -1e9};
    int row[kMaxRows] = {0, 1, 2, 3};
};

double g_tabZoom = 1.0;  // the zoom need being shown (smoothed while scrolling / between pages)

// Scrolling: the notes move past a fixed "now" line (nowX). The zoom follows the target smoothly
// (zooming in in ~0.4 s, BEFORE the dense passage arrives, since the target looks ahead the whole tab;
// back out in ~1.5 s once it has passed), so the speed only changes gently.
TabPages ScrollingPage(TabStaff* tab, double cursorT, float nowX, double target, double frameS) {
    const double tau = target > g_tabZoom ? 0.4 : 1.5;
    g_tabZoom += (target - g_tabZoom) * (1.0 - std::exp(-frameS / tau));
    tab->originX = nowX;
    TabPages p;
    p.t[0] = cursorT;
    p.need[0] = g_tabZoom;
    return p;
}

// Pages (like Guitar Pro / Songsterr): the notes stand still and a cursor moves over them; fixed numbers
// stay readable however fast the cursor goes. When the cursor passes 75 % of the width, the page turns:
// the tab glides left (~0.3 s) so the cursor is back near the left edge. The zoom only changes when a
// page turns (a denser passage coming up than this page was laid out for turns the page early), so
// notes on a page never move. recap: how much of the previous page a new page repeats on its left.
TabPages TurningPage(TabStaff* tab, double now, float pageL, double recap, double target, double frameS) {
    static double s_pageT = -1e9;   // song time at the left edge of the page (where it's going)
    static double s_shownT = -1e9;  // same, as shown (glides to s_pageT)
    static double s_pageNeed = 1.0; // zoom need the page was laid out for
    tab->originX = pageL;
    const float width = tab->lineR - pageL;
    auto pageLen = [&](double need) { return width / (tab->pxPerS * TabZoom(need)); };  // seconds on a page
    const double cursor = (now - s_pageT) / pageLen(s_pageNeed);  // 0..1 across the page
    const bool jumped = now < s_shownT - 0.05 || now > s_shownT + 3 * pageLen(s_pageNeed);  // seek / new song
    const double turnAt = recap + (1.0 - recap) * 0.73;  // 75 % of the width with the default recap
    if (jumped || cursor > turnAt || target > s_pageNeed * 1.25) {
        s_pageNeed = target;
        s_pageT = now - recap * pageLen(target);
        if (jumped) { s_shownT = s_pageT; g_tabZoom = s_pageNeed; }
    }
    const double k = 1.0 - std::exp(-frameS / 0.1);
    s_shownT += (s_pageT - s_shownT) * k;
    g_tabZoom += (s_pageNeed - g_tabZoom) * k;
    TabPages p;
    p.t[0] = s_shownT;
    p.need[0] = g_tabZoom;
    return p;
}

// Several rows (pages only). Pages follow each other: the next page starts where the cursor leaves
// this one, minus the recap (the end of this page, repeated on the left of the next), so the cursor
// jumps from the right end of one row to the same point in the music on the next row. Each page's
// zoom is set from the notes on it alone, when it's laid out, so its notes never move. The rows take
// turns top to bottom: when the cursor leaves a row, that row gets the page after the last one.
TabPages RowPages(TabStaff* tab, const std::vector<TabItem>& items, bool spread, double now, float pageL, double recap, int rows) {
    static double s_rowT = -1e9;   // song time at the left edge of the current page
    static double s_rowNeed = 1.0; // its zoom need
    static int s_row = 0;          // the row the current page is on
    tab->originX = pageL;
    const float width = tab->lineR - pageL;
    auto pageLen = [&](double need) { return width / (tab->pxPerS * TabZoom(need)); };  // seconds on a page
    // The need of the page starting at t: measured over the longest a page can be (need 1).
    auto pageNeed = [&](double t) { return ZoomNeed(items, *tab, spread, t - 0.2, t + pageLen(1.0)); };
    auto after = [&](double t, double need) { return t + pageLen(need) * (1.0 - recap); };
    if (now < s_rowT - 0.05 || now > s_rowT + 2 * pageLen(s_rowNeed)) {  // seek / new song
        s_rowNeed = pageNeed(now);
        s_rowT = now - recap * pageLen(s_rowNeed);
        s_row = 0;
    }
    s_row %= rows;  // (the number of rows was changed in the menu)
    for (int i = 0; i < 8 && now >= s_rowT + pageLen(s_rowNeed); ++i) {  // the cursor left the row
        s_rowT = after(s_rowT, s_rowNeed);
        s_rowNeed = pageNeed(s_rowT);
        s_row = (s_row + 1) % rows;
    }
    TabPages p;
    p.count = rows;
    p.t[0] = s_rowT;
    p.need[0] = s_rowNeed;
    for (int k = 0; k < rows; ++k) {
        if (k > 0) {
            p.t[k] = after(p.t[k - 1], p.need[k - 1]);
            p.need[k] = pageNeed(p.t[k]);
            p.recapEnd[k] = p.t[k - 1] + pageLen(p.need[k - 1]);  // where the previous page ends
        }
        p.row[k] = (s_row + k) % rows;
    }
    g_tabZoom = s_rowNeed;
    return p;
}

// What a staff shows besides its layout: the notes, where the player is, what to dim and highlight.
struct TabShow {
    const View* v = nullptr;
    const Settings* st = nullptr;
    const std::vector<TabItem>* items = nullptr;
    bool rhythm = false;      // the rhythm lane under the staff
    double now = 0, cursorT = 0;
    double nextFrom = 0;      // the next note to play is the first item not ignored with last >= this
    double dimBefore = -1e9;  // rows still to come: notes before this (the recap) are drawn dimmed
    bool nextFound = false;   // the next note to play is highlighted once (on the cursor's row first)
    float highlight = 1.0f;   // strength of that highlight (the dimmed copy on a coming row: fainter)
    float pulse = 1.0f;       // the highlight's pulsing
};

// Played/passed notes fade out over half a second after they end (held notes stay while they ring);
// ignored ones are always faint. On pages they stay, dimmed, until the page turns (the left part of the
// page isn't left empty).
float TabAlpha(const TabItem& it, const TabShow& sh) {
    float a = std::max(0.0f, 1.0f - (float)std::max(0.0, sh.now - it.end) / 0.5f);
    if (sh.st->tabPage) a = std::max(a, 0.35f);
    if (it.last < sh.dimBefore) a = std::min(a, 0.35f);    // the recap of a row still to come
    if (it.last < sh.v->greyTime) a = std::min(a, 0.35f);  // greyed out on the highway (replayed after a resume)
    return it.note->ignore ? a * 0.4f : a;
}

// The strings (in the highway's colours) with their names.
void DrawTabStrings(const TabStaff& tab) {
    for (int str = 0; str < tab.n; ++str) {
        const float y = tab.RowY(str);
        const ImU32 c = (kStringColor[str] & 0x00FFFFFF) | (150u << 24);
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tab.tiny, FLT_MAX, 0, kStringName[str]);
        tab.dl->AddText(g_fontUi, tab.tiny, ImVec2(tab.labelX, y - ns.y * 0.5f), c, kStringName[str]);
        tab.dl->AddLine(ImVec2(tab.lineL, y), ImVec2(tab.lineR, y), c, 1.5f * tab.s);
    }
}

// Rhythm grid, under everything else (like the bar lines of printed tab): every other bar gets a faint
// shade so bars read at a glance, each bar starts with a clear line and its number, and the other beats
// get a thin faint line. Notes between two beat lines are "in between" the beats.
void DrawTabGrid(const TabStaff& tab, const std::vector<TabBeat>& grid, float staffTop, float staffBottom) {
    const float s = tab.s;
    for (size_t i = 0; i < grid.size(); ++i) {
        const TabBeat& b = grid[i];
        if (!b.downbeat || (b.measure & 1) == 0) continue;
        double end = b.time + 3600;  // until the next bar line (or off the right edge)
        for (size_t j = i + 1; j < grid.size(); ++j)
            if (grid[j].downbeat) { end = grid[j].time; break; }
        const float xs = tab.TimeX(b.time), xe = tab.TimeX(end);  // (xe < xs when left-handed)
        const float xa = std::max(tab.lineL, std::min(xs, xe)), xb = std::min(tab.lineR, std::max(xs, xe));
        if (xb > xa) tab.dl->AddRectFilled(ImVec2(xa, staffTop), ImVec2(xb, staffBottom), Col(theme::kGrid, 14));
    }
    for (const TabBeat& b : grid) {
        const float x = tab.TimeX(b.time);
        if (x < tab.lineL - 4 * s || x > tab.lineR + 4 * s) continue;
        if (b.downbeat) {
            tab.dl->AddLine(ImVec2(x, staffTop), ImVec2(x, staffBottom), Col(theme::kGrid, 150), 2 * s);
            const std::string num = std::to_string(b.measure);
            // Just before the line (a note on the bar line has its pick mark right on it, and a section's
            // name comes after it), just above the top string's fret boxes.
            const ImVec2 ns = g_fontUi->CalcTextSizeA(15 * s, FLT_MAX, 0, num.c_str());
            const float numY = tab.TopY() - tab.gap * 0.46f - 2 * s - ns.y;
            const float numX = tab.mirror ? x + 7 * s : x - 7 * s - ns.x;
            tab.dl->AddText(g_fontUi, 15 * s, ImVec2(std::floor(numX), std::floor(numY)), Col(theme::kRhythm, 170), num.c_str());
        } else {
            tab.dl->AddLine(ImVec2(x, staffTop + 6 * s), ImVec2(x, staffBottom - 6 * s), Col(theme::kGrid, 45), 1 * s);
        }
    }
}

// Where a section of the song starts: a gold line across the staff and its name ("Chorus 2") on the bar
// numbers' row, just after the line (clear of the bar number centred on it).
void DrawTabSections(const TabStaff& tab, const std::vector<std::pair<double, std::string>>& sections) {
    const float s = tab.s, fs = 15 * s;
    for (const auto& [t, name] : sections) {
        const float x = tab.TimeX(t);
        const ImVec2 ns = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str());
        if (x < tab.lineL - ns.x - 30 * s || x > tab.lineR + 4 * s) continue;
        const float y = std::floor(tab.TopY() - tab.gap * 0.46f - 2 * s - ns.y);
        const float tx = std::floor(tab.mirror ? x - 14 * s - ns.x : x + 14 * s);
        tab.dl->AddLine(ImVec2(x, tab.TopY() - 14 * s), ImVec2(x, tab.BotY() + 10 * s), Col(theme::kChord, 110), 2 * s);
        tab.dl->AddRectFilled(ImVec2(tx - 4 * s, y - 1 * s), ImVec2(tx + ns.x + 4 * s, y + ns.y + 1 * s), Col(theme::kPanel, 200), 4 * s);
        tab.dl->AddText(g_fontBold, fs, ImVec2(tx, y), Col(theme::kChord, 230), name.c_str());
    }
}

// Rhythm under the staff, like printed tab with rhythm (Guitar Pro, Songsterr): each note gets a stem,
// and the beams say how many notes fit in one beat: none = 1 (quarter note), 1 beam = 2 (eighths),
// 2 = 4 (16ths), 3 = 8 (32nds), 4 = 16; a small "3" = triplets (3 in the time of 2). Half notes get a
// short stem, whole notes none; a dot after the stem = dotted (1.5x as long). A note's value = the time
// to the next note in the song's beats, rounded to the nearest of these. Notes starting in the same
// beat are beamed together; a run ("12 x8") gets its own notes' value.
struct NoteValue { double beats; int beams; bool dot, triplet; };
const NoteValue kNoteValues[] = {
    {4, 0, false, false},       {3, 0, true, false},     {2, 0, false, false},      {1.5, 0, true, false},
    {1, 0, false, false},       {0.75, 1, true, false},  {0.5, 1, false, false},    {1.0 / 3, 1, false, true},
    {0.375, 2, true, false},    {0.25, 2, false, false}, {1.0 / 6, 2, false, true}, {0.125, 3, false, false},
    {1.0 / 12, 3, false, true}, {0.0625, 4, false, false}};

// The note value nearest to q beats (on a log scale: 0.4 is nearer 0.5 than 0.25).
const NoteValue* NearestValue(double q) {
    const NoteValue* best = &kNoteValues[4];
    for (const NoteValue& val : kNoteValues)
        if (std::abs(std::log(q / val.beats)) < std::abs(std::log(q / best->beats))) best = &val;
    return best;
}

// The beat t falls in (index into beats, -1 = before the first one) and that beat's length.
int BeatAt(const std::vector<TabBeat>& beats, double t, double* len) {
    const int i = (int)(std::upper_bound(beats.begin(), beats.end(), t,
                                         [](double x, const TabBeat& b) { return x < b.time; }) - beats.begin()) - 1;
    const int j = std::max(0, std::min(i, (int)beats.size() - 2));
    *len = beats.size() > 1 ? beats[j + 1].time - beats[j].time : 0.5;
    return i;
}

struct Stem { float x, a; const NoteValue* val; int beat; bool beamable; };

// The stem of each item: where, how visible, its note value and the beat it starts in.
std::vector<Stem> TabStems(const TabStaff& tab, const TabShow& sh) {
    const std::vector<TabItem>& items = *sh.items;
    std::vector<Stem> stems(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const TabItem& it = items[i];
        double len = 0.5;
        const int beat = BeatAt(sh.v->tabBeats, it.time, &len);
        // Time to the next note; a run: the gap between its own notes; the last item: unknown (a beat).
        const double d = it.count > 1 ? (it.last - it.time) / (it.count - 1)
                                      : (i + 1 < items.size() ? items[i + 1].time - it.time : len);
        const double q = len > 0 && d > 0 ? d / len : 1;
        stems[i] = {tab.TimeX(it.time), TabAlpha(it, sh), NearestValue(q), beat, it.count == 1};
    }
    return stems;
}

// Beamed together at level k: neighbours in the same beat, both with more than k beams.
bool Joined(const std::vector<Stem>& stems, size_t i, int k) {
    return i + 1 < stems.size() && stems[i].beamable && stems[i + 1].beamable && stems[i].beat >= 0 &&
           stems[i].beat == stems[i + 1].beat && stems[i].val->beams > k && stems[i + 1].val->beams > k;
}

// One stem under the staff (from yTop down): the stem, its dot, beams or stubs, and a triplet's "3".
void DrawStem(const TabStaff& tab, const std::vector<Stem>& stems, size_t i, float yTop) {
    const float s = tab.s, yBot = yTop + tab.stemLen, beamH = 3 * s, beamStep = 5 * s, stub = 8 * s;
    auto ink = [](float a) { return Col(theme::kRhythm, (int)(230 * a)); };
    const Stem& m = stems[i];
    const NoteValue& val = *m.val;
    if (val.beats >= 4 && !val.dot) return;  // whole note: no stem
    const float end = val.beats >= 2 ? yTop + tab.stemLen * 0.5f : yBot;  // half notes: a short stem
    tab.dl->AddLine(ImVec2(m.x, yTop), ImVec2(m.x, end), ink(m.a), 1.5f * s);
    if (val.dot) tab.dl->AddCircleFilled(ImVec2(m.x + 5 * s * tab.dir, end - 4 * s), 1.8f * s, ink(m.a));
    for (int k = 0; k < val.beams; ++k) {
        const float y = yBot - k * beamStep;
        if (Joined(stems, i, k)) {  // a beam to the next note
            const float xn = stems[i + 1].x;
            tab.dl->AddRectFilled(ImVec2(std::min(m.x, xn), y - beamH), ImVec2(std::max(m.x, xn), y),
                                ink(std::min(m.a, stems[i + 1].a)));
        } else if (!(i > 0 && Joined(stems, i - 1, k))) {
            // Not beamed at this level on either side: a short stub, pointing to the note it shares a
            // beam with (or forward in time when it's alone: a flag).
            const bool back = i > 0 && Joined(stems, i - 1, 0);
            const float x2 = back ? m.x - stub * tab.dir : m.x + stub * tab.dir;
            tab.dl->AddRectFilled(ImVec2(std::min(m.x, x2), y - beamH), ImVec2(std::max(m.x, x2), y), ink(m.a));
        }
    }
    // Triplets: a "3" under each beamed group of triplet notes (under a lone one too).
    if (val.triplet && !(i > 0 && Joined(stems, i - 1, 0) && stems[i - 1].val->triplet)) {
        size_t j = i;
        while (Joined(stems, j, 0) && stems[j + 1].val->triplet) ++j;
        const float cx = (m.x + stems[j].x) * 0.5f, fsz = 13 * s;
        const ImVec2 ts = g_fontUi->CalcTextSizeA(fsz, FLT_MAX, 0, "3");
        tab.dl->AddText(g_fontUi, fsz, ImVec2(cx - ts.x * 0.5f, yBot + 2 * s), ink(m.a), "3");
    }
}

void DrawTabRhythm(const TabStaff& tab, const TabShow& sh, float staffBottom) {
    const std::vector<Stem> stems = TabStems(tab, sh);
    for (size_t i = 0; i < stems.size(); ++i) {
        const Stem& m = stems[i];
        if (m.a <= 0 || m.x < tab.lineL - 40 * tab.s || m.x > tab.lineR + 40 * tab.s) continue;
        DrawStem(tab, stems, i, staffBottom + 6 * tab.s);
    }
}

// The item's fret number on string str, in its box at x (alpha a); the next note to play gets a pulsing frame.
void DrawFretBox(const TabStaff& tab, const TabShow& sh, const TabItem& it, int str, float x, float a, bool next) {
    const float s = tab.s, nsz = tab.nsz;
    const std::string label = std::to_string(it.note->frets[str]);
    const std::string run = RunText(it);
    const ImVec2 ls = g_fontBold->CalcTextSizeA(tab.fs, FLT_MAX, 0, label.c_str());
    const float y = tab.RowY(str), bw = tab.BoxHalf(label, run), bh = tab.gap * 0.46f * std::max(0.7f, std::min(1.1f, nsz));
    const ImU32 col = (kStringColor[str] & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);
    tab.dl->AddRectFilled(ImVec2(x - bw, y - bh), ImVec2(x + bw, y + bh), Col(theme::kPanel, (int)(255 * a)), 5 * s);
    if (it.note->mark >= 1 && it.note->mark <= 3) {  // how it went: green = on time, amber = waited for, red = skipped / missed
        static const ImU32 kMarkCol[] = {IM_COL32(60, 175, 90, 0), IM_COL32(225, 150, 40, 0), IM_COL32(215, 60, 50, 0)};
        tab.dl->AddRectFilled(ImVec2(x - bw, y - bh), ImVec2(x + bw, y + bh),
                              kMarkCol[it.note->mark - 1] | ((ImU32)(150 * a) << 24), 5 * s);
    }
    if (next) tab.dl->AddRect(ImVec2(x - bw - 2 * s, y - bh - 2 * s), ImVec2(x + bw + 2 * s, y + bh + 2 * s),
                            Col(theme::kHighlight, (int)(255 * sh.pulse * sh.highlight)), 6 * s, 0, 2.5f * s);
    else tab.dl->AddRect(ImVec2(x - bw, y - bh), ImVec2(x + bw, y + bh), col, 5 * s, 0, 2 * s);
    if (it.note->streak >= 0 && it.note->need > 0) {
        // A trouble spot's note: one dot per good try needed, filled green for each time it was played on
        // time in a row since it went wrong (all filled = cleared).
        const int need = std::min(it.note->need, 10), got = std::min(it.note->streak, need);
        const float z = std::max(0.9f, tab.nsz), r = 3.8f * s * z, step = 10.5f * s * z, dy = y + bh + r + 3 * s;
        const float dx0 = x - (need - 1) * step * 0.5f;
        const float da = std::max(a, 0.6f);
        for (int k = 0; k < need; ++k) {
            const ImVec2 c(dx0 + k * step, dy);
            tab.dl->AddCircleFilled(c, r + 1.2f * s, Col(theme::kPanel, (int)(230 * da)));
            if (k < got) tab.dl->AddCircleFilled(c, r, IM_COL32(70, 200, 100, (int)(255 * da)));
            else tab.dl->AddCircle(c, r - 0.4f * s, Col(theme::kText, (int)(200 * da)), 0, 1.6f * s);
        }
    }
    if (run.empty()) {
        tab.dl->AddText(g_fontBold, tab.fs, ImVec2(x - ls.x * 0.5f, y - ls.y * 0.5f), Col(theme::kText, (int)(255 * a)),
                      label.c_str());
    } else {  // "12" then a label, dimmer "x8"
        const float rs = tab.tiny * 0.85f * nsz;
        const ImVec2 es = g_fontUi->CalcTextSizeA(rs, FLT_MAX, 0, run.c_str());
        const float lx = x - (ls.x + 3 * s * nsz + es.x) * 0.5f;
        tab.dl->AddText(g_fontBold, tab.fs, ImVec2(lx, y - ls.y * 0.5f), Col(theme::kText, (int)(255 * a)), label.c_str());
        tab.dl->AddText(g_fontUi, rs, ImVec2(lx + ls.x + 3 * s * nsz, y - es.y * 0.5f + 1 * s), Col(theme::kTextDim, (int)(230 * a)),
                      run.c_str());
    }
}

// A fret box's technique in the usual tab notation (x = the box's centre, `half` = its half width):
//  - after the box: a slide "/" or "\" (dimmer for an unpitched slide), a bend "^" with its steps ("1", "1/2")
//  - small marks above: h (hammer-on), p (pull-off), T (tap), PM (palm mute), x (muted), > (accent),
//    ~ (vibrato), tr (tremolo), and < > around the number for a harmonic
// The small marks written above a note for these technique bits: "h", "PM", "~"... ("" = none).
std::string TabMarks(uint32_t m) {
    namespace T = technique;
    std::string above;
    auto add = [&](const char* t) { above += (above.empty() ? "" : " ") + std::string(t); };
    if (m & T::kHammerOn) add("h");
    if (m & T::kPullOff) add("p");
    if (m & T::kTap) add("T");
    if (m & T::kPalmMute) add("PM");
    if (m & T::kMute) add("x");
    if (m & T::kAccent) add(">");
    if (m & T::kVibrato) add("~");
    if (m & T::kTremolo) add("tr");
    if (m & T::kPinchHarmonic) add("PH");
    return above;
}

// Draws such marks centred at x, their bottom at y (on a small dark background).
void DrawTabMarks(const TabStaff& tab, const std::string& text, float x, float y, float a) {
    if (text.empty()) return;
    const float s = tab.s, mark = tab.tiny * 0.9f * std::max(0.8f, tab.nsz);
    const ImVec2 ts = g_fontBold->CalcTextSizeA(mark, FLT_MAX, 0, text.c_str());
    const ImVec2 p(x - ts.x * 0.5f, y - ts.y * 0.85f);
    tab.dl->AddRectFilled(ImVec2(p.x - 2 * s, p.y + 1 * s), ImVec2(p.x + ts.x + 2 * s, p.y + ts.y - 1 * s), Col(theme::kPanel, (int)(200 * a)), 3 * s);
    tab.dl->AddText(g_fontBold, mark, p, Col(theme::kText, (int)(235 * a)), text.c_str());
}

// A pick stroke above the staff, on the bar numbers' row, as in printed music: a bracket open at the
// bottom = down stroke, a V = up stroke. The song's own are drawn a little stronger than suggested ones.
void DrawPickMark(const TabStaff& tab, float x, int pick, bool fromSong, float a) {
    const float s = tab.s, hw = 6.5f * s;
    const float cy = tab.TopY() - tab.gap * 0.46f - 2 * s - 10 * s;
    DrawPickSign(tab.dl, ImVec2(x, cy), hw, 2.5f * s, pick, Col(theme::kRhythm, (int)((fromSong ? 235 : 190) * a)));
}

// `aboveMask`: the bits whose marks go above this box (a chord draws the ones all its strings share
// once, above the chord).
void DrawTabTechnique(const TabStaff& tab, const technique::Technique& tq, int fret, int str, float x, float half, float a,
                      uint32_t aboveMask = ~0u) {
    namespace T = technique;
    const uint32_t m = tq.mask;
    if (!m) return;
    const float s = tab.s, y = tab.RowY(str), bh = tab.gap * 0.46f * std::max(0.7f, std::min(1.1f, tab.nsz));
    const float after = x + tab.dir * (half + 2 * s);  // just past the box, in the direction of time
    const ImU32 ink = Col(theme::kText, (int)(235 * a)), dim = Col(theme::kTextDim, (int)(200 * a));
    const float mark = tab.tiny * 0.9f * std::max(0.8f, tab.nsz);

    // After the box: slide / bend.
    const bool slide = (m & T::kSlide) && tq.slideTo >= 0 && tq.slideTo != fret;
    const bool uslide = (m & T::kUnpitchedSlide) && tq.slideUnpitchTo >= 0 && tq.slideUnpitchTo != fret;
    if (slide || uslide) {
        const bool up = (slide ? tq.slideTo : tq.slideUnpitchTo) > fret;
        const float w = 9 * s * std::max(0.8f, tab.nsz), h = bh * 0.8f;
        const float x0 = after, x1 = after + tab.dir * w;
        tab.dl->AddLine(ImVec2(x0, up ? y + h : y - h), ImVec2(x1, up ? y - h : y + h), slide ? ink : dim, 2.4f * s);
    }
    if ((m & T::kBend) && tq.bend > 0.1f) {
        const float ax = after + tab.dir * 5 * s, top = y - bh - 4 * s;
        tab.dl->AddLine(ImVec2(ax, y - 2 * s), ImVec2(ax, top + 4 * s), ink, 2 * s);
        tab.dl->AddTriangleFilled(ImVec2(ax, top - 2 * s), ImVec2(ax - 4 * s, top + 5 * s), ImVec2(ax + 4 * s, top + 5 * s), ink);
        const std::string t = T::BendLabel(tq.bend);
        const ImVec2 ts = g_fontBold->CalcTextSizeA(mark, FLT_MAX, 0, t.c_str());
        const float tx = tab.dir > 0 ? ax + 3 * s : ax - 3 * s - ts.x;
        tab.dl->AddText(g_fontBold, mark, ImVec2(tx, top - ts.y * 0.6f), ink, t.c_str());
    }
    // Harmonic: < > around the number.
    if (m & (T::kHarmonic | T::kPinchHarmonic)) {
        const float hx = half + 3 * s, hh = bh * 0.7f, hw = 5 * s;
        for (float side : {-1.0f, 1.0f}) {
            const float ex = x + side * hx;
            tab.dl->AddLine(ImVec2(ex + side * hw, y - hh), ImVec2(ex, y), ink, 2 * s);
            tab.dl->AddLine(ImVec2(ex, y), ImVec2(ex + side * hw, y + hh), ink, 2 * s);
        }
    }
    // Small marks above the box.
    DrawTabMarks(tab, TabMarks(m & aboveMask), x, y - bh, a);
}

// One item: the chord's bracket and name, tails on held notes, and its fret boxes.
void DrawTabItem(const TabStaff& tab, TabShow& sh, const TabItem& it) {
    const float s = tab.s;
    const TabNote& t = *it.note;
    const float x = tab.TimeX(it.time), xEnd = tab.TimeX(it.end);  // (xEnd < x when left-handed)
    if (std::max(x, xEnd) < tab.lineL - 30 * s || std::min(x, xEnd) > tab.lineR + 30 * s) return;
    const float a = TabAlpha(it, sh);
    if (a <= 0) return;
    const bool next = !sh.nextFound && !t.ignore && it.last >= sh.nextFrom;  // a run stays "next" until its last note
    if (next) sh.nextFound = true;

    int lo = -1, hi = -1;  // lowest/highest string played (for the chord bracket)
    for (int str = 0; str < tab.n; ++str)
        if (t.frets[str] >= 0) { if (lo < 0) lo = str; hi = str; }
    if (lo < 0) return;
    if (t.chord && hi > lo)
        tab.dl->AddLine(ImVec2(x, tab.RowY(hi)), ImVec2(x, tab.RowY(lo)), Col(theme::kChord, (int)(170 * a)), 2 * s);
    if (t.chord && !t.name.empty()) {
        const ImVec2 ts = g_fontBold->CalcTextSizeA(tab.tiny, FLT_MAX, 0, t.name.c_str());
        tab.dl->AddText(g_fontBold, tab.tiny, ImVec2(x - ts.x * 0.5f, tab.staffY + 6 * s),
                      (Col(theme::kChord) & 0x00FFFFFF) | ((ImU32)(255 * a) << 24), t.name.c_str());
    }
    // Held notes (and runs): a tail in the string's colour until the note ends (like the highway's tails).
    if (std::abs(xEnd - x) > it.half * tab.nsz + 4 * s)
        for (int str = 0; str < tab.n; ++str)
            if (t.frets[str] >= 0)
                tab.dl->AddRectFilled(ImVec2(std::min(x, xEnd), tab.RowY(str) - 3 * s), ImVec2(std::max(x, xEnd), tab.RowY(str) + 3 * s),
                                    (kStringColor[str] & 0x00FFFFFF) | ((ImU32)(150 * a) << 24), 3 * s);
    for (int str = 0; str < tab.n; ++str)
        if (t.frets[str] >= 0) DrawFretBox(tab, sh, it, str, x, a, next);
    // Techniques: what every string of a chord shares (palm mute, mute, accent...) is marked once above
    // the chord's top box; the rest next to each string's box.
    uint32_t shared = ~0u;
    int topStr = -1;
    for (int str = 0; str < tab.n; ++str) {
        if (t.frets[str] < 0) continue;
        shared &= t.tech[str].mask;
        if (topStr < 0 || tab.RowY(str) < tab.RowY(topStr)) topStr = str;
    }
    if (!t.chord) shared = 0;
    for (int str = 0; str < tab.n; ++str)
        if (t.frets[str] >= 0 && t.tech[str].mask)
            DrawTabTechnique(tab, t.tech[str], t.frets[str], str, x, tab.BoxHalf(std::to_string(t.frets[str]), RunText(it)), a, ~shared);
    if (shared && topStr >= 0)
        DrawTabMarks(tab, TabMarks(shared), x, tab.RowY(topStr) - tab.gap * 0.46f * std::max(0.7f, std::min(1.1f, tab.nsz)), a);
    if (sh.st->tabPicks && t.pick >= 0 && it.count == 1) DrawPickMark(tab, x, t.pick, t.pickFromSong, a);
}

// Held chord shapes: a thin gold bracket over their notes in the chord names' lane (they're picked one by
// one with the chord kept pressed), from the first note to where the shape ends, with the chord's name at
// its start when it has one. Shapes that follow each other with the same chord name make one bracket (a
// song often repeats the shape every bar: a name every few notes was clutter); nameless ones stay apart
// (they may be different shapes).
void DrawTabShapes(const TabStaff& tab, const std::vector<TabItem>& items) {
    struct Span {
        double from, to;  // the first note, the end of the last shape
        std::string name;
    };
    std::vector<Span> spans;
    double key = -1;  // the shape of the last note seen (its start)
    for (const TabItem& it : items) {
        const TabNote& t = *it.note;
        if (t.shapeEnd < 0 || t.shapeStart == key) continue;
        key = t.shapeStart;
        if (!spans.empty() && !t.shapeName.empty() && spans.back().name == t.shapeName && t.shapeStart <= spans.back().to + 0.05)
            spans.back().to = std::max(spans.back().to, t.shapeEnd);
        else
            spans.push_back({it.time, t.shapeEnd, t.shapeName});
    }
    const float s = tab.s, y = tab.staffY + 6 * s;
    const ImU32 gold = Col(theme::kChord, 200);
    for (const Span& sp : spans) {
        const float x0 = tab.TimeX(sp.from), x1 = tab.TimeX(sp.to);
        if (std::max(x0, x1) < tab.lineL - 20 * s || std::min(x0, x1) > tab.lineR + 20 * s) continue;
        const float dir = x1 >= x0 ? 1.0f : -1.0f;
        const ImVec2 ns = sp.name.empty() ? ImVec2(0, tab.tiny) : g_fontBold->CalcTextSizeA(tab.tiny, FLT_MAX, 0, sp.name.c_str());
        const float ly = y + ns.y * 0.55f;
        float a = x0;
        if (!sp.name.empty()) {
            tab.dl->AddText(g_fontBold, tab.tiny, ImVec2(std::floor(x0 - ns.x * 0.5f), y), gold, sp.name.c_str());
            a = x0 + dir * (ns.x * 0.5f + 5 * s);
        } else {
            tab.dl->AddLine(ImVec2(x0, ly), ImVec2(x0, ly + 6 * s), gold, 1.5f * s);  // the bracket's start
        }
        if ((x1 - a) * dir > 4 * s) {
            tab.dl->AddLine(ImVec2(a, ly), ImVec2(x1, ly), gold, 1.5f * s);
            tab.dl->AddLine(ImVec2(x1, ly), ImVec2(x1, ly + 6 * s), gold, 1.5f * s);  // and its end
        }
    }
}

// One staff: the strings, the beat grid, the cursor, the rhythm lane and the notes, as set up by
// TabStaff::Layout(). cursor: 2 = the cursor ("now" line), 1 = its dimmed copy (a row still to come
// whose repeated part is where the cursor is right now), 0 = none.
void DrawTabStaff(const TabStaff& tab, TabShow& sh, int cursor) {
    const float s = tab.s;
    DrawTabStrings(tab);
    const float cursorX = tab.TimeX(sh.cursorT);  // the "now" line (fixed while scrolling, moving on pages)
    const float staffTop = tab.TopY() - 8 * s, staffBottom = tab.BotY() + 8 * s;
    tab.dl->PushClipRect(ImVec2(tab.lineL - 4 * s, tab.staffY), ImVec2(tab.lineR + 4 * s, tab.staffY + tab.rowH), true);
    // (The beats always come with the View, for the rhythm below; the lines are the tabBeats setting.)
    static const std::vector<TabBeat> kNoBeats;
    DrawTabGrid(tab, sh.st->tabBeats ? sh.v->tabBeats : kNoBeats, staffTop, staffBottom);
    DrawTabSections(tab, sh.v->sections);
    // The practice parts (the practice bar), lightly shaded.
    for (const auto& p : g_drawRanges) {
        const float xa = tab.TimeX(p.first), xb = tab.TimeX(p.second);
        tab.dl->AddRectFilled(ImVec2(std::min(xa, xb), tab.TopY() - 12 * s), ImVec2(std::max(xa, xb), tab.BotY() + 10 * s),
                              Col(theme::kChord, 28), 3 * s);
    }
    // The "now" line: where the highway's notes reach the fretboard.
    if (cursor == 2)
        tab.dl->AddLine(ImVec2(cursorX, tab.TopY() - 16 * s), ImVec2(cursorX, tab.BotY() + 12 * s), Col(theme::kHighlight, 200), 2.5f * s);
    else if (cursor == 1)
        tab.dl->AddLine(ImVec2(cursorX, tab.TopY() - 16 * s), ImVec2(cursorX, tab.BotY() + 12 * s), Col(theme::kHighlight, 80), 2 * s);
    if (sh.rhythm && !sh.items->empty()) DrawTabRhythm(tab, sh, staffBottom);
    DrawTabShapes(tab, *sh.items);
    for (const TabItem& it : *sh.items) DrawTabItem(tab, sh, it);
    tab.dl->PopClipRect();
}

// The scrolling tab: guitar tab of the next few seconds, thinnest string on top. Notes move right to
// left at a constant speed (so the spacing shows the rhythm) and cross the "now" line when they
// reach the highway's fretboard. The next note to play is highlighted; played ones fade out. Under
// the notes, the song's beat grid: bar lines with bar numbers, faint beat lines, every other bar
// shaded; held notes get a tail. With these, the gaps between notes can be read as rhythm.
// Fast passages (setting tabSpread): the whole tab zooms in smoothly so the notes coming up are far
// enough apart to read, and a fast repeat of one fret is drawn once, "12 x8".
// Left-handed (tabMirror) everything runs the other way (notes move left to right, pages turn to the
// left); tabThickTop puts the thickest string on top.
void DrawTab(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    TabStaff tab;
    tab.dl = dl;
    tab.s = S * st.tabSize / 100.0f;  // the tab's own size: string gap, text
    const float s = tab.s;
    tab.n = v.bass ? 4 : 6;
    // top: a lane for chord names (y0 + 6), then the bar numbers right above the strings.
    // bottom: room under the low string, plus the rhythm lane (stems, beams, triplet "3") when shown.
    const bool rhythm = st.tabRhythm && !v.tabBeats.empty();
    tab.stemLen = 24 * s;
    tab.gap = 28 * s;
    tab.top = 60 * s;
    const float w = std::min(ds.x, st.tabWidth * S), labelW = 26 * s, pad = 12 * s;
    const float bottom = rhythm ? 18 * s + tab.stemLen + 18 * s : 18 * s;
    // Rows (pages only, setting tabRows): the box holds 1..4 staffs, one under the other.
    const int rows = st.tabPage ? std::max(1, std::min(kMaxRows, st.tabRows)) : 1;
    tab.rowH = tab.top + tab.gap * (tab.n - 1) + bottom;
    const float h = tab.rowH * rows;
    // Position from the settings, kept on screen.
    const ImVec2 p0 = Place(ds.x * 0.5f + st.tabX * S, st.tabY * S, w, h, ds);
    const float x0 = p0.x, y0 = p0.y;
    g_box[kTab] = {p0, ImVec2(x0 + w, y0 + h), true};
    // Background: how solid is the player's choice (100 % hides the game's own text behind the tab).
    const int bgA = std::max(0, std::min(100, st.tabOpacity)) * 255 / 100;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), Col(theme::kPanel, bgA), 10 * s);

    // Left-handed: the string names go on the right and time runs right to left (TabStaff::TimeX).
    tab.mirror = st.tabMirror;
    tab.dir = tab.mirror ? -1.0f : 1.0f;
    tab.thickTop = st.tabThickTop;
    tab.lineL = x0 + pad + (tab.mirror ? 0 : labelW);
    tab.lineR = x0 + w - pad - (tab.mirror ? labelW : 0);
    tab.labelX = tab.mirror ? tab.lineR + 10 * s : x0 + pad;
    // Where "now" sits: scrolling = the fixed line, a little of the past visible on its left (kCursorStart).
    // Pages: a new page first repeats the end of the previous one (recap, the player's setting), and the
    // cursor starts right after it. The speed (pixels per second) doesn't depend on the recap, so the
    // spacing of the notes stays the same whatever is chosen.
    constexpr float kCursorStart = 0.08f;  // of the width, from the left edge (+ a small margin)
    const float recap = st.tabPage ? std::max(0, std::min(50, st.tabRecap)) / 100.0f : kCursorStart;
    const float pageL = tab.lineL + 6 * s;
    const float nowX = pageL + (tab.lineR - pageL) * kCursorStart;
    const double secs = std::max(1, st.tabSeconds);
    tab.pxPerS = (float)((tab.lineR - nowX) / secs);
    // The fret numbers start at the player's note size (menu); fast passages shrink them a little below
    // it (TabShrink); the items' widths are measured at the player's size.
    tab.baseNote = std::max(60, std::min(130, st.tabNoteSize)) / 100.0f;
    tab.nsz = tab.baseNote;
    tab.tiny = 19 * s;
    tab.fs = 21 * s * tab.nsz;

    const double now = v.songTime;
    // The next note to play (highlighted, and where a run's "x8" counts from): while the song waits,
    // exactly the note it waits for (the clock stops a few ms past it, sometimes more than 20 ms, which
    // made the note AFTER it look "next"); while playing, the first one not yet past.
    // holdT: the note the song waits for, or will stop at next if it isn't played (-1 = none).
    const double holdT = v.waiting && v.waitTime >= 0 ? v.waitTime : v.nextWaitTime;
    const double nextFrom = holdT >= 0 && now > holdT - 0.02 ? holdT - 0.002 : now - 0.02;
    const double cursorT = TabCursor(now, holdT);
    const std::vector<TabItem> items = TabItems(v, tab, st.tabSpread, nextFrom);

    // The zoom target = the most any gap from 0.5 s ago to the end of the tab needs.
    const double target = ZoomNeed(items, tab, st.tabSpread, now - 0.5, now + secs);
    const double frameS = std::min(0.1, (double)ImGui::GetIO().DeltaTime);
    TabPages pages;
    if (rows > 1) pages = RowPages(&tab, items, st.tabSpread, now, pageL, recap, rows);
    else if (!st.tabPage) pages = ScrollingPage(&tab, cursorT, nowX, target, frameS);
    else pages = TurningPage(&tab, now, pageL, recap, target, frameS);

    TabShow sh;
    sh.v = &v;
    sh.st = &st;
    sh.items = &items;
    sh.rhythm = rhythm;
    sh.now = now;
    sh.cursorT = cursorT;
    sh.nextFrom = nextFrom;
    sh.pulse = 0.6f + 0.4f * std::sin((float)ImGui::GetTime() * 5.0f);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), true);
    // The cursor's page first (the next note is highlighted there if it's on two rows), then the pages
    // still to come, each with its recap dimmed; a thin line between the rows.
    for (int k = 0; k < pages.count; ++k) {
        tab.Layout(pages.need[k], pages.t[k], y0 + pages.row[k] * tab.rowH);
        sh.dimBefore = pages.recapEnd[k];
        // A row still to come whose repeated start covers where the cursor is: a dimmed copy of the
        // cursor there too, so the eye can already move down before the cursor jumps.
        const bool ghost = k > 0 && cursorT >= pages.t[k] && cursorT < pages.recapEnd[k];
        if (ghost) {  // the next note gets a faint highlight there too
            const bool found = sh.nextFound;
            sh.nextFound = false;
            sh.highlight = 0.35f;
            DrawTabStaff(tab, sh, 1);
            sh.nextFound = found;
            sh.highlight = 1.0f;
        } else {
            DrawTabStaff(tab, sh, k == 0 ? 2 : 0);
        }
    }
    for (int r = 1; r < rows; ++r)
        dl->AddLine(ImVec2(x0 + pad, y0 + r * tab.rowH), ImVec2(x0 + w - pad, y0 + r * tab.rowH), Col(theme::kGrid, 40), 1 * s);
    dl->PopClipRect();
}

// A short message in the middle of the screen, with opacity a (the caller fades it: a message repeated
// quickly, like "Skipped" at every F9 in a fast passage, just stays up instead of blinking).
void DrawToast(ImDrawList* dl, const std::string& text, float a, float s, ImVec2 ds) {
    if (text.empty() || a <= 0) return;
    const float size = 30 * s, pad = 16 * s;
    const ImVec2 ts = g_fontBold->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const ImVec2 p0(std::floor((ds.x - ts.x) * 0.5f - pad), std::floor(ds.y * 0.34f));
    const ImVec2 p1(p0.x + ts.x + 2 * pad, p0.y + ts.y + pad);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, (int)(215 * a)), 10 * s);
    dl->AddText(g_fontBold, size, ImVec2(p0.x + pad, p0.y + pad * 0.5f), Col(theme::kText, (int)(255 * a)), text.c_str());
}

// The part under the mouse (last frame's boxes, the one drawn on top first), -1 = none. *grip =
// the mouse is on its bottom-right corner (resize).
int HitPart(ImVec2 m, float S, bool* grip) {
    const float margin = 4 * S, corner = 26 * S;
    for (int p = kParts - 1; p >= 0; --p) {
        const Box& b = g_box[p];
        if (!b.drawn || m.x < b.p0.x - margin || m.y < b.p0.y - margin || m.x > b.p1.x + margin || m.y > b.p1.y + margin)
            continue;
        *grip = m.x > b.p1.x - corner && m.y > b.p1.y - corner;
        return p;
    }
    return -1;
}

// While the menu is open, the mouse arranges the screen: dragging a part moves it, dragging its
// bottom-right corner resizes it. Works on the boxes drawn in the previous frame. During a drag
// *lay (what this frame draws) follows the mouse; on release the new layout goes into the shared
// settings (main.cpp then saves it to the ini). S = screen scale.
void Arrange(bool menu, Settings* lay, float S, ImVec2 ds) {
    ImGuiIO& io = ImGui::GetIO();
    if (g_drag.part < 0) {
        if (!menu || io.WantCaptureMouse) return;  // over the menu window: the click is the menu's
        bool grip = false;
        const int hot = HitPart(io.MousePos, S, &grip);
        if (hot < 0) return;
        ImGui::SetMouseCursor(grip ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeAll);
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        g_drag.part = hot;
        g_drag.resize = grip;
        g_drag.mouse0 = io.MousePos;
        g_drag.box0 = g_box[hot];
        g_drag.st0 = *lay;
    }

    const float dx = io.MousePos.x - g_drag.mouse0.x, dy = io.MousePos.y - g_drag.mouse0.y;
    const Box& b = g_drag.box0;
    const float w0 = b.p1.x - b.p0.x, h0 = b.p1.y - b.p0.y;
    auto size = [](float pct) { return (int)std::lround(std::max(50.0f, std::min(250.0f, pct))); };
    Settings e = g_drag.st0;
    if (!g_drag.resize) {
        // Move: the box's new corner, kept on screen, turned back into the settings' coordinates.
        const ImVec2 p = Place(b.p0.x + dx, b.p0.y + dy, w0, h0, ds);
        if (g_drag.part == kBanner) {
            e.bannerX = (int)std::lround((p.x + w0 * 0.5f - ds.x * 0.5f) / S);
            e.bannerY = (int)std::lround(p.y / S);
        } else if (g_drag.part == kMistake) {
            // An offset from its place on the banner's right (it follows the banner); never exactly
            // 0, 0 after a drag, which means "beside it, wherever there's room".
            const Box& bb = g_box[kBanner];
            e.mistakeX = (int)std::lround((p.x - (bb.p1.x + 16 * S)) / S);
            e.mistakeY = (int)std::lround((p.y - bb.p0.y) / S);
            if (e.mistakeX == 0 && e.mistakeY == 0) e.mistakeY = 1;
        } else if (g_drag.part == kClock) {
            e.clockX = (int)std::lround(p.x / S);
            e.clockY = (int)std::lround(p.y / S);
        } else {
            e.tabX = (int)std::lround((p.x - ds.x * 0.5f) / S);
            e.tabY = (int)std::lround(p.y / S);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else {
        // Resize: the banner and the clock size themselves from their text, so the corner scales them
        // (the average of the width and height change; the banner grows on both sides of its
        // centre). The tab's width and height change separately: wider = more room between notes.
        const float ry = (h0 + dy) / h0;
        if (g_drag.part == kBanner) {
            e.bannerSize = size(e.bannerSize * ((w0 + 2 * dx) / w0 + ry) * 0.5f);
        } else if (g_drag.part == kMistake) {
            e.mistakeSize = size(e.mistakeSize * ((w0 + dx) / w0 + ry) * 0.5f);
        } else if (g_drag.part == kClock) {
            e.clockSize = size(e.clockSize * ((w0 + dx) / w0 + ry) * 0.5f);
        } else {
            e.tabWidth = (int)std::lround(std::max(250.0f, std::min(ds.x / S, (w0 + dx) / S)));
            e.tabSize = size(e.tabSize * ry);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    }
    *lay = e;

    if (!menu || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // released (or the menu closed): keep it
        {
            std::lock_guard<std::mutex> lk(g.m);
            CopyLayout(e, &g.settings);
        }
        Log("overlay: %s %s", kPartName[g_drag.part], g_drag.resize ? "resized" : "moved");
        g_drag.part = -1;
    }
}

// While the menu is open: an outline, a name and a corner grip on each part, so it's clear they can
// be dragged. The one under the mouse (or being dragged) is brighter.
void DrawArrangeHints(ImDrawList* dl, float S) {
    const ImGuiIO& io = ImGui::GetIO();
    bool grip = false;
    const int hot = g_drag.part >= 0 ? g_drag.part : (io.WantCaptureMouse ? -1 : HitPart(io.MousePos, S, &grip));
    for (int p = 0; p < kParts; ++p) {
        const Box& b = g_box[p];
        if (!b.drawn) continue;
        const ImU32 c = hot == p ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 110);
        const float m = 3 * S, corner = 20 * S;  // margin around the part, size of the resize corner
        const ImVec2 q0(b.p0.x - m, b.p0.y - m), q1(b.p1.x + m, b.p1.y + m);
        dl->AddRect(q0, q1, c, 8 * S, 0, (hot == p ? 2.5f : 1.5f) * S);
        dl->AddTriangleFilled(ImVec2(q1.x, q1.y - corner), q1, ImVec2(q1.x - corner, q1.y), c);
        // The name on a small tag above the top-left corner (below the part at the top of the screen).
        const ImVec2 ts = g_fontBold->CalcTextSizeA(17 * S, FLT_MAX, 0, kPartName[p]);
        const float ty = q0.y - ts.y - 6 * S >= 0 ? q0.y - ts.y - 6 * S : q1.y + 6 * S;
        dl->AddRectFilled(ImVec2(q0.x, ty - 2 * S), ImVec2(q0.x + ts.x + 12 * S, ty + ts.y + 2 * S), Col(theme::kPanel, 210), 4 * S);
        dl->AddText(g_fontBold, 17 * S, ImVec2(q0.x + 6 * S, ty), c, kPartName[p]);
    }
}

// ------------------------------------------------------------------ the menu
// Layout: a header that is always there (the mode on/off, which chart is used, Skip), then tabs, one
// per topic, each with short titled sections; the longer explanations are in "(?)" tooltips so the
// options stay easy to scan. Every page has the same height (the window doesn't jump when switching
// tabs). Footer: saved automatically + Close.

// A "(?)" after the previous item, showing text in a tooltip.
void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Checkbox + optional "(?)".
bool Check(const char* label, bool* v, const char* help = nullptr) {
    const bool changed = ImGui::Checkbox(label, v);
    if (help) Help(help);
    return changed;
}

// A slider with its label in a left column (labels and sliders line up), and a "(?)" at the end.
void SliderRow(const char* label, const char* id, int* v, int lo, int hi, const char* fmt, const char* help) {
    const float labelW = ImGui::GetFontSize() * 9.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 1.8f);  // room for the "(?)"
    ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    Help(help);
}

// The menu's own colours, from the theme's Menu colour (the accent: title bar, tabs, checkmarks,
// sliders, buttons); the rest stays a neutral dark so every theme keeps the menu readable.
void ApplyMenuStyle() {
    ImVec4* c = ImGui::GetStyle().Colors;
    const ImVec4 a = ImGui::ColorConvertU32ToFloat4(Col(theme::kMenu));
    auto mix = [&](float k, float alpha) {  // k < 1: darker; k > 1: towards white
        const float t = k > 1 ? k - 1 : 0, d = k > 1 ? 1 : k;
        return ImVec4(a.x * d + (1 - a.x) * t, a.y * d + (1 - a.y) * t, a.z * d + (1 - a.z) * t, alpha);
    };
    c[ImGuiCol_TitleBgActive] = mix(1.0f, 1.0f);
    c[ImGuiCol_TitleBg] = mix(0.6f, 1.0f);
    c[ImGuiCol_Tab] = mix(0.55f, 0.85f);
    c[ImGuiCol_TabHovered] = mix(1.35f, 1.0f);
    c[ImGuiCol_TabSelected] = mix(1.0f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = mix(1.5f, 1.0f);
    c[ImGuiCol_TabDimmed] = mix(0.45f, 0.85f);
    c[ImGuiCol_TabDimmedSelected] = mix(0.8f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);   // white tick on the accent colour
    c[ImGuiCol_CheckboxSelectedBg] = mix(0.9f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.985f);  // (almost) solid: the game's menus don't show through
    c[ImGuiCol_SliderGrab] = mix(1.35f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = mix(1.6f, 1.0f);
    c[ImGuiCol_Button] = mix(0.85f, 0.75f);
    c[ImGuiCol_ButtonHovered] = mix(1.15f, 1.0f);
    c[ImGuiCol_ButtonActive] = mix(1.4f, 1.0f);
    c[ImGuiCol_Header] = mix(0.9f, 0.55f);
    c[ImGuiCol_HeaderHovered] = mix(1.1f, 0.8f);
    c[ImGuiCol_HeaderActive] = mix(1.25f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = mix(1.0f, 0.5f);
    c[ImGuiCol_NavCursor] = mix(1.6f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.32f, 0.32f, 0.38f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.30f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.98f);
}

// Page "Playing": when the song waits, and how strict the listening is.
void MenuPlaying(Settings& e, const View& v, int* calibration) {
    ImGui::SeparatorText("Waiting");
    ImGui::BeginDisabled(!e.enabled);
    Check("Wait for chords too", &e.waitChords, "Off: the song only waits for single notes; chords pass by themselves.");
    ImGui::EndDisabled();
    Check("Don't wait again for greyed-out notes", &e.skipGreyed,
          "When you resume from the game's pause screen, the song goes back a few seconds and replays them with the "
          "notes you already passed greyed out. On: the song doesn't stop for those again (the tab dims them too).");
    Check("Accept the same note an octave higher or lower", &e.acceptOctaves,
          "Useful if you play a riff in another position. Off is stricter: string and fret must match.");
    Check("Tell me when my guitar sounds out of tune", &e.tuningCheck,
          "Compares the notes you play with the song's. When a string (or the whole guitar) keeps sounding low or "
          "high, a message says which one and which way to turn it, and a wrong note it caused says so.");
    ImGui::SeparatorText("Timing");
    ImGui::BeginDisabled(!e.stopSong);  // (only for stops)
    SliderRow("Early notes count", "##early", &e.earlyMs, 0, 1000, "up to %d ms",
              "A right note played up to this early counts, and the song doesn't stop for it.");
    SliderRow("Late notes count", "##late", &e.lateMs, 0, 400, e.lateMs ? "up to %d ms" : "off",
              "A right note played up to this late counts, and the song doesn't stop for it: the song stops only this "
              "long after the note, if you haven't played it by then. A note played right on the beat is heard a "
              "little after its time, so without this the song stops for a moment at every note. 0 = off: the song "
              "stops just before the note (below).");
    ImGui::BeginDisabled(e.lateMs > 0);  // (only without a late window)
    SliderRow("Stop before the note", "##lead", &e.leadMs, 0, 500, "%d ms",
              "With \"Late notes count\" off: the song stops this long before the note reaches the line, to give you "
              "time to see it. Keep at least a little (30 ms): stopped right on the note, the game counts it as "
              "already passed.");
    ImGui::EndDisabled();
    SliderRow("Count-in after a wait", "##countin", &e.countInBeats, 0, 4, e.countInBeats ? "%d beats" : "off",
              "After a long wait (over 2 seconds) ends with the right note, the song counts this many beats at its own "
              "tempo (3, 2, 1 on screen) before it goes on, so you find the beat again. 0 = off.");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Wrong notes");
    Check("Show where you really played it", &e.stringDetect,
          "The same note exists on several strings. After a wrong note, Note-by-Note listens to its sound (a thicker "
          "string sounds a little different) and shows only the spot you played, with advice for that spot. It needs "
          "a calibration, once per guitar: press Calibrate and pluck each open string 3 times. Works best with a "
          "clean sound (no compressor, drive or EQ preset before the game). When it isn't sure, the banner shows its "
          "guess and the other places with the same note, faded.");
    ImGui::BeginDisabled(!e.stringDetect);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(v.stringIdWarn ? ImVec4(1.0f, 0.65f, 0.25f, 1) : ImVec4(0.75f, 0.75f, 0.8f, 1), "%s",
                       v.stringIdStatus.c_str());
    ImGui::PopTextWrapPos();
    if (v.calibrating) {
        if (ImGui::Button("Cancel calibration")) *calibration = 2;
    } else if (ImGui::Button(v.calibrated ? "Calibrate again" : "Calibrate")) {
        *calibration = 1;
    }
    ImGui::EndDisabled();
}

// Page "Tab": everything about the scrolling tab.
void MenuTab(Settings& e) {
    Check("Show the notes coming up as tab", &e.showTab);
    ImGui::BeginDisabled(!e.showTab);
    ImGui::SeparatorText("Movement");
    if (ImGui::RadioButton("Pages", e.tabPage)) e.tabPage = true;
    Help("The notes stand still and a cursor moves over them; the page turns near the right edge. Easy to read fast parts.");
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    if (ImGui::RadioButton("Scrolling", !e.tabPage)) e.tabPage = false;
    Help("The notes move to a fixed line, like the game's highway.");
    ImGui::BeginDisabled(!e.tabPage);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Rows");
    for (int r = 1; r <= 4; ++r) {
        ImGui::SameLine();
        char label[8];
        std::snprintf(label, sizeof(label), "%d##rows", r);
        if (ImGui::RadioButton(label, e.tabRows == r)) e.tabRows = r;
    }
    Help("Pages only: with 2 to 4 rows, the next pages already wait in the rows below, so there's no page turn to wait for. "
         "More rows = a taller tab (drag its corner to resize it).");
    ImGui::EndDisabled();
    SliderRow("Seconds ahead", "##tabsec", &e.tabSeconds, 2, 8, "%d s", "How much music the tab shows ahead of the cursor.");
    ImGui::BeginDisabled(!e.tabPage);
    SliderRow("Repeat previous page", "##tabrecap", &e.tabRecap, 0, 50, "%d %%",
              "Pages only: how much of the end of the previous page a new page shows again on its left (percent of the width), "
              "so you can see where you came from. The cursor starts right after it. On the rows still to come it's drawn dimmed.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Reading");
    Check("Bar and beat lines", &e.tabBeats, "Bar lines with bar numbers, and faint lines on the beats.");
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    Check("Rhythm under the tab", &e.tabRhythm,
          "A stem per note; beams say how many notes fit in one beat: none = 1, one beam = 2, two = 4, three = 8. A small 3 = triplets.");
    Check("Spread out fast notes", &e.tabSpread,
          "Fast passages get more room so every fret can be read, and a fast repeat of one fret shows once as \"12 x8\". "
          "Off: spacing exactly by time.");
    Check("Show which way to pick (down / up)", &e.tabPicks,
          "Above each picked note on the tab: a bracket = a down stroke, a V = an up stroke (the usual signs in printed "
          "music); the banner shows the same sign after the note. From the song when it says (few songs "
          "do); otherwise suggested from the rhythm, as alternate picking is taught: down on the beat, up in between "
          "(with 16th notes, down on the beat and on the \"and\"). Hammer-ons, pull-offs and taps get none.");
    Check("Colour the notes you played", &e.tabMarks,
          "Once the song has passed a note, its box on the tab gets a colour: green = you played it on time, amber = the "
          "song waited for it, red = skipped, or (in \"Show the notes\") not played.");

    ImGui::SeparatorText("Strings");
    Check("Thickest string on top", &e.tabThickTop,
          "Off: thinnest string on top, like printed tab. On: thickest on top (for example if you play a flipped guitar). "
          "The banner's small tab follows.");
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    Check("Left-handed (right to left)", &e.tabMirror,
          "Time runs from right to left and the string names move to the right. The banner's small tab follows.");
    Check("Number the strings from the thickest", &e.stringsFromThick,
          "The banner and its advice name strings by number, \"string 4 (D)\", in the string's colour. Off: the "
          "standard numbering of guitar books, string 1 is the thinnest (high e). On: string 1 is the thickest (low E).");
    // Back to the way tab is written in books and on tab sites (the left-handed option is about the
    // player's hands, not about the notation, so it stays as it is).
    if (ImGui::Button("Standard layout (like printed tab)")) {
        e.tabThickTop = false;
        e.stringsFromThick = false;
    }
    Help("Thinnest string on top and string 1 = the thinnest (high e), as in guitar books and on tab sites. "
         "The left-handed option is kept.");

    ImGui::SeparatorText("Look");
    SliderRow("Note size", "##tabnote", &e.tabNoteSize, 60, 130, "%d %%",
              "Size of the fret numbers. Fast passages make them a little smaller by themselves.");
    SliderRow("Background", "##tabbg", &e.tabOpacity, 0, 100, "%d %%",
              "0 = see-through, 100 = solid (hides the game's own text behind the tab).");
    ImGui::EndDisabled();
}

// Page "Screen": what else is shown, and where.
void MenuScreen(Settings& e) {
    ImGui::SeparatorText("Show");
    Check("What to play while the song waits", &e.showBanner,
          "The banner: string, fret and a small tab (for chords: name and shape), and how to fix a wrong note.");
    ImGui::BeginDisabled(!e.showBanner);
    ImGui::Indent();
    Check("Show it on a fretboard", &e.bannerNeck,
          "The banner's picture is a piece of the neck: the note is a dot in its string's colour with the fret "
          "number, and after a wrong note a red X shows where you probably played it. Off = a small tab.");
    Check("Fingers and hand position", &e.bannerHand,
          "On the fretboard: which finger for each note (1 = index .. 4 = little finger), and the frets your hand "
          "covers, shaded. When the hand has to move, the banner says where: \"Hand: move UP to fret 7\".");
    ImGui::BeginDisabled(!e.bannerNeck || !e.bannerHand);
    ImGui::Indent();
    Check("Draw the hand under the fretboard", &e.bannerFingers,
          "Four fingers under the fretboard, over the frets they cover; the ones playing now in the string's colour. "
          "Off: only the finger numbers on the dots (the banner gets a little shorter).");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::Unindent();
    ImGui::EndDisabled();
    Check("Song time", &e.showClock, "A small clock, \"1:23 / 4:28\", top-left by default.");
    Check("Practice bar", &e.showPracticeBar,
          "On the game's progress bar, with the mouse (no menu needed): click a phrase or drag over a part of the song; "
          "Note-by-Note only waits inside the parts you mark, and the tab shades them. Drag a part's end to change it, "
          "click a part to remove it. Red phrases: where the song waited for you most (Practice page).");
    ImGui::SeparatorText("Arrange");
    ImGui::TextWrapped("While this menu is open, drag the banner, the wrong-note panel, the clock or the tab to move "
                       "it, and drag its bottom-right corner to resize it. The wrong-note panel follows the banner. "
                       "The menu itself moves by its title bar.");
    ImGui::Spacing();
    if (ImGui::Button("Reset positions and sizes")) e = WithDefaultLayout(e);
}

// The practice parts, set from the menu (render thread, like the practice bar's drags).
void SetPracticeParts(std::vector<Range> parts) {
    Tidy(&parts);
    g_drawRanges = parts;
    std::lock_guard<std::mutex> lk(g.m);
    g.ranges = parts;
}

// Page "Practice": this song's trouble spots (the phrases where the song waited for the player most, red
// on the practice bar) with a button to practise each, how it went this time, and the practice parts.
void MenuPractice(Settings& e, const View& v, float s, bool* forget) {
    auto mmss = [](double t) {
        char b[16];
        const int x = (int)std::max(0.0, t);
        std::snprintf(b, sizeof(b), "%d:%02d", x / 60, x % 60);
        return std::string(b);
    };
    const double songEnd = v.songLength > 0 ? v.songLength : (v.phraseStarts.empty() ? 0 : v.phraseStarts.back() + 10);
    auto phrase = [&](size_t i) {
        return Range(v.phraseStarts[i], i + 1 < v.phraseStarts.size() ? v.phraseStarts[i + 1] : songEnd);
    };
    std::vector<size_t> spots;  // the hardest first, at most 6
    for (size_t i = 0; i < v.phraseHeat.size() && i < v.phraseStarts.size(); ++i)
        if (v.phraseHeat[i] > 0) spots.push_back(i);
    std::stable_sort(spots.begin(), spots.end(), [&](size_t a, size_t b) { return v.phraseHeat[a] > v.phraseHeat[b]; });
    if (spots.size() > 6) spots.resize(6);

    ImGui::SeparatorText("Trouble spots");
    ImGui::PushTextWrapPos(0);
    if (!v.chartOk) {
        ImGui::TextDisabled("Play a song to see where you stop most.");
    } else if (spots.empty()) {
        ImGui::TextDisabled("None yet in this song. They show up as you play it with \"Wait for each note\": the "
                            "places where the song had to wait for you, longer waits and skipped notes counting more.");
    } else {
        ImGui::TextUnformatted("Where the song waited for you most (red on the practice bar, on the game's progress "
                               "bar). A note you play on time often enough in a row (below) is cleared; on the tab, the "
                               "dots under a note fill up green each time you play it right.");
        for (size_t i : spots) {
            const Range r = phrase(i);
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding();
            const std::string sec = SectionAt(v, r.first);  // "Chorus 2  2:23", or the times
            if (sec.empty()) ImGui::Text("%s - %s", mmss(r.first).c_str(), mmss(r.second).c_str());
            else ImGui::Text("%s  %s", sec.c_str(), mmss(r.first).c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 10.5f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.88f, 0.24f, 0.2f, 1));
            ImGui::ProgressBar(v.phraseHeat[i], ImVec2(220 * s, ImGui::GetFrameHeight() * 0.5f), "");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (i < v.phraseCleared.size() && v.phraseCleared[i].second > 0) {
                ImGui::TextDisabled("%d of %d notes cleared", v.phraseCleared[i].first, v.phraseCleared[i].second);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Practise")) SetPracticeParts({r});
            ImGui::PopID();
        }
        if (spots.size() > 1 && ImGui::Button("Practise all of them")) {
            std::vector<Range> all;
            for (size_t i : spots) all.push_back(phrase(i));
            SetPracticeParts(all);
        }
    }
    SliderRow("Clear a note after", "##troubleclear", &e.troubleClear, 1, 10,
              e.troubleClear == 1 ? "1 good try" : "%d good tries in a row",
              "How many times in a row you must play a note on time (without the song stopping) before it no longer "
              "counts as trouble. On the way there it fades. Going wrong again starts the count over.");
    ImGui::SeparatorText("This time");
    if (v.runSummary.empty()) ImGui::TextDisabled("Nothing played yet.");
    else ImGui::TextUnformatted(v.runSummary.c_str());
    ImGui::SeparatorText("Practice parts");
    if (g_drawRanges.empty()) {
        ImGui::TextDisabled("The whole song. Click a phrase on the practice bar, or drag over it, to practise just a part.");
    } else {
        std::string list;
        for (const auto& r : g_drawRanges) list += (list.empty() ? "" : ",  ") + mmss(r.first) + " - " + mmss(r.second);
        ImGui::Text("Waiting only in %s", list.c_str());
        if (ImGui::Button("Practise the whole song")) SetPracticeParts({});
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::BeginDisabled(!v.chartOk);
    if (ImGui::Button("Forget this song's trouble spots")) *forget = true;
    ImGui::EndDisabled();
    Help("Starts this song's record again (it's kept per song and arrangement, in the NoteByNote_stats folder).");
}

// Page "Colours": a ready-made theme, and any of its colours changed by the player. Picking a theme
// shows it as it is (the player's own colours are dropped). The string colours are the game's and
// never change (the banner names them: "the ORANGE string").
void MenuColours(Settings& e, float s) {
    const int cur = e.theme >= 0 && e.theme < theme::kThemeCount ? e.theme : 0;
    ImGui::SeparatorText("Theme");
    ImGui::SetNextItemWidth(320 * s);
    if (ImGui::BeginCombo("##theme", theme::kThemes[cur].name)) {
        for (int t = 0; t < theme::kThemeCount; ++t) {
            if (ImGui::Selectable(theme::kThemes[t].name, t == cur)) {
                e.theme = t;
                for (int& c : e.colors) c = -1;
            }
            if (t == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    Help("Colours of the banner, clock, tab and this menu. The string colours are always the game's.");
    ImGui::SeparatorText("Your colours");
    ImGui::TextDisabled("Click a square for the colour wheel, or type a hex code.");
    bool own = false;
    for (int i = 0; i < theme::kSlots; ++i) {
        const theme::Slot slot = (theme::Slot)i;
        ImGui::PushID(i);
        const uint32_t rgb = Color(e, slot);
        float c[3] = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
        ImGui::SetNextItemWidth(170 * s);
        // Hex box + swatch; the swatch opens a picker with the round hue wheel.
        if (ImGui::ColorEdit3("##c", c, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueWheel)) {
            auto byte = [](float f) { return (int)std::lround(std::max(0.0f, std::min(1.0f, f)) * 255); };
            const int v = (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]);
            e.colors[i] = v == (int)theme::kThemes[cur].color[i] ? -1 : v;  // back to the theme's = not "own"
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(theme::kSlotInfo[i].label);
        if (e.colors[i] >= 0) {
            own = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) e.colors[i] = -1;
            ImGui::SetItemTooltip("Put back the theme's colour");
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!own);
    if (ImGui::Button("Reset all colours to the theme"))
        for (int& c : e.colors) c = -1;
    ImGui::EndDisabled();
}

// Page "Game": things the mod does for the game itself (they apply from the next start).
void MenuGame(Settings& e) {
    ImGui::SeparatorText("When the game starts");
    ImGui::TextDisabled("These apply from the next time you start the game.");
    Check("Close the Ubisoft login and server popups", &e.skipPopups,
          "Answers the Ubisoft login and \"servers not available\" dialogs by itself. Press Enter and the profile choice stay yours.");
    bool fast = e.fastIntro > 1;
    if (Check("Play the start-up logos 4x faster", &fast)) e.fastIntro = fast ? 4 : 1;
    ImGui::SeparatorText("Stability");
    Check("Avoid the game's own random crash / freeze", &e.fixCrash,
          "Puts back a Windows function that the game's copy protection redirects (in memory only). "
          "Fixes a crash that happens mostly at start-up.");
}

void DrawMenu(const View& v, const Settings& st, float s, ImVec2 ds) {
    ImGui::PushFont(g_fontUi, 22 * s);
    // The first time: against the right edge of the screen, so the banner (top centre) and the tab
    // (left) stay visible to be dragged. After that it stays where the player dragged it (by its
    // title bar).
    ImGui::SetNextWindowPos(ImVec2(ds.x - 40 * s, ds.y * 0.5f), ImGuiCond_Once, ImVec2(1.0f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680 * s, 0), ImGuiCond_Always);
    if (!g_menuWasOpen) ImGui::SetNextWindowFocus();
    bool open = true, skip = false, forget = false;
    int calibration = 0;
    Settings e = st;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s, 7 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 5 * s));
    if (ImGui::Begin("Note-by-Note", &open, flags)) {
        // Header: the mode (bold), the chart in use, Skip on the right. One choice for the mode (it used to be
        // a "Wait for each note" box plus a "Stop the song" box on a page: two switches for one idea).
        ImGui::PushFont(g_fontBold, 24 * s);
        int mode = !e.enabled ? 0 : e.stopSong ? 2 : 1;
        ImGui::RadioButton("Off", &mode, 0);
        ImGui::SameLine(0, 24 * s);
        ImGui::RadioButton("Show the notes", &mode, 1);
        ImGui::SameLine(0, 24 * s);
        ImGui::RadioButton("Wait for each note", &mode, 2);
        ImGui::PopFont();
        Help("Wait for each note: the song stops at every note until you play it.\n"
             "Show the notes: the song plays on as usual; the banner shows each note to play (fretboard, hand, repeat "
             "counter) and moves on as the song passes it.\n"
             "Off: the game plays as usual (the tab and clock still show).");
        e.enabled = mode != 0;
        if (mode != 0) e.stopSong = mode == 2;  // (Off keeps the choice for when it's switched on again)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(v.chartOk ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImVec4(1.0f, 0.65f, 0.25f, 1), "%s",
                           v.chartInfo.empty() ? "No song playing" : v.chartInfo.c_str());
        const char* skipText = v.chord ? "Skip this chord (F9)" : "Skip this note (F9)";
        const float skipW = ImGui::CalcTextSize(skipText).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - skipW);
        ImGui::BeginDisabled(!v.waiting);
        if (ImGui::Button(skipText)) skip = true;
        ImGui::EndDisabled();
        ImGui::Spacing();

        // One tab per topic; every page gets the same height.
        if (ImGui::BeginTabBar("pages")) {
            static const char* const kPages[] = {"Playing", "Practice", "Tab", "Screen", "Colours", "Game"};
            for (int p = 0; p < 6; ++p) {
                if (!ImGui::BeginTabItem(kPages[p])) continue;
                ImGui::BeginChild("page", ImVec2(0, 540 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                switch (p) {
                    case 0: MenuPlaying(e, v, &calibration); break;
                    case 1: MenuPractice(e, v, s, &forget); break;
                    case 2: MenuTab(e); break;
                    case 3: MenuScreen(e); break;
                    case 4: MenuColours(e, s); break;
                    default: MenuGame(e); break;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // Footer.
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Saved automatically. The song is held while this menu is open.");
        const char* closeText = "Close (F8)";
        const float closeW = ImGui::CalcTextSize(closeText).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - closeW);
        if (ImGui::Button(closeText)) open = false;
        ImGui::PushFont(nullptr, 16 * s);
        ImGui::TextDisabled("Thanks to RS_ASIO, Rocksmith2014.NET, MinHook and Dear ImGui. Not affiliated with Ubisoft.");
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open = false;

    // Only when the menu itself changed something (compared with this frame's snapshot), so a layout
    // just saved by a drag or a change from the main loop is never overwritten with old values.
    std::lock_guard<std::mutex> lk(g.m);
    if (!(e == st)) g.settings = e;
    if (skip) g.skipRequest = true;
    if (forget) g.forgetRequest = true;
    if (calibration) g.calibrationRequest = calibration;
    if (!open) g_menuOpen = false;
}

// ------------------------------------------------------------------ ImGui setup / teardown
LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l);

std::string FontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

ImFont* LoadFont(const char* file) {
    const std::string p = FontPath(file);
    if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), 24.0f);
}

bool InitImGui(IDirect3DDevice9* dev) {
    // The game's window: the one the device was created for.
    HWND hwnd = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) hwnd = cp.hFocusWindow;
    if (!hwnd) {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
            D3DPRESENT_PARAMETERS pp{};
            if (SUCCEEDED(sc->GetPresentParameters(&pp))) hwnd = pp.hDeviceWindow;
            sc->Release();
        }
    }
    if (!hwnd) return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't write imgui.ini into the game folder
    io.LogFilename = nullptr;
    // Keyboard navigation in the menu; never change the Windows cursor (the game hides it; ImGui
    // draws its own while the menu is open).
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    g_fontUi = LoadFont("segoeui.ttf");
    g_fontBold = LoadFont("segoeuib.ttf");
    if (!g_fontUi) g_fontUi = io.Fonts->AddFontDefault();
    if (!g_fontBold) g_fontBold = g_fontUi;
    io.FontDefault = g_fontUi;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 12;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.WindowPadding = ImVec2(20, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 10);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.94f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.12f, 0.12f, 1.0f);
    g_baseStyle = st;
    g_styleScale = 0;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);
    g_dev = dev;
    g_hwnd = hwnd;
    g_gameWindow = hwnd;
    g_unicodeWnd = IsWindowUnicode(hwnd) != FALSE;
    g_oldWndProc = (WNDPROC)(g_unicodeWnd ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HkWndProc)
                                          : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HkWndProc));
    g_ready = true;
    Log("overlay: ready (window %p, %s)", (void*)hwnd, g_unicodeWnd ? "unicode" : "ansi");
    return true;
}

void RestoreWndProc() {
    if (!g_hwnd || !g_oldWndProc) return;
    const LONG_PTR cur = g_unicodeWnd ? GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
    if (cur == (LONG_PTR)HkWndProc) {
        if (g_unicodeWnd) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldWndProc);
        else SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldWndProc);
    } else {
        Log("overlay: WARNING someone else subclassed the game window after us; can't unhook it cleanly");
    }
    g_oldWndProc = nullptr;
}

void ShutdownImGui() {  // render thread, g_imgui held
    if (g_ready) {
        RestoreWndProc();
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ready = false;
    }
    g_shutdownDone = true;
}

// Style sizes follow the screen height (designed for 1080p).
void ApplyScale(float s) {
    if (std::fabs(s - g_styleScale) < 0.01f) return;
    g_styleScale = s;
    ImGui::GetStyle() = g_baseStyle;
    ImGui::GetStyle().ScaleAllSizes(s);
}

// ------------------------------------------------------------------ one frame
void Frame(IDirect3DDevice9* dev) {
    std::lock_guard<std::recursive_mutex> lk(g_imgui);
    if (g_stopRequested) { ShutdownImGui(); return; }
    if (g_shutdownDone) return;
    if (dev->TestCooperativeLevel() != D3D_OK) return;  // device lost (alt-tab in fullscreen)
    if (!g_ready && !InitImGui(dev)) return;
    if (dev != g_dev) {  // the game created a new device
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplDX9_Init(dev);
        g_dev = dev;
    }
    if (++g_frames == 1) Log("overlay: first frame drawn");

    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);

    // Snapshot of what to show.
    View v;
    Settings st;
    std::string toast;
    DWORD toastUntil;
    {
        std::lock_guard<std::mutex> lk2(g.m);
        v = g.view;
        st = g.settings;
        toast = g.toast;
        toastUntil = g.toastUntil;
    }
    const bool menu = g_menuOpen;

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2((float)desc.Width, (float)desc.Height);  // draw in back-buffer pixels
    io.MouseDrawCursor = menu;
    if (menu != g_menuWasOpen) io.ClearInputKeys();  // no keys "stuck down" from before
    const float s = desc.Height / 1080.0f;
    ApplyScale(s);
    LoadPalette(st);  // this frame's theme colours; the menu's accent colours follow it too
    ApplyMenuStyle();
    ImGui::NewFrame();

    // Mouse arranging (menu open) uses last frame's boxes; then this frame's drawing records new ones.
    const ImVec2 ds = io.DisplaySize;
    Settings lay = st;  // the settings, with the layout being dragged
    Arrange(menu, &lay, s, ds);
    for (Box& b : g_box) b.drawn = false;

    // While the menu is open, every part that is switched on is shown so it can be arranged: with
    // example content when it has nothing to show right now.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under the menu window
    const bool bannerOn = st.enabled && st.showBanner;
    const bool bannerShown = DrawCalmBanner(dl, v, lay, bannerOn, s, ds);
    if (st.enabled) DrawCountIn(dl, v, s, ds);
    if (g_clearDrawRanges.exchange(false)) g_drawRanges.clear();
    if (!menu) DrawPracticeBar(dl, v, st, s, ds);
    else g_barL = g_barT = g_barR = g_barB = 0;
    if (!bannerShown && bannerOn && menu) {
        View ex;  // "Play fret 5 on the BLUE string" (D string, note G)
        ex.bass = v.bass;
        ex.string = 2;
        ex.fret = 5;
        ex.midi = v.bass ? 43 : 55;
        DrawBanner(dl, ex, lay, s, ds);
    }
    if (bannerOn && menu && g_box[kBanner].drawn && !g_box[kMistake].drawn) {
        // An example wrong note (2 frets too high on the banner's string), so its panel can be arranged.
        const bool real = bannerShown && !v.chord && v.midi >= 0 && v.string >= 0 && v.string < (v.bass ? 4 : 6);
        View ex;
        ex.bass = v.bass;
        ex.string = real ? v.string : 2;
        ex.fret = real ? v.fret : 5;
        ex.midi = real ? v.midi : (v.bass ? 43 : 55);
        const int d = ex.fret + 2 <= 24 ? 2 : -2;
        ex.hint = {{"You played " + music::NoteName(ex.midi + d) + "  -  ", hint::kGrey},
                   {std::string(d > 0 ? "move DOWN" : "move UP") + " 2 frets, to fret " + std::to_string(ex.fret) + " on ", hint::kWhite},
                   {StringLabel(lay, ex.string, v.bass ? 4 : 6), ex.string}};
        ex.heardAt = {{ex.string, ex.fret + d, ex.midi + d, true}};
        DrawMistakePanel(dl, ex, lay, s, ds, g_box[kBanner].p0, g_box[kBanner].p1);
    }
    const bool haveTime = v.inSong && v.songTime >= 0;
    if (st.showClock && haveTime) {
        DrawClock(dl, v, lay, s, ds);
    } else if (st.showClock && menu) {
        View ex;
        ex.songTime = 83;
        ex.songLength = 268;
        DrawClock(dl, ex, lay, s, ds);
    }
    if (st.showTab && haveTime && (!v.tab.empty() || !v.tabBeats.empty())) {
        DrawTab(dl, v, lay, s, ds);
    } else if (st.showTab && menu) {
        View ex;  // just the empty strings
        ex.bass = v.bass;
        ex.songTime = 0;
        DrawTab(dl, ex, lay, s, ds);
    }
    if (menu) DrawArrangeHints(dl, s);
    // The message fades in (0.15 s) and out (0.4 s); the same or a new one arriving while it's up just
    // keeps it up (only the text changes).
    static Fade s_toastFade;
    static std::string s_toastText;
    const bool toastUp = !toast.empty() && GetTickCount() < toastUntil;
    if (toastUp) s_toastText = toast;  // (while fading out: the last text)
    s_toastFade.Step(toastUp, io.DeltaTime, 0.15f, 0.4f);
    DrawToast(dl, s_toastText, s_toastFade.alpha, s, ds);
    if (menu) DrawMenu(v, st, s, ds);
    g_menuWasOpen = menu;

    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd->TotalVtxCount > 0) {  // nothing to draw -> don't touch the device at all
        IDirect3DSurface9* oldRt = nullptr;
        dev->GetRenderTarget(0, &oldRt);
        D3DVIEWPORT9 vp{};
        dev->GetViewport(&vp);
        dev->SetRenderTarget(0, back);
        if (SUCCEEDED(dev->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(dd);
            dev->EndScene();
        }
        if (oldRt) {
            dev->SetRenderTarget(0, oldRt);
            oldRt->Release();
        }
        dev->SetViewport(&vp);
    }
    back->Release();
}

// ------------------------------------------------------------------ hooks
HRESULT APIENTRY HkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    InFlight f;
    Frame(dev);
    return oPresent(dev, src, dst, wnd, dirty);
}

HRESULT APIENTRY HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    InFlight f;
    {
        std::lock_guard<std::recursive_mutex> lk(g_imgui);
        if (g_ready && dev == g_dev) ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    const HRESULT hr = oReset(dev, pp);
    {
        std::lock_guard<std::recursive_mutex> lk(g_imgui);
        if (g_ready && dev == g_dev && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    }
    Log("overlay: device reset (%s)", SUCCEEDED(hr) ? "ok" : "failed");
    return hr;
}

LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    InFlight f;
    WNDPROC old = g_oldWndProc;
    // The practice bar: the mouse works on it any time; its clicks don't reach the game.
    if (!g_menuOpen && (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP)) {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        g_mouseX = x;
        g_mouseY = y;
        if (m == WM_MOUSEMOVE) g_mouseMoved = GetTickCount();
        if (m == WM_LBUTTONDOWN && OverBar(x, y)) {
            g_mouseLeft = true;
            SetCapture(h);
            return 0;
        }
        if (m == WM_LBUTTONUP && g_mouseLeft) {
            g_mouseLeft = false;
            ReleaseCapture();
            return 0;
        }
    }
    if (g_menuOpen) {
        // Our menu has the keyboard and mouse: ImGui gets them, the game doesn't. (System keys like
        // Alt+F4 still pass.)
        const bool key = m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR;
        const bool mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
        if (key || mouse || m == WM_INPUT) {
            {
                std::lock_guard<std::recursive_mutex> lk(g_imgui);
                if (g_ready) ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            }
            if (m == WM_INPUT) return g_unicodeWnd ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
            return 0;
        }
    }
    if (!old) return g_unicodeWnd ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
    return g_unicodeWnd ? CallWindowProcW(old, h, m, w, l) : CallWindowProcA(old, h, m, w, l);
}

// Finds d3d9.dll's IDirect3DDevice9::Reset (vtable slot 16) and Present (slot 17) using a temporary
// device on a hidden window.
bool FindDeviceFunctions(HMODULE d3d9) {
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    auto create = (CreateFn)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create) return false;
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) return false;
    HWND wnd = CreateWindowExW(0, L"STATIC", L"NoteByNote dummy", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr) && dev) {
        void** vt = *(void***)dev;
        g_resetAddr = vt[16];
        g_presentAddr = vt[17];
        dev->Release();
    } else {
        Log("overlay: couldn't create the temporary D3D9 device (0x%08X)", (unsigned)hr);
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    return g_presentAddr && g_resetAddr;
}

DWORD WINAPI InstallThread(LPVOID) {
    HMODULE d3d9 = nullptr;
    for (int i = 0; i < 1200 && !g_stopRequested; ++i) {  // up to 2 minutes
        d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (d3d9) break;
        Sleep(100);
    }
    if (!d3d9 || g_stopRequested) {
        if (!g_stopRequested) Log("overlay: d3d9.dll never loaded; no on-screen display");
        return 0;
    }
    if (!FindDeviceFunctions(d3d9)) return 0;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("overlay: MinHook init failed: %s", MH_StatusToString(st));
        return 0;
    }
    MH_STATUS a = MH_CreateHook(g_presentAddr, (void*)&HkPresent, (void**)&oPresent);
    MH_STATUS b = MH_CreateHook(g_resetAddr, (void*)&HkReset, (void**)&oReset);
    MH_STATUS c = (a == MH_OK && b == MH_OK) ? MH_EnableHook(MH_ALL_HOOKS) : MH_UNKNOWN;
    Log("overlay: hook Present d3d9+0x%X: %s, Reset d3d9+0x%X: %s, enable: %s", (unsigned)((char*)g_presentAddr - (char*)d3d9),
        MH_StatusToString(a), (unsigned)((char*)g_resetAddr - (char*)d3d9), MH_StatusToString(b), MH_StatusToString(c));
    if (c != MH_OK) {
        MH_Uninitialize();
        return 0;
    }
    g_hooked = true;
    return 0;
}

}  // namespace

// ------------------------------------------------------------------ public API
Settings WithDefaultLayout(Settings st) {
    CopyLayout(Settings{}, &st);
    return st;
}

uint32_t Color(const Settings& st, theme::Slot slot) {
    if (st.colors[slot] >= 0) return (uint32_t)st.colors[slot] & 0xFFFFFF;
    const int t = st.theme >= 0 && st.theme < theme::kThemeCount ? st.theme : 0;
    return theme::kThemes[t].color[slot];
}

void Start(const Settings& initial) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.settings = initial;
    }
    g_installThread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
}

void Stop() {
    g_stopRequested = true;
    g_menuOpen = false;
    if (g_installThread) {
        WaitForSingleObject(g_installThread, 5000);
        CloseHandle(g_installThread);
        g_installThread = nullptr;
    }
    if (!g_hooked) return;
    // Let the render thread free ImGui's D3D objects on its next frame (up to 2 s).
    for (int i = 0; i < 200 && g_ready && !g_shutdownDone; ++i) Sleep(10);
    {
        std::lock_guard<std::recursive_mutex> lk(g_imgui);
        if (g_ready) {  // the game isn't drawing (minimized?): at least give the window back
            Log("overlay: no frame to clean up in; leaving ImGui's objects");
            RestoreWndProc();
        }
    }
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 1000 && g_inFlight > 0; ++i) Sleep(1);
    Sleep(50);  // a thread may have just entered a trampoline
    MH_Uninitialize();
    g_hooked = false;
}

void SetView(const View& v) {
    std::lock_guard<std::mutex> lk(g.m);
    g.view = v;
}

void Toast(const std::string& text, DWORD ms) {
    std::lock_guard<std::mutex> lk(g.m);
    g.toast = text;
    g.toastUntil = GetTickCount() + ms;
}

void ToggleMenu() { g_menuOpen = !g_menuOpen; }
HWND GameWindow() { return g_gameWindow; }
bool MenuOpen() { return g_menuOpen; }

Settings GetSettings() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.settings;
}

void SetEnabled(bool on) {
    std::lock_guard<std::mutex> lk(g.m);
    g.settings.enabled = on;
}

std::vector<Range> GetRanges() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.ranges;
}

void ClearRanges() {
    std::lock_guard<std::mutex> lk(g.m);
    g.ranges.clear();
    g_clearDrawRanges = true;  // the render thread drops its copy on the next frame
}

int TakeCalibrationRequest() {
    std::lock_guard<std::mutex> lk(g.m);
    const int r = g.calibrationRequest;
    g.calibrationRequest = 0;
    return r;
}

bool TakeSkipRequest() {
    std::lock_guard<std::mutex> lk(g.m);
    const bool r = g.skipRequest;
    g.skipRequest = false;
    return r;
}

bool TakeForgetRequest() {
    std::lock_guard<std::mutex> lk(g.m);
    const bool r = g.forgetRequest;
    g.forgetRequest = false;
    return r;
}

}  // namespace nbn::overlay
