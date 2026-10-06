// crashlog.cpp: see crashlog.h.
#include "crashlog.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace nbn::crashlog {
namespace {

// Each (code, address) pair is logged once; after kMaxSeen different ones nothing more is logged.
constexpr int kMaxSeen = 48;
struct Seen {
    DWORD code = 0;
    uintptr_t address = 0;
};
Seen g_seen[kMaxSeen];
int g_seenCount = 0;
volatile LONG g_lock = 0;  // spin lock for g_seen (a handler can run on any thread)

uintptr_t g_self = 0, g_selfEnd = 0;  // our own DLL: its guarded memory reads fault on purpose
uintptr_t g_exe = 0;

bool Serious(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            return true;
        default:
            // Not EXCEPTION_STACK_OVERFLOW: there is no stack left to write a log line with.
            return false;
    }
}

// True the first time this pair is seen (and there is room to remember it).
bool FirstTime(DWORD code, uintptr_t address) {
    while (InterlockedExchange(&g_lock, 1)) YieldProcessor();
    bool first = g_seenCount < kMaxSeen;
    for (int i = 0; first && i < g_seenCount; ++i)
        if (g_seen[i].code == code && g_seen[i].address == address) first = false;
    if (first) g_seen[g_seenCount++] = {code, address};
    InterlockedExchange(&g_lock, 0);
    return first;
}

// The module an address is in: its base, and its name as far as it can be told without asking the
// Windows loader (which takes a lock the crashing thread may already hold): the game exe, or the
// name a DLL carries in its own export table.
void Describe(uintptr_t address, char* out, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)address, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        std::snprintf(out, size, "no module");
        return;
    }
    const uintptr_t base = (uintptr_t)mbi.AllocationBase;
    char name[64] = "";
    if (base == g_exe) {
        std::strcpy(name, "game exe");
    } else if (mbi.Type == MEM_IMAGE) {
        __try {
            const auto* dos = (const IMAGE_DOS_HEADER*)base;
            const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE && dir.VirtualAddress) {
                const auto* exp = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
                const char* n = (const char*)(base + exp->Name);
                size_t i = 0;
                for (; i + 1 < sizeof(name) && n[i] >= 0x20 && n[i] < 0x7F; ++i) name[i] = n[i];
                name[i] = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; }
    }
    std::snprintf(out, size, "%s+0x%X (base 0x%08X%s)", name[0] ? name : "module", (unsigned)(address - base), (unsigned)base,
                  mbi.Type == MEM_IMAGE ? "" : ", not a loaded file");
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const uintptr_t at = (uintptr_t)rec->ExceptionAddress;
    if (!Serious(rec->ExceptionCode) || (at >= g_self && at < g_selfEnd) || !FirstTime(rec->ExceptionCode, at))
        return EXCEPTION_CONTINUE_SEARCH;

    char where[160], what[64] = "";
    Describe(at, where, sizeof(where));
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        std::snprintf(what, sizeof(what), " (%s 0x%08X)", kind == 0 ? "reading" : kind == 1 ? "writing" : "running",
                      (unsigned)rec->ExceptionInformation[1]);
    }
    Log("exception 0x%08lX%s at 0x%08X = %s, thread %lu (harmless if the log goes on)", rec->ExceptionCode, what,
        (unsigned)at, where, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

void* g_registration = nullptr;  // what AddVectoredExceptionHandler returned, for Stop

}  // namespace

void Start() {
    if (g_self) return;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)&Handler, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return;
    g_self = (uintptr_t)mbi.AllocationBase;
    const auto* dos = (const IMAGE_DOS_HEADER*)g_self;
    g_selfEnd = g_self + ((const IMAGE_NT_HEADERS*)(g_self + dos->e_lfanew))->OptionalHeader.SizeOfImage;
    g_exe = (uintptr_t)GetModuleHandleW(nullptr);
    // 0 = after the handlers already registered: ours only looks.
    g_registration = AddVectoredExceptionHandler(0, Handler);
    const bool ok = g_registration != nullptr;
    Log("crash log: %s (serious exceptions are written here, once each)", ok ? "on" : "could not be installed");
}

void Stop() {
    if (g_registration) RemoveVectoredExceptionHandler(g_registration);
    g_registration = nullptr;
}

}  // namespace nbn::crashlog
