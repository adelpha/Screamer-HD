#!/usr/bin/env python3
r"""Lists and extracts entries of Screamer's .DAT archives (data\*.dat, gfx\*.dat).

Format (from archive_find_entry/archive_read in STARTL.EXE): the last dword of the file is
the size of a directory stored just before it. The directory is a list of 0x20-byte entries
  char name[16] (XOR 0x3A up to and including the terminating NUL), int offset,
  int f_14, int size, int flags (bit1 = LZW compressed)
ending with an entry whose name decodes to "".

Usage:
  py -3.14 tools/data/datfile.py list SCREAMER/DATA/GEN.DAT
  py -3.14 tools/data/datfile.py cat SCREAMER/DATA/GEN.DAT CARS.DTR
Extracted game data must stay out of git (write it under artifacts/).
"""
import struct
import sys


def directory(data: bytes):
    dsize, = struct.unpack_from("<I", data, len(data) - 4)
    pos = len(data) - 4 - dsize
    out = []
    while pos + 0x20 <= len(data) - 4:
        raw = bytearray(data[pos:pos + 0x20])
        name = bytearray()
        for i in range(16):
            c = raw[i] ^ 0x3A
            if c == 0:
                break
            name.append(c)
        if not name:
            break
        off, f14, size, flags = struct.unpack_from("<4i", raw, 16)
        out.append((name.decode("latin1"), off, f14, size, flags))
        pos += 0x20
    return out


def lzw_decode(src: bytes, out_size: int) -> bytes:
    """LZW as in lzw_decode/lzw_next_code: LSB-first codes of 9..12 bits, 0x100 = clear,
    0x101 = end, first free code 0x102, width grows when the next code reaches 2**width."""
    out = bytearray()
    bitpos = 0
    width, limit, nxt = 9, 0x200, 0x102
    prefix: dict[int, int] = {}
    suffix: dict[int, int] = {}

    def code() -> int:
        nonlocal bitpos
        byte = bitpos >> 3
        v = int.from_bytes(src[byte:byte + 3].ljust(3, b"\0"), "little") >> (bitpos & 7)
        bitpos += width
        return v & ((1 << width) - 1)

    def expand(c: int) -> bytes:
        s = bytearray()
        while c > 0xFF:
            s.append(suffix[c])
            c = prefix[c]
        s.append(c)
        return bytes(reversed(s))

    prev = None
    while len(out) < out_size:
        c = code()
        if c == 0x101:
            break
        if c == 0x100:
            width, limit, nxt = 9, 0x200, 0x102
            prev = code()
            out.append(prev)
            continue
        if c < nxt:
            cur = expand(c)
            first = cur[0]
        else:  # KwKwK case
            p = expand(prev)
            first = p[0]
            cur = p + bytes([first])
        out += cur
        prefix[nxt], suffix[nxt] = prev, first
        nxt += 1
        if nxt >= limit and width != 12:
            width += 1
            limit *= 2
        prev = c
    return bytes(out[:out_size])


def main() -> int:
    cmd, path = sys.argv[1], sys.argv[2]
    data = open(path, "rb").read()
    entries = directory(data)
    if cmd == "list":
        for name, off, f14, size, flags in entries:
            print(f"{name:16} off={off:#09x} f14={f14:#08x} size={size:#08x} flags={flags:#x}")
    elif cmd == "cat":
        for name, off, f14, size, flags in entries:
            if name.lower() == sys.argv[3].lower():
                body = data[off:off + f14]
                sys.stdout.buffer.write(lzw_decode(body, size) if flags & 2 else data[off:off + size])
                return 0
        sys.exit("not found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
