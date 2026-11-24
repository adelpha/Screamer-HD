#!/usr/bin/env python3
"""Turns the x87 code of a function in the disassembly dump into readable expressions.

Ghidra's decompiler loses most floating-point math in STARTL.EXE (values that stay on the
x87 stack across calls to __CHP, sin, cos, ...). Watcom's code is regular enough to follow
the FPU stack symbolically: this prints every instruction, and for FPU stores, compares and
integer conversions the expression that was computed, with struct fields named from
reference/symbols/STARTL.types.h and constants read from the executable.

Usage: py -3.14 tools/ghidra/x87_lift.py FUNC_ADDR [--struct PlayerCar] [--all]
  --all   also print the integer instructions (default: only FPU results and control flow)
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DUMP = ROOT / "artifacts/ghidra/STARTL/dump"
ELF = ROOT / "artifacts/ghidra/STARTL/STARTL.elf"
TYPES = ROOT / "reference/symbols/STARTL.types.h"

BASE = {"int": 4, "unsigned int": 4, "float": 4, "char": 1, "unsigned char": 1, "short": 2,
        "unsigned short": 2, "void": 4}


def struct_fields():
    """name -> list of (offset, size, field name) with nested structs and arrays expanded."""
    text = TYPES.read_text()
    out, sizes = {}, {}
    for m in re.finditer(r"typedef struct (\w+) \{([^\n]*)\n(.*?)\n\} (\w+);", text, re.S):
        name, body = m.group(1), m.group(3)
        fields, off = [], 0
        for line in body.split("\n"):
            line = re.sub(r"/\*.*?\*/", "", line).strip().rstrip(";").strip()
            if not line:
                continue
            words = line.replace("*", " * ").split()
            tlen = 2 if words[0] == "unsigned" else 1
            typ = " ".join(words[:tlen])
            for decl in " ".join(words[tlen:]).split(","):
                decl = decl.strip()
                ptr = decl.startswith("*")
                fname = re.sub(r"[\s*]|\[.*", "", decl)
                dims = [int(x, 0) for x in re.findall(r"\[([^\]]+)\]", decl)]
                n = 1
                for x in dims:
                    n *= x
                sz = 4 if ptr else (BASE.get(typ) or sizes.get(typ, 0))
                if not ptr and typ in out:
                    for i in range(n):
                        for so, ss, sn in out[typ]:
                            idx = f"[{i}]" if dims else ""
                            fields.append((off + i * sz + so, ss, f"{fname}{idx}.{sn}"))
                elif typ in ("unsigned char", "char") and dims:
                    fields.append((off, sz * n, fname))
                else:
                    for i in range(n):
                        fields.append((off + i * sz, sz, f"{fname}[{i}]" if dims else fname))
                off += sz * n
        out[name], sizes[name] = fields, off
    return out


def elf_reader():
    d = ELF.read_bytes()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = []
    for i in range(shnum):
        _, typ, _, addr, off, size = struct.unpack_from("<IIIIII", d, shoff + i * shentsize)
        if typ != 8 and addr:
            secs.append((addr, off, size))

    def read(addr, n):
        for a, o, s in secs:
            if a <= addr and addr + n <= a + s:
                return d[o + addr - a:o + addr - a + n]
        return None
    return read


def main():
    args = sys.argv[1:]
    func = args[0].lower().rjust(8, "0")
    sname = args[args.index("--struct") + 1] if "--struct" in args else "PlayerCar"
    show_all = "--all" in args
    fields = struct_fields().get(sname, [])
    read = elf_reader()

    def field(off):
        for o, s, n in fields:
            if o <= off < o + s:
                return n if o == off else f"{n}+{off - o:#x}"
        return f"f_{off:x}"

    lines = (DUMP / "disasm.txt").read_text().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith(func + " "))
    body = []
    for l in lines[start:]:
        if l.startswith("; =====") and body:
            break
        if re.match(r"[0-9a-f]{8} ", l):
            body.append(l)

    def operand(op, size_hint):
        cm = ""
        if ";" in op:
            op, cm = op.split(";", 1)
            op, cm = op.strip(), cm.strip()
        m = re.match(r"(float|double|dword|word|qword|tword) ptr (?:[A-Z]S:)?\[(.*)\]", op)
        if not m:
            return op
        kind, inner = m.groups()
        m2 = re.fullmatch(r"(E[A-Z]{2}) \+ (-?0x[0-9a-f]+)", inner)
        if m2 and m2.group(1) == "EBP":
            v = int(m2.group(2), 16)
            return f"loc{'_m' if v < 0 else '_'}{abs(v):x}"
        if m2:
            base = regs.get(m2.group(1), m2.group(1))
            if base == "car":
                return f"car->{field(int(m2.group(2), 16))}"
            return f"{base}[{m2.group(2)}]"
        m3 = re.fullmatch(r"0x([0-9a-f]+)", inner)
        if m3:
            a = int(m3.group(1), 16)
            name = cm.split()[0] if cm else f"DAT_{a:08x}"
            if name.startswith(("DAT_", "_DAT_")) or name.startswith("Stack"):
                raw = read(a, 8 if kind == "double" else 4)
                if raw:
                    if kind == "double":
                        return repr(struct.unpack("<d", raw)[0])
                    if kind == "float":
                        return repr(round(struct.unpack("<f", raw)[0], 9)) + "f"
            return name
        if re.fullmatch(r"E[A-Z]{2}", inner):
            return f"*{inner}"
        return f"[{inner}]"

    st = []
    out = []
    # Register contents we can name: the struct pointer argument is kept in [EBP - 4] or
    # [EBP - 0xc] (Watcom's frame), globals are loaded with MOV reg, [addr].
    regs = {}
    car_slot = None

    def push(e):
        st.insert(0, e)

    def pop():
        return st.pop(0) if st else "?"

    def wrap(e):
        return e if re.fullmatch(r"[\w.\->\[\]+]+", e) else f"({e})"

    for l in body:
        addr, ins = l[:8], l[10:]
        comment = ""
        m = re.match(r"(\w+)\s*(.*)", ins)
        mn, ops = m.group(1), m.group(2)
        opl = [o.strip() for o in re.split(r",(?![^\[]*\])", ops)] if ops else []
        text = None
        if mn == "MOV" and len(opl) == 2 and re.fullmatch(r"E[A-Z]{2}", opl[0]):
            src = opl[1].split(";")[0].strip()
            mm = re.fullmatch(r"dword ptr \[EBP \+ (-0x[0-9a-f]+)\]", src)
            if mm and mm.group(1) == car_slot:
                regs[opl[0]] = "car"
            elif re.fullmatch(r"\[0x[0-9a-f]+\]", src) and ";" in opl[1]:
                regs[opl[0]] = opl[1].split(";")[1].strip().split()[0]
            else:
                regs.pop(opl[0], None)
        mm = re.fullmatch(r"MOV dword ptr \[EBP \+ (-0x[0-9a-f]+)\],EAX.*", ins)
        if mm and car_slot is None:
            car_slot = mm.group(1)
        if mn in ("FLD", "FILD"):
            if opl[0].startswith("ST"):
                k = int(opl[0][2:] or 0)
                push(st[k] if k < len(st) else "?")
            else:
                v = operand(opl[0], None)
                push(f"(float){v}" if mn == "FILD" else v)
        elif mn in ("FLDZ", "FLD1"):
            push("0.0" if mn == "FLDZ" else "1.0")
        elif mn in ("FSTP", "FST", "FISTP", "FIST"):
            dst = opl[0]
            val = st[0] if st else "?"
            if dst.startswith("ST"):
                k = int(dst[2:] or 0)
                if k < len(st):
                    st[k] = val
                if mn.endswith("P"):
                    pop()
            else:
                text = f"{operand(dst, None)} = {'(int)' if mn.startswith('FI') else ''}{val}"
                if mn.endswith("P"):
                    pop()
        elif mn in ("FADD", "FSUB", "FMUL", "FDIV", "FSUBR", "FDIVR",
                    "FADDP", "FSUBP", "FMULP", "FDIVP", "FSUBRP", "FDIVRP", "FIADD", "FIMUL", "FIDIV", "FISUB"):
            sym = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/"}[re.sub(r"^FI?|R?P?$", "", mn)]
            rev = "R" in mn[3:]
            if mn.endswith("P") and (not opl or opl[0].startswith("ST")):
                # FxxxP ST(i), ST0 : ST(i) = ST(i) op ST0, pop. Watcom writes "FADDP" with no operands.
                a, b = (st[1] if len(st) > 1 else "?"), (st[0] if st else "?")
                r = f"{wrap(b)} {sym} {wrap(a)}" if rev else f"{wrap(a)} {sym} {wrap(b)}"
                if mn in ("FSUBP", "FDIVP"):  # Intel: ST1 = ST1 - ST0 for FSUBP (AT&T swaps); Ghidra prints Intel
                    r = f"{wrap(a)} {sym} {wrap(b)}"
                pop()
                st[0:1] = [r] if st else [r]
            elif len(opl) == 2 and opl[0].startswith("ST") and opl[1].startswith("ST"):
                i, j = int(opl[0][2:] or 0), int(opl[1][2:] or 0)
                a, b = st[i], st[j]
                st[i] = f"{wrap(b)} {sym} {wrap(a)}" if rev else f"{wrap(a)} {sym} {wrap(b)}"
            else:
                v = operand(opl[0], None)
                if mn.startswith("FI"):
                    v = f"(float){v}"
                a = st[0] if st else "?"
                r = f"{wrap(v)} {sym} {wrap(a)}" if rev else f"{wrap(a)} {sym} {wrap(v)}"
                if st:
                    st[0] = r
                else:
                    push(r)
        elif mn == "FCHS":
            st[0] = f"-{wrap(st[0])}"
        elif mn == "FABS":
            st[0] = f"fabs({st[0]})"
        elif mn == "FSQRT":
            st[0] = f"sqrt({st[0]})"
        elif mn == "FXCH":
            k = int(opl[0][2:] or 1) if opl else 1
            st[0], st[k] = st[k], st[0]
        elif mn in ("FCOMP", "FCOM", "FCOMPP", "FUCOMPP", "FCOMP", "FICOMP"):
            b = operand(opl[0], None) if opl and not opl[0].startswith("ST") else (st[1] if len(st) > 1 else "?")
            text = f"compare {st[0] if st else '?'} with {b}"
            pop()
            if mn in ("FCOMPP", "FUCOMPP"):
                pop()
        elif mn in ("FNSTSW", "SAHF", "WAIT", "FNINIT", "FWAIT"):
            continue
        elif mn == "CALL":
            name = ins.split(";")[-1].strip() if ";" in ins else ops
            if name in ("sin", "cos", "sqrt", "atan", "atan2", "tan", "asin", "acos", "safe_sqrt", "fabs"):
                # Watcom passes the double on the stack: the last FSTP double ptr [ESP] is the argument.
                arg = out.pop()[1].split(" = ", 1)[1] if out and out[-1][1].startswith("*ESP =") else "?"
                push(f"{name}({arg})")
                text = None
            elif name == "__CHP":
                st[0] = f"trunc({st[0]})" if st else "trunc(?)"
            else:
                text = f"call {name}" + (f"   ; fpu stack: {st}" if st else "")
                if show_all is False:
                    out.append((addr, text))
                    continue
        elif mn.startswith("J") or mn == "RET":
            text = f"{mn} {ops}"
        elif mn.startswith("F"):
            text = f"?? {ins}"
        elif show_all:
            text = ins
        if text:
            out.append((addr, text))
    for a, t in out:
        print(a, t)


if __name__ == "__main__":
    main()
