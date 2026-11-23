#!/usr/bin/env python3
"""Turns a raw mode 13h capture (vga.raw) plus the game's own g_palette into vga.bmp.

Used by capture-vga.ps1. The palette is located by finding the game's data object in the
memory dump (same method as tools/ghidra/runtime_probe.py) and reading g_palette
(0x10CF58 in the STARTL.EXE symbol map, 768 bytes of 6-bit DAC values).
Usage: vga_to_bmp.py <STARTL.elf> <capture dir>
"""
import struct
import sys
from pathlib import Path

G_PALETTE = 0x10CF58
DATA3_BASE = 0x100000


def main() -> int:
    elf, d = Path(sys.argv[1]).read_bytes(), Path(sys.argv[2])
    mem = bytearray(0x100000)
    for mb in range(1, 16):
        mem += (d / f"mem_{mb:02d}.bin").read_bytes()
    # data3 starts with the game's first string at +4 (see runtime_probe.py)
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shnum, = struct.unpack_from("<H", elf, 0x30)
    sig = None
    for i in range(shnum):
        _, typ, _, addr, off, size = struct.unpack_from("<6I", elf, shoff + 40 * i)
        if typ == 1 and addr == DATA3_BASE:
            sig = elf[off + 4:off + 28]
    data_rt = mem.find(sig) - 4
    if data_rt < 0:
        sys.exit("game data object not found in memory")
    pal6 = mem[data_rt + G_PALETTE - DATA3_BASE:data_rt + G_PALETTE - DATA3_BASE + 768]
    pal = bytes(((v & 63) << 2) | ((v & 63) >> 4) for v in pal6)  # same expansion as DOSBox / vga6_to_8
    pix = (d / "vga.raw").read_bytes()
    # 8-bit BMP, bottom-up rows
    rows = b"".join(pix[y * 320:(y + 1) * 320] for y in range(199, -1, -1))
    palette = b"".join(bytes((pal[i * 3 + 2], pal[i * 3 + 1], pal[i * 3], 0)) for i in range(256))
    header_size = 14 + 40 + 1024
    bmp = b"BM" + struct.pack("<IHHI", header_size + len(rows), 0, 0, header_size)
    bmp += struct.pack("<IiiHHIIiiII", 40, 320, 200, 1, 8, 0, len(rows), 2835, 2835, 256, 0)
    (d / "vga.bmp").write_bytes(bmp + palette + rows)
    print(d / "vga.bmp")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
