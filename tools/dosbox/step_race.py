#!/usr/bin/env python3
"""Records the player car of the original game every frame of a race, with scripted controls.

Needs a DOSBox started by run-original.ps1 -Webserver -Headless that is in a race (see
record-race.ps1). Stepping the game through the web API (JMP $ traps) hangs DOSBox after a
frame or two, so this installs a small hook in the game instead:

  * the CALL race_frame in the game loop (0x1FF44) is redirected to code written at
    HOOK_BASE (unused memory at 15 MB);
  * the hook copies g_frame_time, the damping-table pointer, g_race_flags and the first
    CAR_BYTES of g_player_car to a buffer, writes the scripted controls over g_input_*
    (controls_update has already run), counts the frame and jumps to race_frame.

Each record holds the state *before* that frame's race_frame, so record k + 1 is the result
of running frame k with record k's frame time and controls. The run is real time; the
frame times are whatever the game measured.

Usage: py -3.14 tools/dosbox/step_race.py OUT.trace FRAMES SCRIPT [SKIP]
  SKIP: frames to drive (following SCRIPT) before recording FRAMES frames
  SCRIPT: comma-separated segments "frames:keys", keys from accel, brake, left, right, e.g.
          "100:accel,60:accel+left,40:brake"
Trace file: b"SCRTRACE", u32 version 2, u32 record size, then records of
  f32 frame_time, i32 pow_row, u32 race_flags, i32 accel, brake, steer_left, steer_right,
  u8[CAR_BYTES] g_player_car
"""
import struct
import sys
import time
import urllib.error
import urllib.request

API = "http://127.0.0.1:8086/api/v1"
CODE = 0x1C2000          # code object delta under the harness config
DATA = 0x1AE000          # data object delta
RACE_FRAME = 0x1FF60
CALL_RACE_FRAME = 0x1FF44  # E8 rel32 in the game loop
PLAYER_CAR = 0x10F918
CAR_BYTES = 0x800          # PlayerCar up to the network fields (0x1084 in all)
FRAME_TIME, POW_TABLE, POW_BASE = 0x118E00, 0x10CF04, 0x1080E4
RACE_FLAGS = 0x118E18
INPUT_ACCEL, INPUT_BRAKE, INPUT_STEER, INPUT_STEER_R = 0x11987C, 0x119880, 0x119884, 0x119888
ACCEL_SIGN, BRAKE_SIGN = 0x1197E8, 0x119804

HOOK_BASE = 0xF00000       # vars: +0 write pointer, +4 frame count, +8 buffer end, +0x10 code
SCRIPT_AT = HOOK_BASE + 0x100
HEADER = 28
RECORD = HEADER + CAR_BYTES


def _retry(fn):
    for attempt in range(50):
        try:
            return fn()
        except urllib.error.HTTPError as e:
            if e.code != 500 or attempt == 49:
                raise
            time.sleep(0.05)


def mem(addr, n):
    def once():
        with urllib.request.urlopen(f"{API}/memory/{addr}/{n}", timeout=30) as r:
            return r.read()
    return _retry(once)


def put(addr, data):
    req = urllib.request.Request(f"{API}/memory/{addr}", data=data, method="PUT",
                                 headers={"Content-Type": "application/octet-stream"})
    _retry(lambda: urllib.request.urlopen(req, timeout=30).read())


def parse_script(s):
    out = []
    for seg in s.split(","):
        n, keys = seg.split(":")
        out += [set(keys.split("+")) - {""}] * int(n)
    return out


def hook_code():
    d = lambda a: struct.pack("<I", a & 0xFFFFFFFF)
    x = bytes.fromhex
    inputs = (INPUT_ACCEL, INPUT_BRAKE, INPUT_STEER, INPUT_STEER_R)
    c = bytearray()
    c += x("60fc")                                     # pushad; cld
    c += x("8b1d") + d(HOOK_BASE + 4)                  # mov ebx, [count]
    c += x("c1e304")                                   # shl ebx, 4
    for i, target in enumerate(inputs):
        c += x("8b83") + d(SCRIPT_AT + 4 * i)          # mov eax, [ebx + script + 4i]
        c += x("a3") + d(target + DATA)                # mov [input], eax
    c += x("ff05") + d(HOOK_BASE + 4)                  # inc dword [count]
    c += x("a1") + d(HOOK_BASE + 4)                    # mov eax, [count]
    c += x("3b05") + d(HOOK_BASE + 12)                 # cmp eax, [skip]
    jumps = [len(c)]
    c += x("0f86") + d(0)                              # jbe done (still skipping)
    c += x("8b3d") + d(HOOK_BASE)                      # mov edi, [ptr]
    c += x("3b3d") + d(HOOK_BASE + 8)                  # cmp edi, [end]
    jumps.append(len(c))
    c += x("0f83") + d(0)                              # jae done (buffer full)
    for a in (FRAME_TIME, POW_TABLE, RACE_FLAGS) + inputs:
        c += x("a1") + d(a + DATA) + x("ab")           # mov eax, [a]; stosd
    c += x("be") + d(PLAYER_CAR + DATA)                # mov esi, g_player_car
    c += x("b9") + d(CAR_BYTES // 4) + x("f3a5")       # mov ecx, n; rep movsd
    c += x("893d") + d(HOOK_BASE)                      # mov [ptr], edi
    done = len(c)
    for j in jumps:
        c[j + 2:j + 6] = struct.pack("<i", done - (j + 6))
    c += x("61")                                       # popad
    c += x("68") + d(RACE_FRAME + CODE) + x("c3")      # push race_frame; ret
    return bytes(c)


def main():
    out, frames, script = sys.argv[1], int(sys.argv[2]), parse_script(sys.argv[3])
    skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0  # frames to drive before recording
    i32 = lambda b: struct.unpack("<i", b)[0]
    accel_sign = i32(mem(ACCEL_SIGN + DATA, 4)) or 1
    brake_sign = i32(mem(BRAKE_SIGN + DATA, 4)) or 1
    table = bytearray()
    for k in range(skip + frames + 1):
        keys = script[k] if k < len(script) else set()
        table += struct.pack("<iiii", 1023 * accel_sign if "accel" in keys else 0,
                             1023 * brake_sign if "brake" in keys else 0,
                             1023 if "left" in keys else 0, 1023 if "right" in keys else 0)
    buffer_at = (SCRIPT_AT + len(table) + 0xFFF) & ~0xFFF
    end = buffer_at + frames * RECORD
    if end > 0x1000000:
        sys.exit("too many frames for the buffer")
    put(SCRIPT_AT, bytes(table))
    put(HOOK_BASE + 0x10, hook_code())
    put(HOOK_BASE, struct.pack("<IIII", buffer_at, 0, end, skip))
    call_at = CALL_RACE_FRAME + CODE
    original = mem(call_at, 5)
    if original[0] != 0xE8:
        sys.exit(f"unexpected bytes at the race_frame call: {original.hex()}")
    rel = (HOOK_BASE + 0x10) - (call_at + 5)
    put(call_at + 1, struct.pack("<i", rel))
    t0 = time.time()
    try:
        while True:
            n = i32(mem(HOOK_BASE + 4, 4))
            if n >= skip + frames:
                break
            if time.time() - t0 > 30 + (skip + frames) * 0.5:
                print(f"only {n} frames recorded", file=sys.stderr)
                break
            time.sleep(0.5)
    finally:
        put(call_at, original)
    n = max(0, min(i32(mem(HOOK_BASE + 4, 4)) - skip, frames))
    raw = b"".join(mem(buffer_at + o, min(0x40000, n * RECORD - o)) for o in range(0, n * RECORD, 0x40000))
    recs = []
    for k in range(n):
        r = bytearray(raw[k * RECORD:(k + 1) * RECORD])
        ptr = struct.unpack_from("<I", r, 4)[0]
        struct.pack_into("<i", r, 4, (ptr - (POW_BASE + DATA)) // 400)
        recs.append(bytes(r))
    with open(out, "wb") as f:
        f.write(b"SCRTRACE" + struct.pack("<II", 2, RECORD))
        f.write(b"".join(recs))
    flags = [struct.unpack_from("<I", r, 8)[0] for r in recs]
    if flags:
        print(f"{n} frames -> {out}; race flags first {flags[0]:#x} last {flags[-1]:#x}")


if __name__ == "__main__":
    main()
