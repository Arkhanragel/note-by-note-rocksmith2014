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
    DWORD toastStart = 0, toastUntil = 0;
    bool skipRequest = false;
} g;
std::atomic<bool> g_menuOpen{false};

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
}

// ------------------------------------------------------------------ movable parts
// The parts the player can drag while the menu is open. Each Draw* function records where it drew
// its part (render thread only); the next frame's mouse handling hit-tests those boxes.
enum Part { kBanner, kClock, kTab, kParts };
const char* kPartName[kParts] = {"Banner", "Clock", "Tab"};
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
const char* kColorName[6] = {"RED", "YELLOW", "BLUE", "ORANGE", "GREEN", "PURPLE"};
const char* kStringName[6] = {"E", "A", "D", "G", "B", "e"};  // bass uses the first four
const char* kOrdinal[6] = {"1st", "2nd", "3rd", "4th", "5th", "6th"};

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

void DrawSegs(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, const std::vector<Seg>& segs) {
    for (const auto& s : segs) {
        dl->AddText(f, size, pos, s.col, s.text.c_str());
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0, s.text.c_str()).x;
    }
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

// The "waiting" banner: what to play in words (+ colour) and as a tiny tab.
// S = screen scale (height / 1080); sizes also follow the player's banner size.
void DrawBanner(ImDrawList* dl, const View& v, const Settings& st, float S, ImVec2 ds) {
    const float s = S * st.bannerSize / 100.0f;
    const int n = v.bass ? 4 : 6;
    const int i = v.string < 0 ? 0 : (v.string >= n ? n - 1 : v.string);
    const ImU32 col = kStringColor[i];
    const float big = 46 * s, mid = 26 * s, tiny = 20 * s;

    char fret[32];
    std::snprintf(fret, sizeof(fret), "fret %d", v.fret);
    std::vector<Seg> line1;
    if (v.fret == 0) line1 = {{"Play the ", Col(theme::kText)}, {std::string(kColorName[i]) + " string", col}, {" open", Col(theme::kText)}};
    else line1 = {{"Play ", Col(theme::kText)}, {fret, Col(theme::kText)}, {" on the ", Col(theme::kText)}, {std::string(kColorName[i]) + " string", col}};

    char which[96];
    if (i == 0) std::snprintf(which, sizeof(which), "%s string - the thickest one", kStringName[i]);
    else if (i == n - 1) std::snprintf(which, sizeof(which), "%s string - the thinnest one", kStringName[i]);
    else std::snprintf(which, sizeof(which), "%s string - the %s counting from the thickest", kStringName[i], kOrdinal[i]);
    std::vector<Seg> line2 = {{which, Col(theme::kText)}};
    if (v.midi >= 0) line2.push_back({"   \xC2\xB7   note " + music::NoteName(v.midi), Col(theme::kTextDim)});
    const std::vector<Seg> line3 = {{v.fret == 0 ? "(no finger on the neck)   " : "", Col(theme::kTextDim)}, {"F9 = skip   F8 = menu", Col(theme::kTextDim)}};
    const std::vector<Seg> lineH = HintSegs(v.hint);

    const float textW = std::max({SegsWidth(g_fontBold, big, line1), SegsWidth(g_fontUi, mid, line2),
                                  SegsWidth(g_fontUi, mid, lineH), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + mid + 10 * s + (lineH.empty() ? 0 : mid + 10 * s) + tiny;

    // Tab picture: thinnest string on top, like tab and sheet music.
    const float gap = 17 * s, tabW = 190 * s, labelW = 22 * s;
    const float tabH = gap * (n - 1);
    const float pad = 24 * s, sep = 34 * s;
    const float w = pad + textW + sep + labelW + tabW + pad;
    const float h = pad + std::max(textH, tabH + 16 * s) + pad;
    const ImVec2 p0 = BannerPlace(st, S, w, h, ds);
    const ImVec2 p1(p0.x + w, p0.y + h);
    g_box[kBanner] = {p0, p1, true};

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 222), 14 * s);
    dl->AddRect(p0, p1, (col & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 14 * s, 0, 3.5f * s);

    ImVec2 t(p0.x + pad, p0.y + (h - textH) * 0.5f);
    DrawSegs(dl, g_fontBold, big, t, line1);
    t.y += big + 8 * s;
    DrawSegs(dl, g_fontUi, mid, t, line2);
    t.y += mid + 10 * s;
    if (!lineH.empty()) {
        DrawSegs(dl, g_fontUi, mid, t, lineH);
        t.y += mid + 10 * s;
    }
    DrawSegs(dl, g_fontUi, tiny, t, line3);

    const float tx = p0.x + pad + textW + sep, ty = p0.y + (h - tabH) * 0.5f;
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

    std::vector<Seg> line2;  // "RED open = E    YELLOW 2 = B ..." from the thickest string
    int played = 0;
    for (int i = 0; i < n; ++i) {
        if (v.frets[i] < 0) continue;
        if (played++) line2.push_back({"     ", Col(theme::kText)});
        line2.push_back({kColorName[i], kStringColor[i]});
        line2.push_back({v.frets[i] == 0 ? " open" : " " + std::to_string(v.frets[i]), Col(theme::kText)});
        if (v.notes[i] >= 0) line2.push_back({" = " + music::NoteName(v.notes[i], flats), Col(theme::kTextDim)});
    }
    const std::vector<Seg> line3 = {{played < n ? "x = don't play that string   " : "", Col(theme::kTextDim)}, {"F9 = skip   F8 = menu", Col(theme::kTextDim)}};
    const std::vector<Seg> lineH = HintSegs(v.hint);

    const float textW = std::max({SegsWidth(g_fontBold, big, line1), SegsWidth(g_fontUi, mid, lineM), SegsWidth(g_fontUi, mid, line2),
                                  SegsWidth(g_fontUi, mid, lineH), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + (lineM.empty() ? 0 : mid + 8 * s) + mid + 10 * s + (lineH.empty() ? 0 : mid + 10 * s) + tiny;

    // Tab picture, thinnest string on top; wider string spacing than the single-note tab so a
    // bubble fits on every string.
    const float gap = 34 * s, tabW = 150 * s, labelW = 22 * s;
    const float tabH = gap * (n - 1);
    const float pad = 24 * s, sep = 34 * s;
    const float w = pad + textW + sep + labelW + tabW + pad;
    const float h = pad + std::max(textH, tabH + 30 * s) + pad;
    const ImVec2 p0 = BannerPlace(st, S, w, h, ds);
    const ImVec2 p1(p0.x + w, p0.y + h);
    g_box[kBanner] = {p0, p1, true};

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 222), 14 * s);
    dl->AddRect(p0, p1, (gold & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 14 * s, 0, 3.5f * s);

    ImVec2 t(p0.x + pad, p0.y + (h - textH) * 0.5f);
    DrawSegs(dl, g_fontBold, big, t, line1);
    t.y += big + 8 * s;
    if (!lineM.empty()) {
        DrawSegs(dl, g_fontUi, mid, t, lineM);
        t.y += mid + 8 * s;
    }
    DrawSegs(dl, g_fontUi, mid, t, line2);
    t.y += mid + 10 * s;
    if (!lineH.empty()) {
        DrawSegs(dl, g_fontUi, mid, t, lineH);
        t.y += mid + 10 * s;
    }
    DrawSegs(dl, g_fontUi, tiny, t, line3);

    const float tx = p0.x + pad + textW + sep, ty = p0.y + (h - tabH) * 0.5f;
    const MiniTab mt(st, tx, labelW, tabW, s);
    const float bx = mt.lineL + tabW * 0.5f, fs = 20 * s;
    for (int str = 0; str < n; ++str) {
        const float y = ty + MiniTabRow(st, str, n) * gap;
        const bool on = v.frets[str] >= 0;
        const ImU32 c = on ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (90u << 24));
        const char* name = kStringName[str];
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, name);
        dl->AddText(g_fontUi, tiny, ImVec2(mt.labelX, y - ns.y * 0.5f), c, name);
        dl->AddLine(ImVec2(mt.lineL, y), ImVec2(mt.lineL + tabW, y), c, on ? 4 * s : 2 * s);
        const std::string label = on ? std::to_string(v.frets[str]) : "x";
        const ImVec2 lsz = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
        if (on) {
            const float rad = std::min(gap * 0.48f, std::max(lsz.x, lsz.y) * 0.5f + 4 * s);
            dl->AddCircleFilled(ImVec2(bx, y), rad, Col(theme::kPanel, 255));
            dl->AddCircle(ImVec2(bx, y), rad, kStringColor[str], 0, 3 * s);
        }
        dl->AddText(g_fontBold, fs, ImVec2(bx - lsz.x * 0.5f, y - lsz.y * 0.5f), on ? Col(theme::kText) : Col(theme::kTextDim), label.c_str());
    }
}

// The song clock, top-left by default: "1:23 / 4:28". Small and quiet, the game's HUD stays readable.
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
    const float w = ts.x + 2 * padX, h = ts.y + 2 * padY;
    const ImVec2 p0 = Place(st.clockX * S, st.clockY * S, w, h, ds);
    const ImVec2 p1(p0.x + w, p0.y + h);
    g_box[kClock] = {p0, p1, true};
    dl->AddRectFilled(p0, p1, Col(theme::kPanel, 170), 8 * s);
    dl->AddText(g_fontBold, size, ImVec2(p0.x + padX, p0.y + padY), Col(theme::kText, 230), text.c_str());
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
    const float s = S * st.tabSize / 100.0f;  // the tab's own size: string gap, text
    const int n = v.bass ? 4 : 6;
    const ImU32 gold = Col(theme::kChord);
    // top: a lane for chord names (y0 + 6), then the bar numbers right above the strings.
    // bottom: room under the low string, plus the rhythm lane (stems, beams, triplet "3") when shown.
    const bool rhythm = st.tabRhythm && !v.tabBeats.empty();
    const float stemLen = 24 * s;
    const float w = std::min(ds.x, st.tabWidth * S), gap = 28 * s, top = 60 * s, labelW = 26 * s, pad = 12 * s;
    const float bottom = rhythm ? 18 * s + stemLen + 18 * s : 18 * s;
    // Rows (pages only, setting tabRows): the box holds 1..4 staffs, one under the other.
    const int rows = st.tabPage ? std::max(1, std::min(4, st.tabRows)) : 1;
    const bool multiRow = rows > 1;
    const float rowH = top + gap * (n - 1) + bottom;  // one staff with its lanes
    const float h = rowH * rows;
    // Position from the settings, kept on screen.
    const ImVec2 p0 = Place(ds.x * 0.5f + st.tabX * S, st.tabY * S, w, h, ds);
    const float x0 = p0.x, y0 = p0.y;
    g_box[kTab] = {p0, ImVec2(x0 + w, y0 + h), true};
    // Background: how solid is the player's choice (100 % hides the game's own text behind the tab).
    const int bgA = std::max(0, std::min(100, st.tabOpacity)) * 255 / 100;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), Col(theme::kPanel, bgA), 10 * s);

    // Left-handed (tabMirror): the string names go on the right and time runs right to left. All the
    // timing below (cursor start, pages, zoom) is worked out as usual, left to right; only timeX()
    // flips the result, so the drawing code must not assume "later = further right" (it uses dir).
    const bool mirror = st.tabMirror;
    const float dir = mirror ? -1.0f : 1.0f;  // +1: later notes are to the right; -1: to the left
    const float lineL = x0 + pad + (mirror ? 0 : labelW), lineR = x0 + w - pad - (mirror ? labelW : 0);
    const float labelX = mirror ? lineR + 10 * s : x0 + pad;  // where the string names start
    // Where "now" sits: scrolling = the fixed line, a little of the past visible on its left (kCursorStart).
    // Pages: a new page first repeats the end of the previous one (recap, the player's setting), and the
    // cursor starts right after it. The speed (pixels per second) doesn't depend on the recap, so the
    // spacing of the notes stays the same whatever is chosen.
    constexpr float kCursorStart = 0.08f;  // of the width, from the left edge (+ a small margin)
    const float recap = st.tabPage ? std::max(0, std::min(50, st.tabRecap)) / 100.0f : kCursorStart;
    const float pageL = lineL + 6 * s;
    const float nowX = pageL + (lineR - pageL) * kCursorStart;
    const double secs = std::max(1, st.tabSeconds);
    const float pxPerS = (float)((lineR - nowX) / secs);
    // nsz: the size of the fret numbers and their boxes. Starts at the player's note size (menu), and in fast
    // passages shrinks by itself a little below it (see the zoom below); the items' widths are measured at
    // the player's size first. fs follows it.
    const float baseNote = std::max(60, std::min(130, st.tabNoteSize)) / 100.0f;
    float nsz = baseNote;
    const float tiny = 19 * s;
    float fs = 21 * s * nsz;
    float staffY = y0;  // top of the staff being drawn (the lower row: y0 + rowH)
    // String -> y: thinnest on top (printed tab), or thickest on top (tabThickTop). Code that needs the
    // staff's top or bottom line uses staffTopY()/staffBotY(), not a particular string.
    auto rowY = [&](int str) { return staffY + top + (st.tabThickTop ? str : n - 1 - str) * gap; };
    auto staffTopY = [&] { return staffY + top; };
    auto staffBotY = [&] { return staffY + top + (n - 1) * gap; };

    const double now = v.songTime;
    // The next note to play (highlighted, and where a run's "x8" counts from): while the song waits,
    // exactly the note it waits for (the clock stops a few ms past it, sometimes more than 20 ms, which
    // made the note AFTER it look "next"); while playing, the first one not yet past.
    // holdT: the note the song waits for, or will stop at next if it isn't played (-1 = none).
    const double holdT = v.waiting && v.waitTime >= 0 ? v.waitTime : v.nextWaitTime;
    const double nextFrom = holdT >= 0 && now > holdT - 0.02 ? holdT - 0.002 : now - 0.02;
    // Where the cursor ("now" line) is drawn. It never passes holdT: the song stops a little past that
    // note (chords: up to 200 ms while the chord detector decides), and a cursor following the song
    // there had to jump back onto the note. So it stops ON the note as the song reaches it, and once
    // the note is played or skipped it catches up with the song at 2.5x speed (a 0.2 s gap closes in
    // ~0.13 s) instead of jumping forward. A seek or a new song moves it at once.
    const double cursorWant = holdT >= 0 ? std::min(now, holdT) : now;
    static double s_cursorT = -1e9;
    const double frameDt = std::min(0.1, (double)ImGui::GetIO().DeltaTime);
    if (cursorWant > s_cursorT + 1.0 || cursorWant < s_cursorT - 0.25) s_cursorT = cursorWant;  // seek / new song
    else if (cursorWant < s_cursorT) s_cursorT = cursorWant;  // a small step back: follow it
    else s_cursorT = std::min(cursorWant, s_cursorT + frameDt * 2.5);
    const double cursorT = s_cursorT;

    // What gets drawn: one item per note or chord. With "spread" on, a fast repeat of the same fret
    // on the same string (4+ notes, each within kRunGap of the previous one) becomes ONE item drawn
    // as "12 x8" (x = how many are still to play), so a tremolo-like run doesn't fill the tab.
    struct Item {
        const TabNote* note;  // the (first) note: frets, chord name, ignore
        double time, last;    // first and last note's time (the same unless it's a run)
        double end;           // when the last note stops ringing
        int count, left;      // notes in the run / still to play (1 / 1 for a plain note)
        float half;           // half the width it takes on screen (widest fret box or chord name)
    };
    const bool spread = st.tabSpread;
    constexpr double kRunGap = 0.12;  // 16th notes at 125 bpm or faster
    auto sameFret = [](const TabNote& a, const TabNote& b) {
        return !a.chord && !b.chord && a.ignore == b.ignore && std::equal(std::begin(a.frets), std::end(a.frets), b.frets);
    };
    // The fret box label, and for a run the small "x8" after it.
    auto runText = [](const Item& it) { return it.count > 1 ? "x" + std::to_string(it.left > 0 ? it.left : it.count) : std::string(); };
    auto boxHalf = [&](const std::string& fret, const std::string& run) {
        const ImVec2 ls = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, fret.c_str());
        float w = std::max(ls.x, ls.y * 0.8f);
        if (!run.empty()) w += 3 * s * nsz + g_fontUi->CalcTextSizeA(tiny * 0.85f * nsz, FLT_MAX, 0, run.c_str()).x;
        return w * 0.5f + 5 * s * nsz;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < v.tab.size();) {
        size_t j = i + 1;
        if (spread)
            while (j < v.tab.size() && sameFret(v.tab[j], v.tab[i]) && v.tab[j].time - v.tab[j - 1].time <= kRunGap) ++j;
        if (j - i < 4) j = i + 1;  // 2 or 3 quick repeats stay separate notes
        Item it{&v.tab[i], v.tab[i].time, v.tab[j - 1].time, v.tab[j - 1].time + v.tab[j - 1].sustain, (int)(j - i), 0, 0};
        for (size_t k = i; k < j; ++k) it.left += v.tab[k].time >= nextFrom;
        const std::string run = runText(it);
        for (int str = 0; str < n; ++str)
            if (it.note->frets[str] >= 0) it.half = std::max(it.half, boxHalf(std::to_string(it.note->frets[str]), run));
        if (it.note->chord && !it.note->name.empty())
            it.half = std::max(it.half, g_fontBold->CalcTextSizeA(tiny, FLT_MAX, 0, it.note->name.c_str()).x * 0.5f + 2 * s);
        items.push_back(it);
        i = j;
    }

    // Time -> x: ONE speed for the whole tab at any moment, so every note moves at the same speed and
    // the spacing stays exactly proportional to time (the rhythm reads true). Spread: the tab zooms in
    // when the notes coming up are too close to read. Target zoom = the most any gap from 0.5 s ago to
    // the end of the tab needs (both half widths + a small gap, over the gap's length), at most kMaxZoom.
    // The zoom follows the target smoothly (zooming in in ~0.4 s, BEFORE the dense passage arrives,
    // since the target looks ahead the whole tab; back out in ~1.5 s once it has passed), so the speed
    // only changes gently, when the music gets denser or sparser. (Tried before: stretching each gap on
    // its own / a speed that varies along the tab: the notes sped up and slowed down as they moved.)
    constexpr double kMaxZoom = 6.0;
    auto needIn = [&](double from, double to) {  // the zoom the gaps between from and to need
        double need = 1.0;
        if (spread)
            for (size_t i = 1; i < items.size(); ++i) {
                const double a = items[i - 1].time, dt = items[i].time - a;
                if (dt <= 0 || items[i].time < from || a > to) continue;
                const double px = (items[i - 1].half + items[i].half + 4 * s) / pxPerS;
                need = std::max(need, std::min(kMaxZoom, px / dt));
            }
        return need;
    };
    const double target = needIn(now - 0.5, now + secs);
    const double frameS = std::min(0.1, (double)ImGui::GetIO().DeltaTime);
    // First the notes shrink (down to kMinNote of the player's size), so the tab keeps its speed; only
    // past that does it zoom (moves faster). need = zoom needed at the player's note size.
    constexpr double kMinNote = 0.75;
    auto shrinkFor = [&](double need) { return std::max(kMinNote, std::min(1.0, 1.0 / need)); };
    auto zoomFor = [&](double need) { return std::max(1.0, need * shrinkFor(need)); };
    static double s_zoom = 1.0;  // the zoom need being shown (smoothed)
    double originT = cursorT;    // time -> x: x = originX + (t - originT) * pxPerS * zoom
    float originX = nowX;
    // Several rows: page k (0 = the cursor's page) starts at pageT[k] with zoom need pageNeedK[k], on
    // row pageRow[k]; its first part repeats the previous page up to recapEnd[k] (drawn dimmed).
    double pageT[4] = {}, pageNeedK[4] = {1, 1, 1, 1}, recapEnd[4] = {-1e9, -1e9, -1e9, -1e9};
    int pageRow[4] = {0, 1, 2, 3};
    if (multiRow) {
        // Pages follow each other: the next page starts where the cursor leaves this one, minus the
        // recap (the end of this page, repeated on the left of the next), so the cursor jumps from the
        // right end of one row to the same point in the music on the next row. Each page's zoom is
        // set from the notes on it alone, when it's laid out, so its notes never move. The rows take
        // turns top to bottom: when the cursor leaves a row, that row gets the page after the last one.
        static double s_rowT = -1e9;   // song time at the left edge of the current page
        static double s_rowNeed = 1.0; // its zoom need
        static int s_row = 0;          // the row the current page is on
        originX = pageL;
        const float width = lineR - originX;
        auto pageLen = [&](double need) { return width / (pxPerS * zoomFor(need)); };  // seconds on a page
        // The need of the page starting at t: measured over the longest a page can be (need 1).
        auto pageNeed = [&](double t) { return needIn(t - 0.2, t + pageLen(1.0)); };
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
        pageT[0] = s_rowT;
        pageNeedK[0] = s_rowNeed;
        for (int k = 0; k < rows; ++k) {
            if (k > 0) {
                pageT[k] = after(pageT[k - 1], pageNeedK[k - 1]);
                pageNeedK[k] = pageNeed(pageT[k]);
                recapEnd[k] = pageT[k - 1] + pageLen(pageNeedK[k - 1]);  // where the previous page ends
            }
            pageRow[k] = (s_row + k) % rows;
        }
        originT = s_rowT;
        s_zoom = s_rowNeed;
    } else if (!st.tabPage) {
        // Scrolling: the notes move past a fixed "now" line. The zoom follows the target smoothly
        // (zooming in in ~0.4 s, BEFORE the dense passage arrives, since the target looks ahead the
        // whole tab; back out in ~1.5 s once it has passed).
        const double tau = target > s_zoom ? 0.4 : 1.5;
        s_zoom += (target - s_zoom) * (1.0 - std::exp(-frameS / tau));
    } else {
        // Pages (like Guitar Pro / Songsterr): the notes stand still and a cursor moves over them; fixed
        // numbers stay readable however fast the cursor goes. When the cursor passes 75 % of the width,
        // the page turns: the tab glides left (~0.3 s) so the cursor is back near the left edge. The
        // zoom only changes when a page turns (a denser passage coming up than this page was laid out
        // for turns the page early), so notes on a page never move.
        static double s_pageT = -1e9;   // song time at the left edge of the page (where it's going)
        static double s_shownT = -1e9;  // same, as shown (glides to s_pageT)
        static double s_pageNeed = 1.0; // zoom need the page was laid out for
        originX = pageL;
        const float width = lineR - originX;
        auto pageLen = [&](double need) { return width / (pxPerS * zoomFor(need)); };  // seconds on a page
        const double cursor = (now - s_pageT) / pageLen(s_pageNeed);  // 0..1 across the page
        const bool jumped = now < s_shownT - 0.05 || now > s_shownT + 3 * pageLen(s_pageNeed);  // seek / new song
        const double turnAt = recap + (1.0 - recap) * 0.73;  // 75 % of the width with the default recap
        if (jumped || cursor > turnAt || target > s_pageNeed * 1.25) {
            s_pageNeed = target;
            s_pageT = now - recap * pageLen(target);
            if (jumped) { s_shownT = s_pageT; s_zoom = s_pageNeed; }
        }
        const double k = 1.0 - std::exp(-frameS / 0.1);
        s_shownT += (s_pageT - s_shownT) * k;
        s_zoom += (s_pageNeed - s_zoom) * k;
        originT = s_shownT;
    }
    double zoom = 1.0;
    auto timeX = [&](double t) {
        const float x = originX + (float)((t - originT) * pxPerS * zoom);
        return mirror ? lineL + lineR - x : x;  // left-handed: the same layout, flipped
    };
    // Sets up one staff: its zoom need (note size + zoom), the song time at originX, its top.
    auto layout = [&](double need, double t, float top) {
        nsz = baseNote * (float)shrinkFor(need);
        fs = 21 * s * nsz;
        zoom = zoomFor(need);
        originT = t;
        staffY = top;
    };
    bool nextFound = false;  // the next note to play is highlighted once (on the cursor's row first)
    double dimBefore = -1e9; // rows still to come: notes before this (the recap) are drawn dimmed
    const float pulse = 0.6f + 0.4f * std::sin((float)ImGui::GetTime() * 5.0f);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), true);

    // One staff: the strings, the beat grid, the cursor ("now" line, only on the row it's on), the
    // rhythm lane and the notes, as set up by layout().
    auto drawStaff = [&](bool withCursor) {
        for (int str = 0; str < n; ++str) {
            const float y = rowY(str);
            const ImU32 c = (kStringColor[str] & 0x00FFFFFF) | (150u << 24);
            const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, kStringName[str]);
            dl->AddText(g_fontUi, tiny, ImVec2(labelX, y - ns.y * 0.5f), c, kStringName[str]);
            dl->AddLine(ImVec2(lineL, y), ImVec2(lineR, y), c, 1.5f * s);
        }
        const float cursorX = timeX(cursorT);  // the "now" line (fixed while scrolling, moving on pages)
        const float staffTop = staffTopY() - 8 * s, staffBottom = staffBotY() + 8 * s;
        dl->PushClipRect(ImVec2(lineL - 4 * s, staffY), ImVec2(lineR + 4 * s, staffY + rowH), true);

        // Rhythm grid, under everything else (like the bar lines of printed tab): every other bar gets a
        // faint shade so bars read at a glance, each bar starts with a clear line and its number, and the
        // other beats get a thin faint line. Notes between two beat lines are "in between" the beats.
        // (The beats always come with the View, for the rhythm below; the lines are the tabBeats setting.)
        static const std::vector<TabBeat> kNoBeats;
        const std::vector<TabBeat>& grid = st.tabBeats ? v.tabBeats : kNoBeats;
        for (size_t i = 0; i < grid.size(); ++i) {
            const TabBeat& b = grid[i];
            if (!b.downbeat || (b.measure & 1) == 0) continue;
            double end = b.time + 3600;  // until the next bar line (or off the right edge)
            for (size_t j = i + 1; j < grid.size(); ++j)
                if (grid[j].downbeat) { end = grid[j].time; break; }
            const float xs = timeX(b.time), xe = timeX(end);  // (xe < xs when left-handed)
            const float xa = std::max(lineL, std::min(xs, xe)), xb = std::min(lineR, std::max(xs, xe));
            if (xb > xa) dl->AddRectFilled(ImVec2(xa, staffTop), ImVec2(xb, staffBottom), Col(theme::kGrid, 14));
        }
        for (const TabBeat& b : grid) {
            const float x = timeX(b.time);
            if (x < lineL - 4 * s || x > lineR + 4 * s) continue;
            if (b.downbeat) {
                dl->AddLine(ImVec2(x, staffTop), ImVec2(x, staffBottom), Col(theme::kGrid, 150), 2 * s);
                const std::string num = std::to_string(b.measure);
                const ImVec2 ns = g_fontUi->CalcTextSizeA(15 * s, FLT_MAX, 0, num.c_str());  // centred on the line
                const float numY = staffTopY() - gap * 0.46f - 2 * s - ns.y;  // just above the top string's fret boxes
                dl->AddText(g_fontUi, 15 * s, ImVec2(std::floor(x - ns.x * 0.5f), std::floor(numY)), Col(theme::kRhythm, 170), num.c_str());
            } else {
                dl->AddLine(ImVec2(x, staffTop + 6 * s), ImVec2(x, staffBottom - 6 * s), Col(theme::kGrid, 45), 1 * s);
            }
        }

        // The "now" line: where the highway's notes reach the fretboard.
        if (withCursor)
            dl->AddLine(ImVec2(cursorX, staffTopY() - 16 * s), ImVec2(cursorX, staffBotY() + 12 * s), Col(theme::kHighlight, 200), 2.5f * s);

        // Played/passed notes fade out over half a second after they end (held notes stay while they
        // ring); ignored ones are always faint.
        // On pages they stay, dimmed, until the page turns (the left part of the page isn't left empty).
        auto alphaOf = [&](const Item& it) {
            float a = std::max(0.0f, 1.0f - (float)std::max(0.0, now - it.end) / 0.5f);
            if (st.tabPage) a = std::max(a, 0.35f);
            if (it.last < dimBefore) a = std::min(a, 0.35f);  // the recap of a row still to come
            return it.note->ignore ? a * 0.4f : a;
        };

        // Rhythm under the staff, like printed tab with rhythm (Guitar Pro, Songsterr): each note gets a
        // stem, and the beams say how many notes fit in one beat: none = 1 (quarter note), 1 beam = 2
        // (eighths), 2 = 4 (16ths), 3 = 8 (32nds), 4 = 16; a small "3" = triplets (3 in the time of 2).
        // Half notes get a short stem, whole notes none; a dot after the stem = dotted (1.5x as long).
        // A note's value = the time to the next note in the song's beats, rounded to the nearest of these.
        // Notes starting in the same beat are beamed together; a run ("12 x8") gets its own notes' value.
        if (rhythm && !items.empty()) {
            struct Value { double beats; int beams; bool dot, triplet; };
            static const Value kValues[] = {
                {4, 0, false, false},      {3, 0, true, false},     {2, 0, false, false},       {1.5, 0, true, false},
                {1, 0, false, false},      {0.75, 1, true, false},  {0.5, 1, false, false},     {1.0 / 3, 1, false, true},
                {0.375, 2, true, false},   {0.25, 2, false, false}, {1.0 / 6, 2, false, true},  {0.125, 3, false, false},
                {1.0 / 12, 3, false, true}, {0.0625, 4, false, false}};
            // The beat t falls in (index into tabBeats, -1 = before the first one) and that beat's length.
            const std::vector<TabBeat>& beats = v.tabBeats;
            auto beatAt = [&](double t, double* len) {
                const int i = (int)(std::upper_bound(beats.begin(), beats.end(), t,
                                                     [](double x, const TabBeat& b) { return x < b.time; }) - beats.begin()) - 1;
                const int j = std::max(0, std::min(i, (int)beats.size() - 2));
                *len = beats.size() > 1 ? beats[j + 1].time - beats[j].time : 0.5;
                return i;
            };
            struct Stem { float x, a; const Value* val; int beat; bool beamable; };
            std::vector<Stem> stems(items.size());
            for (size_t i = 0; i < items.size(); ++i) {
                const Item& it = items[i];
                double len = 0.5;
                const int beat = beatAt(it.time, &len);
                // Time to the next note; a run: the gap between its own notes; the last item: unknown (a beat).
                const double d = it.count > 1 ? (it.last - it.time) / (it.count - 1)
                                              : (i + 1 < items.size() ? items[i + 1].time - it.time : len);
                const double q = len > 0 && d > 0 ? d / len : 1;
                const Value* best = &kValues[4];
                for (const Value& val : kValues)
                    if (std::abs(std::log(q / val.beats)) < std::abs(std::log(q / best->beats))) best = &val;
                stems[i] = {timeX(it.time), alphaOf(it), best, beat, it.count == 1};
            }
            // Beamed together at level k: neighbours in the same beat, both with more than k beams.
            auto joined = [&](size_t i, int k) {
                return i + 1 < stems.size() && stems[i].beamable && stems[i + 1].beamable && stems[i].beat >= 0 &&
                       stems[i].beat == stems[i + 1].beat && stems[i].val->beams > k && stems[i + 1].val->beams > k;
            };
            const float yTop = staffBottom + 6 * s, yBot = yTop + stemLen, beamH = 3 * s, beamStep = 5 * s, stub = 8 * s;
            auto ink = [](float a) { return Col(theme::kRhythm, (int)(230 * a)); };
            for (size_t i = 0; i < stems.size(); ++i) {
                const Stem& m = stems[i];
                if (m.a <= 0 || m.x < lineL - 40 * s || m.x > lineR + 40 * s) continue;
                const Value& val = *m.val;
                if (val.beats >= 4 && !val.dot) continue;  // whole note: no stem
                const float end = val.beats >= 2 ? yTop + stemLen * 0.5f : yBot;  // half notes: a short stem
                dl->AddLine(ImVec2(m.x, yTop), ImVec2(m.x, end), ink(m.a), 1.5f * s);
                if (val.dot) dl->AddCircleFilled(ImVec2(m.x + 5 * s * dir, end - 4 * s), 1.8f * s, ink(m.a));
                for (int k = 0; k < val.beams; ++k) {
                    const float y = yBot - k * beamStep;
                    if (joined(i, k)) {  // a beam to the next note
                        const float xn = stems[i + 1].x;
                        dl->AddRectFilled(ImVec2(std::min(m.x, xn), y - beamH), ImVec2(std::max(m.x, xn), y),
                                          ink(std::min(m.a, stems[i + 1].a)));
                    } else if (!(i > 0 && joined(i - 1, k))) {
                        // Not beamed at this level on either side: a short stub, pointing to the note it
                        // shares a beam with (or forward in time when it's alone: a flag).
                        const bool back = i > 0 && joined(i - 1, 0);
                        const float x2 = back ? m.x - stub * dir : m.x + stub * dir;
                        dl->AddRectFilled(ImVec2(std::min(m.x, x2), y - beamH), ImVec2(std::max(m.x, x2), y), ink(m.a));
                    }
                }
                // Triplets: a "3" under each beamed group of triplet notes (under a lone one too).
                if (val.triplet && !(i > 0 && joined(i - 1, 0) && stems[i - 1].val->triplet)) {
                    size_t j = i;
                    while (joined(j, 0) && stems[j + 1].val->triplet) ++j;
                    const float cx = (m.x + stems[j].x) * 0.5f, fsz = 13 * s;
                    const ImVec2 ts = g_fontUi->CalcTextSizeA(fsz, FLT_MAX, 0, "3");
                    dl->AddText(g_fontUi, fsz, ImVec2(cx - ts.x * 0.5f, yBot + 2 * s), ink(m.a), "3");
                }
            }
        }

        for (const Item& it : items) {
            const TabNote& t = *it.note;
            const float x = timeX(it.time), xEnd = timeX(it.end);  // (xEnd < x when left-handed)
            if (std::max(x, xEnd) < lineL - 30 * s || std::min(x, xEnd) > lineR + 30 * s) continue;
            const float a = alphaOf(it);
            if (a <= 0) continue;
            const bool next = !nextFound && !t.ignore && it.last >= nextFrom;  // a run stays "next" until its last note
            if (next) nextFound = true;
            const std::string run = runText(it);

            int lo = -1, hi = -1;  // lowest/highest string played (for the chord bracket)
            for (int str = 0; str < n; ++str)
                if (t.frets[str] >= 0) { if (lo < 0) lo = str; hi = str; }
            if (lo < 0) continue;
            if (t.chord && hi > lo)
                dl->AddLine(ImVec2(x, rowY(hi)), ImVec2(x, rowY(lo)), Col(theme::kChord, (int)(170 * a)), 2 * s);
            if (t.chord && !t.name.empty()) {
                const ImVec2 ts = g_fontBold->CalcTextSizeA(tiny, FLT_MAX, 0, t.name.c_str());
                dl->AddText(g_fontBold, tiny, ImVec2(x - ts.x * 0.5f, staffY + 6 * s), (gold & 0x00FFFFFF) | ((ImU32)(255 * a) << 24),
                            t.name.c_str());
            }
            // Held notes (and runs): a tail in the string's colour until the note ends (like the highway's
            // tails).
            if (std::abs(xEnd - x) > it.half * nsz + 4 * s)
                for (int str = 0; str < n; ++str)
                    if (t.frets[str] >= 0)
                        dl->AddRectFilled(ImVec2(std::min(x, xEnd), rowY(str) - 3 * s), ImVec2(std::max(x, xEnd), rowY(str) + 3 * s),
                                          (kStringColor[str] & 0x00FFFFFF) | ((ImU32)(150 * a) << 24), 3 * s);
            for (int str = 0; str < n; ++str) {
                if (t.frets[str] < 0) continue;
                const std::string label = std::to_string(t.frets[str]);
                const ImVec2 ls = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
                const float y = rowY(str), bw = boxHalf(label, run), bh = gap * 0.46f * std::max(0.7f, std::min(1.1f, nsz));
                const ImU32 col = (kStringColor[str] & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);
                dl->AddRectFilled(ImVec2(x - bw, y - bh), ImVec2(x + bw, y + bh), Col(theme::kPanel, (int)(255 * a)), 5 * s);
                if (next) dl->AddRect(ImVec2(x - bw - 2 * s, y - bh - 2 * s), ImVec2(x + bw + 2 * s, y + bh + 2 * s),
                                      Col(theme::kHighlight, (int)(255 * pulse)), 6 * s, 0, 2.5f * s);
                else dl->AddRect(ImVec2(x - bw, y - bh), ImVec2(x + bw, y + bh), col, 5 * s, 0, 2 * s);
                if (run.empty()) {
                    dl->AddText(g_fontBold, fs, ImVec2(x - ls.x * 0.5f, y - ls.y * 0.5f), Col(theme::kText, (int)(255 * a)),
                                label.c_str());
                } else {  // "12" then a small, dimmer "x8"
                    const float rs = tiny * 0.85f * nsz;
                    const ImVec2 es = g_fontUi->CalcTextSizeA(rs, FLT_MAX, 0, run.c_str());
                    const float lx = x - (ls.x + 3 * s * nsz + es.x) * 0.5f;
                    dl->AddText(g_fontBold, fs, ImVec2(lx, y - ls.y * 0.5f), Col(theme::kText, (int)(255 * a)), label.c_str());
                    dl->AddText(g_fontUi, rs, ImVec2(lx + ls.x + 3 * s * nsz, y - es.y * 0.5f + 1 * s), Col(theme::kTextDim, (int)(230 * a)),
                                run.c_str());
                }
            }
        }
        dl->PopClipRect();
    };  // drawStaff

    if (multiRow) {
        // The cursor's page first (the next note is highlighted there if it's on two rows), then the
        // pages still to come, each with its recap dimmed; a thin line between the rows.
        for (int k = 0; k < rows; ++k) {
            layout(pageNeedK[k], pageT[k], y0 + pageRow[k] * rowH);
            dimBefore = recapEnd[k];
            drawStaff(k == 0);
        }
        for (int r = 1; r < rows; ++r)
            dl->AddLine(ImVec2(x0 + pad, y0 + r * rowH), ImVec2(x0 + w - pad, y0 + r * rowH), Col(theme::kGrid, 40), 1 * s);
    } else {
        layout(s_zoom, originT, y0);
        drawStaff(true);
    }
    dl->PopClipRect();
}

void DrawToast(ImDrawList* dl, const std::string& text, DWORD start, DWORD until, float s, ImVec2 ds) {
    const DWORD now = GetTickCount();
    if (text.empty() || now >= until) return;
    float a = 1.0f;
    if (until - now < 400) a = (until - now) / 400.0f;  // fade out
    if (now - start < 150) a = std::min(a, (now - start) / 150.0f);  // fade in
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
        const float m = 3 * S, g = 20 * S;
        const ImVec2 q0(b.p0.x - m, b.p0.y - m), q1(b.p1.x + m, b.p1.y + m);
        dl->AddRect(q0, q1, c, 8 * S, 0, (hot == p ? 2.5f : 1.5f) * S);
        dl->AddTriangleFilled(ImVec2(q1.x, q1.y - g), q1, ImVec2(q1.x - g, q1.y), c);
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
void MenuPlaying(Settings& e) {
    ImGui::SeparatorText("Waiting");
    ImGui::BeginDisabled(!e.enabled);
    Check("Wait for chords too", &e.waitChords, "Off: the song only waits for single notes; chords pass by themselves.");
    ImGui::EndDisabled();
    Check("Accept the same note an octave higher or lower", &e.acceptOctaves,
          "Useful if you play a riff in another position. Off is stricter: string and fret must match.");
    ImGui::SeparatorText("Timing");
    SliderRow("Stop before the note", "##lead", &e.leadMs, 0, 500, "%d ms",
              "The song stops this long before the note reaches the line, to give you time to see it.");
    SliderRow("Early notes count", "##early", &e.earlyMs, 0, 1000, "up to %d ms",
              "A right note played up to this early counts, and the song doesn't stop for it.");
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

    ImGui::SeparatorText("Strings");
    Check("Thickest string on top", &e.tabThickTop,
          "Off: thinnest string on top, like printed tab. On: thickest on top (for example if you play a flipped guitar). "
          "The banner's small tab follows.");
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    Check("Left-handed (right to left)", &e.tabMirror,
          "Time runs from right to left and the string names move to the right. The banner's small tab follows.");

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
    Check("Song time", &e.showClock, "A small clock, \"1:23 / 4:28\", top-left by default.");
    ImGui::SeparatorText("Arrange");
    ImGui::TextWrapped("While this menu is open, drag the banner, the clock or the tab to move it, and drag its "
                       "bottom-right corner to resize it. The menu itself moves by its title bar.");
    ImGui::Spacing();
    if (ImGui::Button("Reset positions and sizes")) e = WithDefaultLayout(e);
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
    bool open = true, skip = false;
    Settings e = st;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s, 7 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 5 * s));
    if (ImGui::Begin("Note-by-Note", &open, flags)) {
        // Header: the mode (bold), the chart in use, Skip on the right.
        ImGui::PushFont(g_fontBold, 26 * s);
        ImGui::Checkbox("Wait for each note", &e.enabled);
        ImGui::PopFont();
        Help("The song stops at every note until you play it. Off: the game plays as usual (the tab and clock still show).");
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
            static const char* const kPages[] = {"Playing", "Tab", "Screen", "Colours", "Game"};
            for (int p = 0; p < 5; ++p) {
                if (!ImGui::BeginTabItem(kPages[p])) continue;
                ImGui::BeginChild("page", ImVec2(0, 540 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                switch (p) {
                    case 0: MenuPlaying(e); break;
                    case 1: MenuTab(e); break;
                    case 2: MenuScreen(e); break;
                    case 3: MenuColours(e, s); break;
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
    DWORD toastStart, toastUntil;
    {
        std::lock_guard<std::mutex> lk2(g.m);
        v = g.view;
        st = g.settings;
        toast = g.toast;
        toastStart = g.toastStart;
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
    if (bannerOn && v.inSong && v.waiting) {
        if (v.chord) DrawChordBanner(dl, v, lay, s, ds);
        else DrawBanner(dl, v, lay, s, ds);
    } else if (bannerOn && menu) {
        View ex;  // "Play fret 5 on the BLUE string" (D string, note G)
        ex.bass = v.bass;
        ex.string = 2;
        ex.fret = 5;
        ex.midi = v.bass ? 43 : 55;
        DrawBanner(dl, ex, lay, s, ds);
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
    DrawToast(dl, toast, toastStart, toastUntil, s, ds);
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
        if ((d3d9 = GetModuleHandleW(L"d3d9.dll")) != nullptr) break;
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
    g.toastStart = GetTickCount();
    g.toastUntil = g.toastStart + ms;
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

bool TakeSkipRequest() {
    std::lock_guard<std::mutex> lk(g.m);
    const bool r = g.skipRequest;
    g.skipRequest = false;
    return r;
}

}  // namespace nbn::overlay
