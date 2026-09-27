// fastintro.cpp: see fastintro.h.
#include "fastintro.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "MinHook.h"
#include "log.h"

namespace nbn::fastintro {
namespace {

// fake = anchorFake + (real - anchorReal) * speed. Written only by the main loop (twice: start and
// end), read by every thread that asks for the time, so it is a "seqlock": the writer makes seq odd
// while it changes the fields, readers retry if seq was odd or changed while they read.
struct Clock {
    std::atomic<uint32_t> seq{0};
    int64_t anchorReal = 0, anchorFake = 0;
    int64_t speed = 1;

    int64_t Fake(int64_t real) const {
        for (;;) {
            const uint32_t s = seq.load(std::memory_order_acquire);
            if (s & 1) continue;
            const int64_t r = anchorReal, f = anchorFake, k = speed;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s) return f + (real - r) * k;
        }
    }
    // From now on: speed k, continuing from the current fake time (never jumps).
    void Set(int64_t real, int64_t k) {
        const int64_t f = Fake(real);
        seq.fetch_add(1, std::memory_order_acq_rel);
        anchorReal = real;
        anchorFake = f;
        speed = k;
        seq.fetch_add(1, std::memory_order_release);
    }
};

Clock g_qpc, g_tgt;                    // QueryPerformanceCounter (counts), timeGetTime (ms)
std::atomic<long> g_qpcCalls{0}, g_tgtCalls{0};
bool g_installed = false, g_fast = false;
DWORD g_startTick = 0;
void* g_qpcAddr = nullptr;
void* g_tgtAddr = nullptr;

using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using TgtFn = DWORD(WINAPI*)();
QpcFn oQpc = nullptr;
TgtFn oTgt = nullptr;

BOOL WINAPI HkQpc(LARGE_INTEGER* c) {
    const BOOL ok = oQpc(c);
    if (ok && c) {
        g_qpcCalls.fetch_add(1, std::memory_order_relaxed);
        c->QuadPart = g_qpc.Fake(c->QuadPart);
    }
    return ok;
}

DWORD WINAPI HkTgt() {
    g_tgtCalls.fetch_add(1, std::memory_order_relaxed);
    // 32-bit milliseconds that wrap every 49 days: do the maths in 32 bits too.
    return (DWORD)g_tgt.Fake((int64_t)oTgt()) ;
}

int64_t RealQpc() {
    LARGE_INTEGER c{};
    (oQpc ? oQpc : QueryPerformanceCounter)(&c);
    return c.QuadPart;
}

bool Hook(void* target, void* detour, void** original, const char* name) {
    const MH_STATUS a = MH_CreateHook(target, detour, original);
    const MH_STATUS b = a == MH_OK ? MH_EnableHook(target) : a;
    if (b != MH_OK) Log("fast intro: can't hook %s: %s", name, MH_StatusToString(b));
    return b == MH_OK;
}

}  // namespace

void Start(int speed) {
    if (speed <= 1) { Log("fast intro: off"); return; }
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { Log("fast intro: MinHook: %s", MH_StatusToString(st)); return; }
    // The lowest level function of each clock, so every way of asking for the time goes through it
    // (kernel32's QueryPerformanceCounter jumps to ntdll's RtlQueryPerformanceCounter).
    g_qpcAddr = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlQueryPerformanceCounter");
    HMODULE winmm = LoadLibraryW(L"winmm.dll");  // already loaded by the game; keeps it loaded for us
    g_tgtAddr = winmm ? (void*)GetProcAddress(winmm, "timeGetTime") : nullptr;
    // Anchors before the hooks go live (speed 1 = the real time until Set below).
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    g_qpc.anchorReal = g_qpc.anchorFake = c.QuadPart;
    if (g_tgtAddr) g_tgt.anchorReal = g_tgt.anchorFake = ((TgtFn)g_tgtAddr)();
    const bool q = g_qpcAddr && Hook(g_qpcAddr, (void*)&HkQpc, (void**)&oQpc, "RtlQueryPerformanceCounter");
    const bool t = g_tgtAddr && Hook(g_tgtAddr, (void*)&HkTgt, (void**)&oTgt, "timeGetTime");
    g_installed = q || t;
    if (!g_installed) return;
    if (q) g_qpc.Set(RealQpc(), speed);
    if (t) g_tgt.Set((int64_t)oTgt(), speed);
    g_fast = true;
    g_startTick = GetTickCount();
    Log("fast intro: clocks x%d (QPC %s, timeGetTime %s)", speed, q ? "ok" : "no", t ? "ok" : "no");
}

void Tick(bool titleOrLater) {
    if (!g_fast) return;
    const DWORD now = GetTickCount();
    // 60 real seconds is far more than the intro needs even at x2: a safety net.
    if (!titleOrLater && now - g_startTick < 60000) return;
    if (oQpc) g_qpc.Set(RealQpc(), 1);
    if (oTgt) g_tgt.Set((int64_t)oTgt(), 1);
    g_fast = false;
    Log("fast intro: back to normal speed after %.1f s%s (calls: QPC %ld, timeGetTime %ld)", (now - g_startTick) / 1000.0,
        titleOrLater ? "" : " (timeout)", g_qpcCalls.load(), g_tgtCalls.load());
}

void Stop() {
    if (!g_installed) return;
    // The clocks jump back to the real time here (only when the mod is unloaded during development).
    if (g_qpcAddr) { MH_DisableHook(g_qpcAddr); MH_RemoveHook(g_qpcAddr); }
    if (g_tgtAddr) { MH_DisableHook(g_tgtAddr); MH_RemoveHook(g_tgtAddr); }
    g_installed = g_fast = false;
}

}  // namespace nbn::fastintro
