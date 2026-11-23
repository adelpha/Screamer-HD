#!/usr/bin/env python3
"""Checks that every struct in a types header has the size stated in its '/* 0x.. bytes' comment.

Only understands the simple field forms used in reference/symbols/*.types.h.
Usage: py -3.14 tools/ghidra/check_types.py reference/symbols/STARTL.types.h
"""
import re
import sys

BASE = {"int": 4, "unsigned int": 4, "float": 4, "char": 1, "unsigned char": 1, "short": 2,
        "unsigned short": 2, "void": 4}
text = open(sys.argv[1]).read()
sizes = {}
bad = 0
for m in re.finditer(r"typedef struct (\w+) \{([^\n]*)\n(.*?)\n\} (\w+);", text, re.S):
    name, head, body = m.group(1), m.group(2), m.group(3)
    total = 0
    for line in body.split("\n"):
        line = re.sub(r"/\*.*?\*/", "", line).strip()
        if not line:
            continue
        line = line.rstrip(";").strip()
        words = line.replace("*", " * ").split()
        tlen = 2 if words[0] == "unsigned" else 1
        typ = " ".join(words[:tlen])
        rest = " ".join(words[tlen:])
        for decl in rest.split(","):
            decl = decl.strip()
            ptr = decl.startswith("*")
            dims = [int(x, 0) for x in re.findall(r"\[([^\]]+)\]", decl)]
            n = 1
            for x in dims:
                n *= x
            sz = 4 if ptr else (BASE.get(typ) or sizes.get(typ))
            if sz is None:
                print("unknown type:", typ, "in", name)
                bad += 1
                sz = 0
            total += sz * n
    sizes[name] = total
    want = re.search(r"(0x[0-9a-f]+) bytes", head)
    if want and int(want.group(1), 16) != total:
        print(f"{name}: {total:#x} bytes, comment says {want.group(1)}")
        bad += 1
print(f"{len(sizes)} structs checked, {bad} problems")
sys.exit(1 if bad else 0)
