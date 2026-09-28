"""Makes byte patterns ("signatures") for the game addresses Note-by-Note uses, from a memory dump.

The mod finds these addresses by pattern at start-up (mod/nbn/signatures.h), so it can work on game
builds we don't have (the older Remastered exe) as long as the code around them is the same.

Input: a memory image of the running game (the whole module, file offset = RVA; by default
mod/build/Release/run/Rocksmith2014_mem.bin). Never commit it.

What is blanked out ("??") in a pattern, because it changes between builds or load addresses:
  - the target of relative calls/jumps (E8/E9 rel32, 0F 8x rel32),
  - every 4-byte value that points inside the image (absolute addresses: relocated by ASLR, and
    different in another build).
Two kinds of target:
  func    the pattern starts at the function; the match IS the address.
  ref     a place in the code that uses the address (a global variable, a vtable); the pattern
          starts at that instruction and the mod reads the address out of the match (capture).
Each pattern grows one instruction at a time until it matches exactly once in the executable
sections (the same sections the mod scans). For `ref` targets up to 3 patterns are made, from
different places in the code; the mod checks that they agree.

Usage: .venv\\Scripts\\python tools\\sigmaker.py [dump] > mod\\nbn\\signatures.h
"""
import re
import struct
import sys

import capstone

DUMP = sys.argv[1] if len(sys.argv) > 1 else r"mod\build\Release\run\Rocksmith2014_mem.bin"
DUMP_BASE = 0x009B0000  # the base the game was loaded at when the dump was made (probe log)

# name, kind, RVA on the L&P build (verified, see mod/nbn/game.cpp)
TARGETS = [
    ("Root", "ref", 0x00F6062C),
    ("PreviewName", "ref", 0x00F60514),
    ("ProviderVtable", "ref", 0x00DA0E70),
    ("PostEventChar", "func", 0x00AC4870),
    ("ExecuteActionOnEventId", "func", 0x00AC4900),
    ("GetEventIDFromPlayingID", "func", 0x00ABFE80),
]
MIN_LEN, MIN_FIXED, MAX_INSNS = 12, 10, 150

img = open(DUMP, "rb").read()
pe = struct.unpack_from("<I", img, 0x3C)[0]
nsec = struct.unpack_from("<H", img, pe + 6)[0]
optsize = struct.unpack_from("<H", img, pe + 20)[0]
size_of_image = struct.unpack_from("<I", img, pe + 24 + 56)[0]
sections = []  # executable sections: (rva, size)
s = pe + 24 + optsize
for _ in range(nsec):
    vsize, va = struct.unpack_from("<II", img, s + 8)
    if struct.unpack_from("<I", img, s + 36)[0] & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
        sections.append((va, vsize))
    s += 40

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def in_image(v):
    return DUMP_BASE <= v < DUMP_BASE + size_of_image


def masked(rva, n_bytes):
    """Bytes of one instruction at rva, with the build-dependent parts as None."""
    b = list(img[rva:rva + n_bytes])
    op = img[rva]
    if op in (0xE8, 0xE9) and n_bytes == 5:
        b[1:5] = [None] * 4
    elif op == 0x0F and 0x80 <= img[rva + 1] <= 0x8F and n_bytes == 6:
        b[2:6] = [None] * 4
    for i in range(1, n_bytes - 3):
        if b[i] is not None and in_image(struct.unpack_from("<I", img, rva + i)[0]):
            b[i:i + 4] = [None] * 4
    return b


def matches(pattern):
    """Where the pattern matches in the executable sections (stops at 2). Overlapping matches count
    (a lookahead), as in the mod's scanner: twin functions next to each other overlap."""
    body = b"".join(b"." if x is None else re.escape(bytes([x])) for x in pattern)
    rx = re.compile(b"(?=" + body + b")", re.DOTALL)
    found = []
    for va, vsize in sections:
        for m in rx.finditer(img, va, va + vsize - len(pattern) + 1):
            found.append(m.start())
            if len(found) > 1:
                return found
    return found


def grow(start, first_len=None):
    """Pattern from `start`, one instruction at a time, until unique. None if it never is."""
    pat, rva = [], start
    for _ in range(MAX_INSNS):
        insn = next(md.disasm(img[rva:rva + 16], rva), None)
        if insn is None:
            return None
        pat += masked(rva, insn.size)
        rva += insn.size
        fixed = sum(x is not None for x in pat)
        if len(pat) >= MIN_LEN and fixed >= MIN_FIXED and matches(pat) == [start]:
            return pat
    return None


def ref_sites(target_rva):
    """Instructions in the code that contain the absolute address base+target_rva."""
    needle = struct.pack("<I", DUMP_BASE + target_rva)
    for va, vsize in sections:
        p = img.find(needle, va, va + vsize)
        while p != -1:
            # the instruction holding it starts 1..6 bytes before (opcode, modrm, sib, disp...)
            for back in range(1, 7):
                insn = next(md.disasm(img[p - back:p - back + 16], p - back), None)
                if insn and insn.size >= back + 4 and insn.mnemonic in ("mov", "push", "cmp", "lea", "add", "sub", "test"):
                    yield p - back, back
                    break
            p = img.find(needle, p + 1, va + vsize)


def fmt(pattern):
    return " ".join("??" if x is None else "%02X" % x for x in pattern)


HEADER = r"""// signatures.h: byte patterns that find the game addresses Note-by-Note uses (see game.cpp, Scan).
//
// GENERATED by tools/sigmaker.py from a memory dump of the L&P build: don't edit, regenerate with
//   .venv\Scripts\python tools\sigmaker.py > mod\nbn\signatures.h
// "??" = a byte that changes between game builds or load addresses (absolute addresses, call/jump
// targets). Func: the match is the function itself. Ref: the match is code that uses the address;
// the address is read from the match at `capture`. Several patterns for one name must agree.
#pragma once

namespace nbn::game::sig {

enum class Kind { Func, Ref };

struct Signature {
    const char* name;
    Kind kind;
    int capture;          // Ref: offset in the match of the 4-byte absolute address
    const char* pattern;  // hex bytes, "??" = any
};

// From %s (L&P build, dump base 0x%08X).
inline constexpr Signature kSignatures[] = {"""

print(HEADER % (DUMP.replace("\\", "/"), DUMP_BASE))
for name, kind, rva in TARGETS:
    if kind == "func":
        pat = grow(rva)
        print('    {"%s", Kind::Func, 0, "%s"},' % (name, fmt(pat)) if pat else "    // %s: NO unique pattern" % name)
        continue
    found = []
    for i, (site, capture) in enumerate(ref_sites(rva)):
        if i >= 30 or len(found) >= 6:
            break
        pat = grow(site)
        if pat:
            found.append((len(pat), site, capture, pat))
    found.sort()
    if not found:
        print("    // %s: NO unique pattern" % name)
    for _, site, capture, pat in found[:3]:
        print('    {"%s", Kind::Ref, %d, "%s"},  // site rva 0x%06X' % (name, capture, fmt(pat), site))
print("};")
print("")
print("}  // namespace nbn::game::sig")
