// crashfix.h: works around the game's own random crash / hang (not caused by the mod).
//
// The game's copy protection hooks ntdll!NtProtectVirtualMemory (the function behind every
// VirtualProtect, also used by the Windows heap): its first instruction is replaced by a jmp to a
// trampoline, which jumps to a handler inside the game exe's protected code (.UBX0). Now and then
// that handler "returns" to a garbage address (a different one every time, so probably a race
// between threads): the game crashes, or hangs if it happens while Windows holds the loader's
// function-table lock (seen while the profile loads). All the game crash dumps show exactly this.
//
// The fix puts ntdll's original first instruction back, so NtProtectVirtualMemory goes straight to
// Windows again. The original bytes come from ntdll.dll on disk, and the rest of the function must
// match the file, or nothing is touched. The protector's handler only guards memory protection
// changes; the game doesn't need it to run.
#pragma once

namespace nbn::crashfix {

// Repairs the hook if it is there (call as early as possible; logs what it did). on = false: only
// logs whether the hook is there.
void Start(bool on);

// Call now and then (every few seconds): logs (once) if the protector puts its hook back, and
// repairs it again.
void Tick();

}  // namespace nbn::crashfix
