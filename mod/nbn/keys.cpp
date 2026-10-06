// keys.cpp: see keys.h.
#include "keys.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace nbn::keys {

namespace {

const wchar_t* const kSection = L"NoteByNote";

std::wstring Read(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[32];
    GetPrivateProfileStringW(kSection, key, def, buf, 32, ini.c_str());
    return buf;
}

}  // namespace

int Parse(const std::wstring& text, int def) {
    if (text.size() >= 2 && (text[0] == L'F' || text[0] == L'f')) {
        const int n = _wtoi(text.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (text.rfind(L"0x", 0) == 0) return (int)wcstol(text.c_str(), nullptr, 16);
    return def;
}

std::string Name(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    char code[16];
    snprintf(code, sizeof code, "key 0x%02X", vk & 0xFF);
    return code;
}

void MigrateIni(const std::wstring& iniPath) {
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetPrivateProfileIntW(kSection, L"KeysVersion", 1, iniPath.c_str()) >= 2) return;
    // A key that isn't in the file was on its default too (the default is what the mod used).
    auto moveKey = [&](const wchar_t* name, const wchar_t* was, const wchar_t* now) {
        if (_wcsicmp(Read(iniPath, name, was).c_str(), was) == 0)
            WritePrivateProfileStringW(kSection, name, now, iniPath.c_str());
    };
    // An ini older than MenuKey has ToggleKey: a player who changed THAT chose their key, so MenuKey is
    // not written (MenuKey would win over it).
    if (_wcsicmp(Read(iniPath, L"ToggleKey", L"F8").c_str(), L"F8") == 0) moveKey(L"MenuKey", L"F8", L"F5");
    moveKey(L"SkipKey", L"F9", L"F6");
    WritePrivateProfileStringW(kSection, L"KeysVersion", L"2", iniPath.c_str());
}

int MenuKey(const std::wstring& iniPath) {
    return Parse(Read(iniPath, L"MenuKey", Read(iniPath, L"ToggleKey", L"F5").c_str()), kDefaultMenu);
}

int SkipKey(const std::wstring& iniPath) { return Parse(Read(iniPath, L"SkipKey", L"F6"), kDefaultSkip); }

}  // namespace nbn::keys
