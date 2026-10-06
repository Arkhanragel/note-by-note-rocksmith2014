// keys.h: the two keys of the mod (the menu and "skip this note"): reading them from the ini, their
// names on screen, and the one-time move from the old defaults.
//
// Until 0.3.1 the keys were F8 (menu) and F9 (skip). RSModsPlus uses F8 / F9 / F10 for its drop pedal
// (fixed keys; 3.x: F7, F9, F10), so one press did two things. From 0.3.2 they are F5 and F6, free in
// RSMods and in both RSModsPlus lines.
//
// Plain functions on an ini file, no game state (tested by nbn_keys_test).
#pragma once
#include <string>

namespace nbn::keys {

constexpr int kDefaultMenu = 0x74;  // VK_F5
constexpr int kDefaultSkip = 0x75;  // VK_F6

// A key as written in the ini -> its virtual-key code: "F1".."F24", or a code as "0x4D". Anything
// else gives `def`.
int Parse(const std::wstring& text, int def);

// A key's name for the player: "F5" for a function key, the code for any other ("key 0x4D").
std::string Name(int vk);

// Moves an ini written by 0.3.1 or older to the new keys, once: MenuKey F8 -> F5 and SkipKey F9 -> F6.
// Only a key still on its old default moves (a key the player chose stays), and `KeysVersion=2` marks
// the file as done, so F8 or F9 chosen on purpose afterwards is kept. A file that doesn't exist is
// left alone (the first run writes one with the new keys).
void MigrateIni(const std::wstring& iniPath);

// The menu key and the skip key of the ini (after MigrateIni). A very old ini has ToggleKey instead of
// MenuKey (the key used to switch the mode directly): it still counts.
int MenuKey(const std::wstring& iniPath);
int SkipKey(const std::wstring& iniPath);

}  // namespace nbn::keys
