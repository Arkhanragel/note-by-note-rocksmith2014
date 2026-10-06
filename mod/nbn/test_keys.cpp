// test_keys.cpp: the menu and skip keys (keys.h): reading them, their names, and the one-time move of
// an older settings file from F8 / F9 to F5 / F6.
//
// The move changes players' files, so every kind of file a player can have is tried here, on real ini
// files in the temp folder. Prints "all passed" or the failures.
#include <windows.h>

#include <cstdio>
#include <string>

#include "keys.h"

namespace {

using namespace nbn;

int g_fails = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fails;
    }
}

// A settings file with this text, in the temp folder. Removed by the destructor.
struct TempIni {
    std::wstring path;

    explicit TempIni(const char* text) {
        wchar_t dir[MAX_PATH], file[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        GetTempFileNameW(dir, L"nbk", 0, file);
        path = file;
        FILE* f = _wfopen(path.c_str(), L"w");
        if (f) {
            std::fputs(text, f);
            std::fclose(f);
        }
    }
    ~TempIni() { DeleteFileW(path.c_str()); }
    TempIni(const TempIni&) = delete;
    TempIni& operator=(const TempIni&) = delete;

    // The value of a key as text, "(none)" when the file doesn't have it.
    std::wstring Get(const wchar_t* key) const {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"NoteByNote", key, L"(none)", buf, 64, path.c_str());
        return buf;
    }
    // The whole file, to see that nothing else changed.
    std::string Text() const {
        std::string s;
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return s;
        char buf[512];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
        std::fclose(f);
        return s;
    }
};

void TestParseAndName() {
    Check(keys::Parse(L"F5", 0) == VK_F5 && keys::Parse(L"f6", 0) == VK_F6, "F5 and f6 are read");
    Check(keys::Parse(L"F1", 0) == VK_F1 && keys::Parse(L"F24", 0) == VK_F24, "F1 and F24 are read");
    Check(keys::Parse(L"F0", 7) == 7 && keys::Parse(L"F25", 7) == 7, "F0 and F25 are not keys: the default");
    Check(keys::Parse(L"0x4D", 0) == 0x4D, "a key code is read");
    Check(keys::Parse(L"", 7) == 7 && keys::Parse(L"M", 7) == 7 && keys::Parse(L"banana", 7) == 7,
          "anything else gives the default");
    Check(keys::Name(VK_F5) == "F5" && keys::Name(VK_F6) == "F6" && keys::Name(VK_F12) == "F12", "function keys' names");
    Check(keys::Name(0x4D) == "key 0x4D", "another key is named by its code");
    Check(keys::Parse(L"F5", 0) == keys::kDefaultMenu && keys::Parse(L"F6", 0) == keys::kDefaultSkip, "the defaults are F5 and F6");
}

void TestMigrate() {
    // The file every 0.3.1 player has: both keys on the old defaults.
    {
        TempIni ini("; settings\n[NoteByNote]\nEnabled=1\n; the menu key\nMenuKey=F8\n; skips\nSkipKey=F9\nLeadMs=30\n");
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"F5" && ini.Get(L"SkipKey") == L"F6", "old defaults move to F5 / F6");
        Check(ini.Get(L"KeysVersion") == L"2", "the file is marked as moved");
        Check(ini.Get(L"Enabled") == L"1" && ini.Get(L"LeadMs") == L"30", "the other settings are untouched");
        Check(ini.Text().find("; the menu key") != std::string::npos, "the comments stay");
        Check(keys::MenuKey(ini.path) == VK_F5 && keys::SkipKey(ini.path) == VK_F6, "and the mod reads F5 / F6");
        // The player then puts F8 back on purpose: the next start keeps it.
        WritePrivateProfileStringW(L"NoteByNote", L"MenuKey", L"F8", ini.path.c_str());
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"F8" && keys::MenuKey(ini.path) == VK_F8, "F8 chosen after the move is kept");
    }
    // A player who had chosen another menu key: it stays; the skip key was the default, it moves.
    {
        TempIni ini("[NoteByNote]\nMenuKey=F3\nSkipKey=F9\n");
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"F3" && ini.Get(L"SkipKey") == L"F6", "a chosen menu key stays, the default skip key moves");
    }
    // ... and the other way round, with lower-case text.
    {
        TempIni ini("[NoteByNote]\nMenuKey=f8\nSkipKey=F2\n");
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"F5" && ini.Get(L"SkipKey") == L"F2", "f8 (lower case) moves, a chosen skip key stays");
    }
    // A file with no key lines at all (the mod was using its defaults, F8 / F9).
    {
        TempIni ini("[NoteByNote]\nEnabled=0\n");
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"F5" && ini.Get(L"SkipKey") == L"F6", "a file without the keys gets F5 / F6");
        Check(ini.Get(L"Enabled") == L"0", "and keeps its other settings");
    }
    // A very old file: ToggleKey instead of MenuKey. Still on F8: the menu key becomes F5.
    {
        TempIni ini("[NoteByNote]\nToggleKey=F8\n");
        keys::MigrateIni(ini.path);
        Check(keys::MenuKey(ini.path) == VK_F5, "an old ToggleKey=F8 file: the menu is on F5");
    }
    // ... changed by the player: their key stays the menu key.
    {
        TempIni ini("[NoteByNote]\nToggleKey=F2\n");
        keys::MigrateIni(ini.path);
        Check(ini.Get(L"MenuKey") == L"(none)" && keys::MenuKey(ini.path) == VK_F2, "a chosen ToggleKey stays the menu key");
        Check(ini.Get(L"SkipKey") == L"F6", "and its skip key moves");
    }
    // A new install (the first-run file already has the new keys and the mark): nothing changes.
    {
        TempIni ini("[NoteByNote]\nMenuKey=F5\nSkipKey=F6\nKeysVersion=2\n");
        const std::string before = ini.Text();
        keys::MigrateIni(ini.path);
        Check(ini.Text() == before, "a new install's file is not rewritten");
    }
    // Running it twice gives the same file as once.
    {
        TempIni ini("[NoteByNote]\nMenuKey=F8\nSkipKey=F9\n");
        keys::MigrateIni(ini.path);
        const std::string once = ini.Text();
        keys::MigrateIni(ini.path);
        Check(ini.Text() == once, "a second run changes nothing");
    }
    // No file: none is created (the first run writes the real one).
    {
        std::wstring missing;
        {
            TempIni ini("");
            missing = ini.path;
        }
        keys::MigrateIni(missing);
        Check(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES, "a missing file is not created");
        Check(keys::MenuKey(missing) == VK_F5 && keys::SkipKey(missing) == VK_F6, "without a file the keys are F5 / F6");
    }
}

}  // namespace

int main() {
    TestParseAndName();
    TestMigrate();
    if (g_fails == 0) std::printf("all passed\n");
    return g_fails ? 1 : 0;
}
