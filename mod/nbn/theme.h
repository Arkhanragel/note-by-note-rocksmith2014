// theme.h: the overlay's colours ("skins"). The player picks a ready-made theme in the F8 menu and
// can change any single colour of it with a colour picker (or a hex code in NoteByNote.ini).
//
// The string colours are NOT part of a theme: they are the game's highway colours (red, yellow,
// blue...), and the banner's words ("the ORANGE string") must keep matching what the highway shows.
//
// Colours are plain 0xRRGGBB numbers (no ImGui here, so main.cpp can read/write them). How see-through
// each thing is stays with the drawing code (a faint beat line and a solid bar line use the same
// "Grid" colour), and the tab's background strength stays the TabBackground setting.
#pragma once
#include <cstdint>
#include <string>

namespace nbn::theme {

// The colours a theme sets. Keep the order: settings and the ini refer to them by position/key.
enum Slot {
    kPanel,      // backgrounds: banner, clock, tab, messages, the boxes behind fret numbers
    kText,       // main text and fret numbers
    kTextDim,    // quieter text: note names, key hints, "x8" after a repeated fret
    kChord,      // chords: the chord banner's frame and name, chord names and brackets in the tab
    kWarning,    // the "!" in front of the "how to fix it" line
    kHighlight,  // the tab's "now" line and the frame around the next note
    kGrid,       // tab bar lines, beat lines, the shade of every other bar, the line between two rows
    kRhythm,     // stems, beams and triplet "3" under the tab, and the bar numbers
    kMenu,       // the menu's accents: title bar, tabs, ticked boxes, sliders, buttons
    kSlots
};

struct SlotInfo {
    const char* key;    // ini key, e.g. "ColorPanel"
    const char* label;  // menu text
};
extern const SlotInfo kSlotInfo[kSlots];

struct Theme {
    const char* name;      // shown in the menu and saved in the ini (Theme=...)
    uint32_t color[kSlots];  // 0xRRGGBB
};
extern const Theme kThemes[];
extern const int kThemeCount;

// Index of the theme with this name (not case sensitive), 0 (Default) if none.
int FindTheme(const std::string& name);

// "#RRGGBB" <-> 0xRRGGBB. Parse accepts it with or without the '#', returns -1 if it isn't a colour.
std::string ToHex(uint32_t rgb);
int ParseHex(const std::string& s);

}  // namespace nbn::theme
