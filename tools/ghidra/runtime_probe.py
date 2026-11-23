#!/usr/bin/env python3
"""Locate Screamer's LE objects in a DOSBox memory dump and print symbol values.

DOS/4GW does not load the objects at their preferred bases (0x10000 / 0x100000), so
addresses from reference/symbols/<exe>.sym must be translated. This script finds the
runtime base of the code object (by matching main()'s first bytes) and of the data object
(by matching the first string of data3), then prints every 'd' symbol from the .sym file
(or the names given with --only) as hex dwords plus a string preview.

Usage (memory dumped by tools/ghidra/runtime-probe.ps1):
    py -3.14 tools/ghidra/runtime_probe.py artifacts/ghidra/STARTL/STARTL.elf \
        reference/symbols/STARTL.sym artifacts/ghidra/probe [--only g_game_mode,g_track_name]
"""
from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

CODE_BASE, DATA2_BASE, DATA3_BASE = 0x10000, 0xF0000, 0x100000


def load_dump(d: Path) -> bytearray:
    mem = bytearray(0x100000)  # first MB is not dumped
    for mb in range(1, 64):
        f = d / f"mem_{mb:02d}.bin"
        if not f.exists():
            break
        mem += f.read_bytes()
    return mem


def elf_sections(elf: bytes) -> list[tuple[int, int, int]]:
    """(addr, file offset, size) of PROGBITS sections."""
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shnum, = struct.unpack_from("<H", elf, 0x30)
    out = []
    for i in range(shnum):
        _, typ, _, addr, off, size = struct.unpack_from("<6I", elf, shoff + 40 * i)
        if typ == 1 and addr:
            out.append((addr, off, size))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("sym")
    ap.add_argument("dumpdir")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    elf = Path(a.elf).read_bytes()
    secs = elf_sections(elf)

    def elf_bytes(addr: int, n: int) -> bytes:
        for base, off, size in secs:
            if base <= addr < base + size:
                return elf[off + addr - base:off + addr - base + n]
        raise ValueError(hex(addr))

    syms = {}
    for line in Path(a.sym).read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([0-9a-fA-F]{8})\s+([fdl])\s+(\S+)", line)
        if m:
            syms[m.group(3).rstrip("?")] = (int(m.group(1), 16), m.group(2))

    mem = load_dump(Path(a.dumpdir))
    main_addr = syms["main"][0]
    sig = elf_bytes(main_addr, 13)  # up to the first relocated operand
    code_rt = mem.find(sig) - (main_addr - CODE_BASE)
    data_sig = elf_bytes(DATA3_BASE + 4, 24)
    data_rt = mem.find(data_sig) - 4
    print(f"code object: runtime {code_rt:#x} (preferred {CODE_BASE:#x}, delta {code_rt - CODE_BASE:+#x})")
    print(f"data object: runtime {data_rt:#x} (preferred {DATA3_BASE:#x}, delta {data_rt - DATA3_BASE:+#x})")

    def rt(addr: int) -> int:
        return addr - DATA3_BASE + data_rt if addr >= DATA3_BASE else addr - CODE_BASE + code_rt

    only = set(filter(None, a.only.split(",")))
    for name, (addr, kind) in sorted(syms.items(), key=lambda kv: kv[1][0]):
        if kind != "d" or (only and name not in only) or DATA2_BASE <= addr < DATA3_BASE:
            continue
        raw = mem[rt(addr):rt(addr) + 16]
        dwords = " ".join(f"{v:08x}" for v in struct.unpack("<4I", raw))
        text = raw.split(b"\0")[0]
        preview = f'  "{text.decode("latin1")}"' if len(text) >= 2 and all(32 <= c < 127 for c in text) else ""
        print(f"{name:28} {addr:#010x} -> {rt(addr):#010x}  {dwords}{preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
