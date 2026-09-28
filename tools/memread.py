"""memread.py: read Rocksmith2014.exe memory from OUTSIDE the game (dev tool, like Cheat Engine).

Uses OpenProcess + ReadProcessMemory, so nothing is injected. All addresses are RVAs of the exe
(see BITACORA.md). Run with the 32-bit or 64-bit Python in .venv:

    python tools/memread.py song        # song object + song data header, while a song is playing
    python tools/memread.py hex <addr> <n>
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8 | 0x10  # modules, incl. 32-bit ones

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class Game:
    def __init__(self, exe="Rocksmith2014.exe"):
        self.pid = self._find_pid(exe)
        self.base = self._find_base(exe)
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, self.pid)
        if not self.h:
            raise OSError("OpenProcess failed: %d" % ctypes.get_last_error())

    @staticmethod
    def _find_pid(exe):
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.lower() == exe.lower():
                k32.CloseHandle(snap)
                return e.th32ProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
        k32.CloseHandle(snap)
        raise SystemExit("the game isn't running")

    def _find_base(self, exe):
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, self.pid)
        e = MODULEENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Module32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szModule.lower() == exe.lower():
                k32.CloseHandle(snap)
                return e.modBaseAddr
            ok = k32.Module32NextW(snap, ctypes.byref(e))
        k32.CloseHandle(snap)
        raise SystemExit("module not found")

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            raise OSError("read 0x%X failed" % addr)
        return buf.raw

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def f32(self, addr):
        return struct.unpack("<f", self.read(addr, 4))[0]

    def song(self):
        return self.u32(self.u32(self.base + 0xF6062C) + 0xB0)


def hexdump(data, addr):
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        words = struct.unpack("<%dI" % (len(row) // 4), row[:len(row) // 4 * 4])
        floats = " ".join(("%9.3f" % f if 1e-3 < abs(f) < 1e6 else "        .") for f in struct.unpack("<%df" % len(words), row[:len(words) * 4]))
        print("%08X +%03X  %s   %s" % (addr + i, i, " ".join("%08X" % w for w in words), floats))


if __name__ == "__main__":
    g = Game()
    print("pid %d base 0x%08X" % (g.pid, g.base))
    cmd = sys.argv[1] if len(sys.argv) > 1 else "song"
    if cmd == "hex":
        a, n = int(sys.argv[2], 16), int(sys.argv[3], 0)
        hexdump(g.read(a, n), a)
    elif cmd == "song":
        s = g.song()
        print("song 0x%08X  clock %.3f" % (s, g.f32(s + 0x3B4)))
        d = g.u32(s + 0x78)
        print("song data 0x%08X" % d)
        hexdump(g.read(d, 0x140), d)
