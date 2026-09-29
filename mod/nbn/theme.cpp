// theme.cpp: the built-in themes (see theme.h).
#include "theme.h"

#include <cctype>
#include <cstdio>

namespace nbn::theme {

const SlotInfo kSlotInfo[kSlots] = {
    {"ColorPanel", "Background"},
    {"ColorText", "Text and fret numbers"},
    {"ColorTextDim", "Quiet text (note names, key hints)"},
    {"ColorChord", "Chords"},
    {"ColorWarning", "Warning mark (!)"},
    {"ColorHighlight", "Tab cursor and next note"},
    {"ColorGrid", "Bar and beat lines"},
    {"ColorRhythm", "Rhythm and bar numbers"},
    {"ColorMenu", "Menu accents"},
};

// Order of each row: Panel, Text, TextDim, Chord, Warning, Highlight, Grid, Rhythm, Menu.
// "Default" is the look the overlay always had; the others must stay readable over the game with
// every string colour (the fret boxes are drawn in the Panel colour with the Text colour on top).
const Theme kThemes[] = {
    {"Default", {0x0E0E14, 0xFFFFFF, 0xAFAFB9, 0xFFCE54, 0xFF6E5A, 0xFFFFFF, 0xFFFFFF, 0xDCDCE6, 0x8C1F1F}},
    // Pure black and white, brighter quiet text: for small screens or a TV far away.
    {"High contrast", {0x000000, 0xFFFFFF, 0xE6E6E6, 0xFFDD00, 0xFF5A3C, 0x00E5FF, 0xFFFFFF, 0xFFFFFF, 0x3A3A3A}},
    // Dark blue panels with soft blue lines.
    {"Midnight", {0x0A1224, 0xEBF0FF, 0x8CA0BE, 0xFFC85A, 0xFF7864, 0x78C8FF, 0x96BEFF, 0xB4C8EB, 0x1E468C}},
    // Warm dark brown, like an old amp.
    {"Vintage", {0x1C140E, 0xFFF4E0, 0xC0AA8C, 0xFFB43C, 0xFF6E46, 0xFFE6B4, 0xFFDCA0, 0xE6D2B4, 0x6E3C14}},
    // Light panels with dark text (set the tab background to 90-100 % so the game doesn't show through).
    {"Paper", {0xF5F3EB, 0x141418, 0x5A5A64, 0xA86E00, 0xC82814, 0x000000, 0x000000, 0x1E1E28, 0x3C5A96}},
};
const int kThemeCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));

int FindTheme(const std::string& name) {
    for (int i = 0; i < kThemeCount; ++i) {
        const char* a = kThemes[i].name;
        size_t j = 0;
        while (a[j] && j < name.size() && std::tolower((unsigned char)a[j]) == std::tolower((unsigned char)name[j])) ++j;
        if (!a[j] && j == name.size()) return i;
    }
    return 0;
}

std::string ToHex(uint32_t rgb) {
    char b[8];
    std::snprintf(b, sizeof(b), "#%06X", (unsigned)(rgb & 0xFFFFFF));
    return b;
}

int ParseHex(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    if (i < s.size() && s[i] == '#') ++i;
    int v = 0, digits = 0;
    for (; i < s.size() && std::isxdigit((unsigned char)s[i]); ++i, ++digits)
        v = v * 16 + (std::isdigit((unsigned char)s[i]) ? s[i] - '0' : std::tolower((unsigned char)s[i]) - 'a' + 10);
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return digits == 6 && i == s.size() ? v : -1;
}

}  // namespace nbn::theme
