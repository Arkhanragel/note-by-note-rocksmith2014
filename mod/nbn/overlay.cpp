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

bool Same(const Settings& a, const Settings& b) {
    return a.enabled == b.enabled && a.leadMs == b.leadMs && a.earlyMs == b.earlyMs &&
           a.acceptOctaves == b.acceptOctaves && a.showBanner == b.showBanner && a.waitChords == b.waitChords &&
           a.showClock == b.showClock;
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

const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kGrey = IM_COL32(175, 175, 185, 255);

struct Seg {  // a piece of text in one colour
    std::string text;
    ImU32 col;
};

float SegsWidth(ImFont* f, float size, const std::vector<Seg>& segs) {
    float w = 0;
    for (const auto& s : segs) w += f->CalcTextSizeA(size, FLT_MAX, 0, s.text.c_str()).x;
    return w;
}

void DrawSegs(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, const std::vector<Seg>& segs) {
    for (const auto& s : segs) {
        dl->AddText(f, size, pos, s.col, s.text.c_str());
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0, s.text.c_str()).x;
    }
}

// The "waiting" banner: what to play in words (+ colour) and as a tiny tab.
void DrawBanner(ImDrawList* dl, const View& v, float s, ImVec2 ds) {
    const int n = v.bass ? 4 : 6;
    const int i = v.string < 0 ? 0 : (v.string >= n ? n - 1 : v.string);
    const ImU32 col = kStringColor[i];
    const float big = 46 * s, mid = 26 * s, tiny = 20 * s;

    char fret[32];
    std::snprintf(fret, sizeof(fret), "fret %d", v.fret);
    std::vector<Seg> line1;
    if (v.fret == 0) line1 = {{"Play the ", kWhite}, {std::string(kColorName[i]) + " string", col}, {" open", kWhite}};
    else line1 = {{"Play ", kWhite}, {fret, kWhite}, {" on the ", kWhite}, {std::string(kColorName[i]) + " string", col}};

    char which[96];
    if (i == 0) std::snprintf(which, sizeof(which), "%s string - the thickest one", kStringName[i]);
    else if (i == n - 1) std::snprintf(which, sizeof(which), "%s string - the thinnest one", kStringName[i]);
    else std::snprintf(which, sizeof(which), "%s string - the %s counting from the thickest", kStringName[i], kOrdinal[i]);
    std::vector<Seg> line2 = {{which, kWhite}};
    if (v.midi >= 0) line2.push_back({"   \xC2\xB7   note " + music::NoteName(v.midi), kGrey});
    const std::vector<Seg> line3 = {{v.fret == 0 ? "(no finger on the neck)   " : "", kGrey}, {"F9 = skip   F8 = menu", kGrey}};

    const float textW = std::max({SegsWidth(g_fontBold, big, line1), SegsWidth(g_fontUi, mid, line2), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + mid + 10 * s + tiny;

    // Tab picture: thinnest string on top, like tab and sheet music.
    const float gap = 17 * s, tabW = 190 * s, labelW = 22 * s;
    const float tabH = gap * (n - 1);
    const float pad = 24 * s, sep = 34 * s;
    const float w = pad + textW + sep + labelW + tabW + pad;
    const float h = pad + std::max(textH, tabH + 16 * s) + pad;
    const ImVec2 p0(std::floor((ds.x - w) * 0.5f), std::floor(ds.y * 0.11f));
    const ImVec2 p1(p0.x + w, p0.y + h);

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 20, 222), 14 * s);
    dl->AddRect(p0, p1, (col & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 14 * s, 0, 3.5f * s);

    ImVec2 t(p0.x + pad, p0.y + (h - textH) * 0.5f);
    DrawSegs(dl, g_fontBold, big, t, line1);
    t.y += big + 8 * s;
    DrawSegs(dl, g_fontUi, mid, t, line2);
    t.y += mid + 10 * s;
    DrawSegs(dl, g_fontUi, tiny, t, line3);

    const float tx = p0.x + pad + textW + sep, ty = p0.y + (h - tabH) * 0.5f;
    for (int r = 0; r < n; ++r) {
        const int str = n - 1 - r;
        const float y = ty + r * gap;
        const bool target = str == i;
        const ImU32 c = target ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (110u << 24));
        const char* name = kStringName[str];
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, name);
        dl->AddText(g_fontUi, tiny, ImVec2(tx, y - ns.y * 0.5f), c, name);
        dl->AddLine(ImVec2(tx + labelW, y), ImVec2(tx + labelW + tabW, y), c, target ? 4 * s : 2 * s);
    }
    char num[8];
    std::snprintf(num, sizeof(num), "%d", v.fret);
    const ImVec2 bc(tx + labelW + tabW * 0.5f, ty + (n - 1 - i) * gap);
    const float fs = 24 * s;
    const ImVec2 nsz = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, num);
    const float rad = std::max(nsz.x, nsz.y) * 0.5f + 7 * s;
    dl->AddCircleFilled(bc, rad, IM_COL32(14, 14, 20, 255));
    dl->AddCircle(bc, rad, col, 0, 3 * s);
    dl->AddText(g_fontBold, fs, ImVec2(bc.x - nsz.x * 0.5f, bc.y - nsz.y * 0.5f), kWhite, num);
}

// The "waiting" banner for a chord: its name, each string to play in its colour with its fret, and
// the chord shape as a tab (a bubble per played string, "x" = don't play that string).
void DrawChordBanner(ImDrawList* dl, const View& v, float s, ImVec2 ds) {
    const int n = v.bass ? 4 : 6;
    const ImU32 gold = IM_COL32(255, 206, 84, 255);
    const float big = 46 * s, mid = 26 * s, tiny = 20 * s;

    std::vector<int> notes;  // lowest string first
    for (int i = 0; i < n; ++i) if (v.frets[i] >= 0 && v.notes[i] >= 0) notes.push_back(v.notes[i]);
    const bool flats = music::UsesFlats(v.chordName);

    std::vector<Seg> line1;
    if (!v.chordName.empty()) line1 = {{"Play the chord  ", kWhite}, {v.chordName, gold}};
    else line1 = {{"Play these strings together", kWhite}};

    // What the chord is, in words: "B power chord  -  notes B and F#".
    const std::string meaning = music::ChordMeaning(v.chordName, notes);
    std::vector<Seg> lineM;
    if (!meaning.empty()) lineM.push_back({meaning, gold});
    if (!notes.empty()) lineM.push_back({(meaning.empty() ? "notes " : "   \xC2\xB7   notes ") + music::NoteList(notes, flats), kGrey});

    std::vector<Seg> line2;  // "RED open = E    YELLOW 2 = B ..." from the thickest string
    int played = 0;
    for (int i = 0; i < n; ++i) {
        if (v.frets[i] < 0) continue;
        if (played++) line2.push_back({"     ", kWhite});
        line2.push_back({kColorName[i], kStringColor[i]});
        line2.push_back({v.frets[i] == 0 ? " open" : " " + std::to_string(v.frets[i]), kWhite});
        if (v.notes[i] >= 0) line2.push_back({" = " + music::NoteName(v.notes[i], flats), kGrey});
    }
    const std::vector<Seg> line3 = {{played < n ? "x = don't play that string   " : "", kGrey}, {"F9 = skip   F8 = menu", kGrey}};

    const float textW = std::max({SegsWidth(g_fontBold, big, line1), SegsWidth(g_fontUi, mid, lineM),
                                  SegsWidth(g_fontUi, mid, line2), SegsWidth(g_fontUi, tiny, line3)});
    const float textH = big + 8 * s + (lineM.empty() ? 0 : mid + 8 * s) + mid + 10 * s + tiny;

    // Tab picture, thinnest string on top; wider string spacing than the single-note tab so a
    // bubble fits on every string.
    const float gap = 34 * s, tabW = 150 * s, labelW = 22 * s;
    const float tabH = gap * (n - 1);
    const float pad = 24 * s, sep = 34 * s;
    const float w = pad + textW + sep + labelW + tabW + pad;
    const float h = pad + std::max(textH, tabH + 30 * s) + pad;
    const ImVec2 p0(std::floor((ds.x - w) * 0.5f), std::floor(ds.y * 0.11f));
    const ImVec2 p1(p0.x + w, p0.y + h);

    const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.0f);
    dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 20, 222), 14 * s);
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
    DrawSegs(dl, g_fontUi, tiny, t, line3);

    const float tx = p0.x + pad + textW + sep, ty = p0.y + (h - tabH) * 0.5f;
    const float bx = tx + labelW + tabW * 0.5f, fs = 20 * s;
    for (int r = 0; r < n; ++r) {
        const int str = n - 1 - r;
        const float y = ty + r * gap;
        const bool on = v.frets[str] >= 0;
        const ImU32 c = on ? kStringColor[str] : ((kStringColor[str] & 0x00FFFFFF) | (90u << 24));
        const char* name = kStringName[str];
        const ImVec2 ns = g_fontUi->CalcTextSizeA(tiny, FLT_MAX, 0, name);
        dl->AddText(g_fontUi, tiny, ImVec2(tx, y - ns.y * 0.5f), c, name);
        dl->AddLine(ImVec2(tx + labelW, y), ImVec2(tx + labelW + tabW, y), c, on ? 4 * s : 2 * s);
        const std::string label = on ? std::to_string(v.frets[str]) : "x";
        const ImVec2 lsz = g_fontBold->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
        if (on) {
            const float rad = std::min(gap * 0.48f, std::max(lsz.x, lsz.y) * 0.5f + 4 * s);
            dl->AddCircleFilled(ImVec2(bx, y), rad, IM_COL32(14, 14, 20, 255));
            dl->AddCircle(ImVec2(bx, y), rad, kStringColor[str], 0, 3 * s);
        }
        dl->AddText(g_fontBold, fs, ImVec2(bx - lsz.x * 0.5f, y - lsz.y * 0.5f), on ? kWhite : kGrey, label.c_str());
    }
}

// The song clock, top-left: "1:23 / 4:28". Small and quiet, the game's HUD stays readable.
void DrawClock(ImDrawList* dl, const View& v, float s) {
    auto mmss = [](double t) {
        const int x = (int)std::max(0.0, t);
        char b[16];
        std::snprintf(b, sizeof(b), "%d:%02d", x / 60, x % 60);
        return std::string(b);
    };
    const std::string text = mmss(v.songTime) + (v.songLength > 0 ? "  /  " + mmss(v.songLength) : "");
    const float size = 26 * s, padX = 14 * s, padY = 6 * s;
    const ImVec2 ts = g_fontBold->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const ImVec2 p0(std::floor(24 * s), std::floor(24 * s));
    const ImVec2 p1(p0.x + ts.x + 2 * padX, p0.y + ts.y + 2 * padY);
    dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 20, 170), 8 * s);
    dl->AddText(g_fontBold, size, ImVec2(p0.x + padX, p0.y + padY), IM_COL32(235, 235, 240, 230), text.c_str());
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
    dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 20, (int)(215 * a)), 10 * s);
    dl->AddText(g_fontBold, size, ImVec2(p0.x + pad, p0.y + pad * 0.5f), IM_COL32(255, 255, 255, (int)(255 * a)), text.c_str());
}

void DrawMenu(const View& v, const Settings& st, float s, ImVec2 ds) {
    ImGui::PushFont(g_fontUi, 24 * s);
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700 * s, 0), ImGuiCond_Always);
    if (!g_menuWasOpen) ImGui::SetNextWindowFocus();
    bool open = true, skip = false;
    Settings e = st;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Note-by-Note", &open, flags)) {
        ImGui::Checkbox("Wait for each note", &e.enabled);
        ImGui::TextDisabled("The song stops at every note until you play it.");
        ImGui::BeginDisabled(!e.enabled);
        ImGui::Checkbox("Wait for chords too", &e.waitChords);
        ImGui::EndDisabled();
        ImGui::TextColored(v.chartOk ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImVec4(1.0f, 0.65f, 0.25f, 1), "%s", v.chartInfo.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(!v.waiting);
        if (ImGui::Button(v.chord ? "Skip this chord  (F9)" : "Skip this note  (F9)")) skip = true;
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Stop the song this long before the note reaches the line");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##lead", &e.leadMs, 0, 500, "%d ms");
        ImGui::TextUnformatted("A right note played this early still counts");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##early", &e.earlyMs, 0, 1000, "%d ms");
        ImGui::Checkbox("Also accept the same note one octave higher or lower", &e.acceptOctaves);
        ImGui::Checkbox("Show what to play while the song waits", &e.showBanner);
        ImGui::Checkbox("Show the song time (top-left corner)", &e.showClock);

        ImGui::Separator();
        ImGui::TextDisabled("Changes are saved automatically. The song is held while this menu is open.");
        if (ImGui::Button("Close  (F8 / Esc)")) open = false;
        ImGui::PushFont(nullptr, 17 * s);
        ImGui::TextDisabled("Thanks to RS_ASIO, Rocksmith2014.NET, MinHook and Dear ImGui.");
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopFont();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open = false;

    std::lock_guard<std::mutex> lk(g.m);
    if (!Same(e, g.settings)) g.settings = e;
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
    ImGui::NewFrame();

    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under the menu window
    if (v.inSong && v.waiting && st.enabled && st.showBanner) {
        if (v.chord) DrawChordBanner(dl, v, s, io.DisplaySize);
        else DrawBanner(dl, v, s, io.DisplaySize);
    }
    if (v.inSong && st.showClock && v.songTime >= 0) DrawClock(dl, v, s);
    DrawToast(dl, toast, toastStart, toastUntil, s, io.DisplaySize);
    if (menu) DrawMenu(v, st, s, io.DisplaySize);
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
