#!/usr/bin/env python3
"""Collects struct field usage from the decompiler dump to help define record types.

For each listed function, finds accesses through its first parameter such as
`*(float *)(param_1 + 0x4c8)` or `param_1[0x118]` and prints offset, access types and the
functions that use it. With --struct it prints a draft C struct instead (fields typed by
majority use, named f_<offset>, gaps as byte arrays). The output is a starting point for
reference/symbols/*.types.h, where fields get real names; it is not authoritative.

Usage:
  py -3.14 tools/ghidra/field_usage.py artifacts/ghidra/STARTL/dump/decomp.c <func> [<func> ...]
  py -3.14 tools/ghidra/field_usage.py DECOMP --struct PlayerCar --size 0x1084 <func> ...
  (func = name or hex address; @file reads names from a file)
"""
import argparse
import re
from collections import defaultdict

SMALL = {"char", "byte", "undefined1", "short", "ushort", "undefined2", "word", "bool"}


def collect(decomp: str, wanted: list[str]):
    blocks = {}
    for blk in decomp.split("// ===== ")[1:]:
        addr, name = blk.split("\n", 1)[0].split()[:2]
        blocks[addr.lower()] = blocks[name] = blk
    use = defaultdict(lambda: defaultdict(set))
    for w in wanted:
        blk = blocks.get(w) or blocks.get(w.lower().rjust(8, "0"))
        if not blk:
            print("// missing", w)
            continue
        m = re.search(r"\(\s*([\w ]+?)\s*(\*?)\s*param_1\b", blk)
        ptype = (m.group(1) + m.group(2)) if m else ""
        for t, off in re.findall(r"\*\((\w+) \*\)\(param_1 \+ (0x[0-9a-f]+|\d+)\)", blk):
            use[int(off, 0)][t].add(w)
        if ptype.endswith("*") and ptype[:-1].strip() in ("int", "uint", "float", "undefined4"):
            for idx in re.findall(r"param_1\[(0x[0-9a-f]+|\d+)\]", blk):
                use[int(idx, 0) * 4][ptype[:-1].strip()].add(w)
        for off in re.findall(r"\(param_1 \+ (0x[0-9a-f]+|\d+)\)(?!\))", blk):
            use[int(off, 0)]["&"].add(w)
    return use


def emit_struct(use, name: str, size: int) -> str:
    out = [f"struct {name} {{ /* {size:#x} bytes */"]
    pos = 0
    for off in sorted(use):
        if off < pos or off >= size or off % 4:
            continue
        counts = {t: len(fs) for t, fs in use[off].items() if t != "&"}
        if not counts or max(counts, key=counts.get) in SMALL:
            continue
        t = max(counts, key=counts.get)
        ctype = "float" if t == "float" else "unsigned int" if t == "uint" else "int"
        if off > pos:
            out.append(f"    unsigned char pad_{pos:03x}[{off - pos:#x}];")
        out.append(f"    {ctype} f_{off:03x};")
        pos = off + 4
    if pos < size:
        out.append(f"    unsigned char pad_{pos:03x}[{size - pos:#x}];")
    out.append("};")
    return "\n".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("decomp")
    ap.add_argument("funcs", nargs="+")
    ap.add_argument("--struct")
    ap.add_argument("--size", type=lambda s: int(s, 0))
    a = ap.parse_args()
    wanted = []
    for f in a.funcs:
        wanted += open(f[1:]).read().split() if f.startswith("@") else [f]
    use = collect(open(a.decomp, encoding="utf-8").read(), wanted)
    if a.struct:
        print(emit_struct(use, a.struct, a.size))
        return
    for off in sorted(use):
        types = ", ".join(f"{t}x{len(fs)}" for t, fs in use[off].items())
        funcs = sorted({f for fs in use[off].values() for f in fs})
        print(f"{off:#06x}  {types:30} {' '.join(funcs[:6])}{' ...' if len(funcs) > 6 else ''}")


if __name__ == "__main__":
    main()
