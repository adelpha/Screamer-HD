#!/usr/bin/env python3
"""Convert a DOS/4GW Linear Executable (LE) into a flat-mapped ELF32 for Ghidra.

Ghidra has no LE/LX loader. This script parses the LE header, loads every object at
its preferred base address, applies all internal fixups (so pointers hold final flat
addresses) and writes a static ELF32 i386 executable with one PT_LOAD segment and one
section per object. Ghidra's stock ELF loader then imports it with correct addresses.

It also writes <out>.fixups.json listing every fixup source address and its target,
which tools/ghidra/ApplyLeFixups.java uses to mark pointers.

Usage:
    py -3.14 tools/le/le2elf.py SCREAMER/STARTL.EXE artifacts/ghidra/STARTL.elf

Selector fixups (16-bit selector of an object) are left untouched; in the DOS/4GW flat
model all objects share the same flat selectors, so they do not affect analysis.
"""
from __future__ import annotations

import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

OBJ_READ, OBJ_WRITE, OBJ_EXEC = 0x1, 0x2, 0x4
OBJ_BIG = 0x2000  # 32-bit default operand size


@dataclass
class LeObject:
    index: int  # 1-based
    vsize: int
    base: int
    flags: int
    page_index: int  # 1-based into object page table
    page_count: int
    data: bytearray = field(default_factory=bytearray)

    @property
    def name(self) -> str:
        kind = "code" if self.flags & OBJ_EXEC else "data"
        return f"{kind}{self.index}"


class LeFile:
    def __init__(self, raw: bytes):
        self.raw = raw
        if raw[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        self.le = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[self.le:self.le + 2] != b"LE":
            raise ValueError("no LE header at e_lfanew")
        u32 = lambda off: struct.unpack_from("<I", raw, self.le + off)[0]
        self.num_pages = u32(0x14)
        self.eip_obj, self.eip = u32(0x18), u32(0x1C)
        self.esp_obj, self.esp = u32(0x20), u32(0x24)
        self.page_size = u32(0x28)
        self.last_page_size = u32(0x2C)
        self.obj_table = self.le + u32(0x40)
        self.num_objs = u32(0x44)
        self.obj_page_table = self.le + u32(0x48)
        self.fixup_page_table = self.le + u32(0x68)
        self.fixup_record_table = self.le + u32(0x6C)
        self.data_pages = u32(0x80)  # relative to file start
        self.objects: list[LeObject] = []
        for i in range(self.num_objs):
            vsize, base, flags, pidx, pcnt, _ = struct.unpack_from("<6I", raw, self.obj_table + 24 * i)
            self.objects.append(LeObject(i + 1, vsize, base, flags, pidx, pcnt))
        self.fixups: list[dict] = []
        self._load_pages()
        self._apply_fixups()

    # LE object page table entries: 24-bit page number (high byte first) + flags byte.
    def _page_number(self, page_index: int) -> tuple[int, int]:
        b = self.raw[self.obj_page_table + 4 * (page_index - 1):][:4]
        return (b[0] << 16) | (b[1] << 8) | b[2], b[3]

    def _page_file_offset(self, page_number: int) -> tuple[int, int]:
        off = self.data_pages + (page_number - 1) * self.page_size
        size = self.last_page_size if page_number == self.num_pages else self.page_size
        return off, size

    def _load_pages(self) -> None:
        for obj in self.objects:
            span = max(obj.vsize, obj.page_count * self.page_size)
            obj.data = bytearray(span)
            for p in range(obj.page_count):
                num, flags = self._page_number(obj.page_index + p)
                if flags not in (0, ):
                    raise ValueError(f"unsupported page flags {flags:#x} (object {obj.index} page {p})")
                off, size = self._page_file_offset(num)
                obj.data[p * self.page_size:p * self.page_size + size] = self.raw[off:off + size]

    def object_for_page(self, page_index: int) -> tuple[LeObject, int]:
        for obj in self.objects:
            if obj.page_index <= page_index < obj.page_index + obj.page_count:
                return obj, page_index - obj.page_index
        raise ValueError(f"page {page_index} belongs to no object")

    def _apply_fixups(self) -> None:
        raw = self.raw
        for page in range(1, self.num_pages + 1):
            start, end = struct.unpack_from("<II", raw, self.fixup_page_table + 4 * (page - 1))
            obj, pinobj = self.object_for_page(page)
            page_base = pinobj * self.page_size
            pos = self.fixup_record_table + start
            stop = self.fixup_record_table + end
            while pos < stop:
                src_type, flags = raw[pos], raw[pos + 1]
                pos += 2
                if src_type & 0x20:
                    count = raw[pos]
                    pos += 1
                    srcoffs = None
                else:
                    srcoffs = [struct.unpack_from("<h", raw, pos)[0]]
                    pos += 2
                target_kind = flags & 0x3
                if target_kind != 0:
                    raise ValueError(f"unsupported fixup target type {target_kind} (imports)")
                if flags & 0x40:
                    tobj = struct.unpack_from("<H", raw, pos)[0]
                    pos += 2
                else:
                    tobj = raw[pos]
                    pos += 1
                stype = src_type & 0x0F
                toff = 0
                if stype != 0x2:  # selector-only fixups carry no target offset
                    if flags & 0x10:
                        toff = struct.unpack_from("<I", raw, pos)[0]
                        pos += 4
                    else:
                        toff = struct.unpack_from("<H", raw, pos)[0]
                        pos += 2
                if flags & 0x04:
                    raise ValueError("additive fixups not supported")
                if srcoffs is None:
                    srcoffs = [struct.unpack_from("<h", raw, pos + 2 * k)[0] for k in range(count)]
                    pos += 2 * count
                target = (self.objects[tobj - 1].base + toff) & 0xFFFFFFFF
                for so in srcoffs:
                    self._apply_one(obj, page_base + so, stype, target, tobj)

    def _apply_one(self, obj: LeObject, off: int, stype: int, target: int, tobj: int) -> None:
        if off < 0 or off >= len(obj.data):
            return  # the part of a page-spanning fixup that belongs to another page
        src_addr = obj.base + off
        if stype == 0x7:  # 32-bit offset
            struct.pack_into("<I", obj.data, off, target)
        elif stype == 0x8:  # 32-bit self-relative
            struct.pack_into("<i", obj.data, off, target - (src_addr + 4))
        elif stype == 0x5:  # 16-bit offset
            struct.pack_into("<H", obj.data, off, target & 0xFFFF)
        elif stype == 0x2:  # 16-bit selector: leave the original bytes
            pass
        elif stype == 0x6:  # 16:32 pointer: write the offset part only
            struct.pack_into("<I", obj.data, off, target)
        elif stype == 0x3:  # 16:16 pointer
            struct.pack_into("<H", obj.data, off, target & 0xFFFF)
        elif stype == 0x0:  # byte
            obj.data[off] = target & 0xFF
        else:
            raise ValueError(f"unsupported fixup source type {stype:#x}")
        self.fixups.append({"src": src_addr, "type": stype, "target": target, "target_obj": tobj})


def write_elf(le: LeFile, out: Path) -> None:
    """One PT_LOAD per object. Sections: the stored part of each object as PROGBITS and the
    zero-filled tail (BSS / stack) as a separate NOBITS section, so Ghidra maps all of it."""
    objs = le.objects
    entry = le.objects[le.eip_obj - 1].base + le.eip
    ehsize, phentsize, shentsize = 52, 32, 40

    data_off = (ehsize + phentsize * len(objs) + 0xFFF) & ~0xFFF
    layout = []  # (obj, file offset, stored bytes, memory size)
    cur = data_off
    for o in objs:
        stored = min(len(o.data), o.page_count * le.page_size)
        layout.append((o, cur, stored, max(o.vsize, stored)))
        cur += (stored + 0xFFF) & ~0xFFF

    # (name, type, flags, addr, offset, size)
    sections = []
    for o, off, stored, memsz in layout:
        shflags = 0x2 | (0x1 if o.flags & OBJ_WRITE else 0) | (0x4 if o.flags & OBJ_EXEC else 0)
        if stored:
            sections.append((o.name, 1, shflags, o.base, off, stored))
        if memsz > stored:
            sections.append((o.name + ".bss" if stored else o.name, 8, shflags, o.base + stored, off + stored,
                             memsz - stored))
    shstr = bytearray(b"\0")
    name_offs = []
    for name, *_ in sections:
        name_offs.append(len(shstr))
        shstr += name.encode() + b"\0"
    shstr_name = len(shstr)
    shstr += b".shstrtab\0"

    shstr_off = cur
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    nsec = len(sections) + 2
    buf = bytearray(sh_off + shentsize * nsec)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    struct.pack_into("<16sHHIIIIIHHHHHH", buf, 0, ident, 2, 3, 1, entry, ehsize, sh_off, 0,
                     ehsize, phentsize, len(objs), shentsize, nsec, nsec - 1)
    for i, (o, off, stored, memsz) in enumerate(layout):
        pflags = (4 if o.flags & OBJ_READ else 0) | (2 if o.flags & OBJ_WRITE else 0) | (1 if o.flags & OBJ_EXEC else 0)
        struct.pack_into("<8I", buf, ehsize + phentsize * i, 1, off, o.base, o.base, stored, memsz, pflags, 0x1000)
        buf[off:off + stored] = o.data[:stored]
    buf[shstr_off:shstr_off + len(shstr)] = shstr
    for i, ((name, sh_type, shflags, addr, off, size), noff) in enumerate(zip(sections, name_offs)):
        struct.pack_into("<10I", buf, sh_off + shentsize * (i + 1), noff, sh_type, shflags, addr, off, size,
                         0, 0, 16, 0)
    struct.pack_into("<10I", buf, sh_off + shentsize * (nsec - 1), shstr_name, 3, 0, 0, shstr_off,
                     len(shstr), 0, 0, 1, 0)
    out.write_bytes(buf)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, dst = Path(argv[1]), Path(argv[2])
    le = LeFile(src.read_bytes())
    dst.parent.mkdir(parents=True, exist_ok=True)
    write_elf(le, dst)
    info = {
        "source": src.name,
        "entry": le.objects[le.eip_obj - 1].base + le.eip,
        "initial_esp": le.objects[le.esp_obj - 1].base + le.esp,
        "objects": [{"index": o.index, "name": o.name, "base": o.base, "vsize": o.vsize, "flags": o.flags,
                     "pages": o.page_count} for o in le.objects],
        "fixups": le.fixups,
    }
    dst.with_suffix(".fixups.json").write_text(json.dumps(info))
    for o in le.objects:
        print(f"object {o.index} {o.name:6} base {o.base:#010x} size {o.vsize:#09x} flags {o.flags:#06x} pages {o.page_count}")
    print(f"entry {info['entry']:#010x}  esp {info['initial_esp']:#010x}  fixups {len(le.fixups)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
