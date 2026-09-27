// crashfix.cpp: see crashfix.h.
#include "crashfix.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "log.h"

namespace nbn::crashfix {
namespace {

// NTSTATUS NtProtectVirtualMemory(HANDLE process, PVOID* base, SIZE_T* size, ULONG newProtect, ULONG* oldProtect)
using NtProtectFn = LONG(NTAPI*)(HANDLE, void**, SIZE_T*, ULONG, ULONG*);

uint8_t* g_fn = nullptr;       // ntdll!NtProtectVirtualMemory in memory
uint8_t g_orig[5] = {};        // its original first instruction (mov eax, <system call number>)
NtProtectFn g_direct = nullptr;  // our own stub: the original instruction + jmp to the rest of the function
bool g_on = false;
int g_repairs = 0;

// The first 16 bytes of an ntdll export as they are in ntdll.dll on disk, with the one absolute
// address in them (mov edx, offset Wow64SystemServiceCall at +6) moved to where ntdll is loaded.
bool ReadFromDisk(HMODULE ntdll, const uint8_t* fn, uint8_t out[16]) {
    wchar_t path[MAX_PATH];
    // "C:\Windows\SYSTEM32\ntdll.dll": a 32-bit process reads SysWOW64 through that name.
    if (!GetModuleFileNameW(ntdll, path, MAX_PATH)) return false;
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 0x400) return false;
    const auto* dos = (const IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > file.size()) return false;
    const auto* nt = (const IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    // The section that holds the function: file offset = RVA - section RVA + section file offset.
    const uint32_t rva = (uint32_t)(fn - (const uint8_t*)ntdll);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (rva < sec->VirtualAddress || rva + 16 > sec->VirtualAddress + sec->SizeOfRawData) continue;
        const size_t off = rva - sec->VirtualAddress + sec->PointerToRawData;
        if (off + 16 > file.size()) return false;
        std::memcpy(out, file.data() + off, 16);
        if (out[5] == 0xBA) {  // mov edx, imm32: relocate the address like the loader did
            uint32_t a;
            std::memcpy(&a, out + 6, 4);
            a += (uint32_t)((uintptr_t)ntdll - nt->OptionalHeader.ImageBase);
            std::memcpy(out + 6, &a, 4);
        }
        return true;
    }
    return false;
}

// Puts g_orig back over the jmp. The page is unlocked through our stub (straight to Windows, not
// through the protector's handler), and the 5 bytes are written with one atomic 8-byte write, so a
// thread calling the function at that moment sees either the whole jmp or the whole original.
bool Repair() {
    void* page = g_fn;
    SIZE_T size = 8;
    ULONG old = 0;
    if (g_direct(GetCurrentProcess(), &page, &size, PAGE_EXECUTE_READWRITE, &old) < 0) return false;
    auto* p64 = (volatile LONG64*)g_fn;
    for (;;) {
        const LONG64 cur = *p64;
        LONG64 fixed = cur;
        std::memcpy(&fixed, g_orig, 5);
        if (InterlockedCompareExchange64(p64, fixed, cur) == cur) break;
    }
    page = g_fn;
    size = 8;
    ULONG unused = 0;
    g_direct(GetCurrentProcess(), &page, &size, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), g_fn, 8);
    return std::memcmp(g_fn, g_orig, 5) == 0;
}

}  // namespace

void Start(bool on) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_fn = ntdll ? (uint8_t*)GetProcAddress(ntdll, "NtProtectVirtualMemory") : nullptr;
    if (!g_fn) { Log("crash fix: NtProtectVirtualMemory not found"); return; }
    if (g_fn[0] != 0xE9) { Log("crash fix: NtProtectVirtualMemory is not redirected, nothing to do"); g_fn = nullptr; return; }
    const uint8_t* target = g_fn + 5 + *(const int32_t*)(g_fn + 1);
    if (!on) { Log("crash fix: off (NtProtectVirtualMemory is redirected to %p)", target); g_fn = nullptr; return; }

    // Only a plain "mov eax, n" whose following bytes are exactly ntdll's: anything else is not the
    // hook we know, so we leave it alone.
    uint8_t disk[16];
    if (!ReadFromDisk(ntdll, g_fn, disk) || disk[0] != 0xB8 || std::memcmp(disk + 5, g_fn + 5, 11) != 0 ||
        ((uintptr_t)g_fn & 7) != 0) {
        Log("crash fix: NtProtectVirtualMemory doesn't look as expected, not touched");
        g_fn = nullptr;
        return;
    }
    std::memcpy(g_orig, disk, 5);

    // Stub: mov eax, n ; jmp NtProtectVirtualMemory+5
    auto* stub = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) { Log("crash fix: VirtualAlloc failed"); g_fn = nullptr; return; }
    std::memcpy(stub, g_orig, 5);
    stub[5] = 0xE9;
    const int32_t rel = (int32_t)((g_fn + 5) - (stub + 10));
    std::memcpy(stub + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    g_direct = (NtProtectFn)stub;

    g_on = true;
    const bool ok = Repair();
    if (ok) ++g_repairs;
    Log("crash fix: NtProtectVirtualMemory redirect (to %p) %s", target, ok ? "removed, original instruction back" : "could NOT be removed");
}

void Tick() {
    if (!g_on || g_fn[0] != 0xE9) return;
    // The protector put its hook back (not seen so far). Repair again, but not forever.
    if (g_repairs >= 10) return;
    const bool ok = Repair();
    ++g_repairs;
    Log("crash fix: the redirect came back; removed again (%s, %d times)", ok ? "ok" : "failed", g_repairs);
}

}  // namespace nbn::crashfix
