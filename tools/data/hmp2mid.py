"""Converts Screamer's HMI MIDI songs (DATA/MUSIC.DAT MUSICn.HMP) to standard MIDI files.

    py -3.14 tools/data/hmp2mid.py SCREAMER/DATA/MUSIC.DAT artifacts/midi/out

HMP layout (see src/core/fm_synth.h): header "HMIMIDIP013195" (tracks from 0x388) or
"HMIMIDIP" (0x308); +0x30 track count, +0x38 ticks per second. Each track: u32 index,
u32 bytes including this 12-byte header, u32 ?, then MIDI events whose delta times are 7-bit
groups, least significant first, the last byte flagged with bit 7.

The .mid files are format 1 with one track per HMP track; timing keeps the song's ticks
(division = ticks per second, tempo 1 s per quarter note). The output is derived from the
user's game files: keep it out of git (artifacts/ is ignored).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import datfile  # noqa: E402


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def convert(hmp):
    if hmp[:8] != b'HMIMIDIP':
        raise ValueError('not an HMP file')
    ntracks = struct.unpack_from('<I', hmp, 0x30)[0]
    tps = struct.unpack_from('<I', hmp, 0x38)[0] or 120
    pos = 0x388 if hmp[8:14] == b'013195' else 0x308
    tracks = []
    for _ in range(ntracks):
        _, length, _ = struct.unpack_from('<III', hmp, pos)
        data = hmp[pos + 12:pos + length]
        pos += length
        out = bytearray()
        i = 0
        running = 0
        while i < len(data):
            delta, shift = 0, 0
            while i < len(data):
                b = data[i]
                i += 1
                delta |= (b & 0x7F) << shift
                shift += 7
                if b & 0x80:
                    break
            if i >= len(data):
                break
            start = i
            status = data[i]
            if status & 0x80:
                i += 1
            if status == 0xFF:
                typ, n = data[i], data[i + 1]
                i += 2 + n
            elif status in (0xF0, 0xF7):
                while i < len(data) and data[i] != 0xF7:
                    i += 1
                i += 1
            else:
                # A channel message; without a status byte it runs on the last one.
                if status & 0x80:
                    running = status
                i += 1 if (running & 0xF0) in (0xC0, 0xD0) else 2
            out += vlq(delta) + data[start:i]
            if status == 0xFF and data[start + 1] == 0x2F:
                break
        if not out.endswith(b'\xff\x2f\x00'):
            out += b'\x00\xff\x2f\x00'
        tracks.append(bytes(out))
    # Track 0 gets the tempo: 1,000,000 us per quarter note = `tps` ticks per second.
    tempo = b'\x00\xff\x51\x03' + (1000000).to_bytes(3, 'big')
    tracks[0] = tempo + tracks[0]
    mid = b'MThd' + struct.pack('>IHHH', 6, 1, len(tracks), tps)
    for t in tracks:
        mid += b'MTrk' + struct.pack('>I', len(t)) + t
    return mid



def main():
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    data = open(src, 'rb').read()
    for name, off, packed, size, flags in datfile.directory(data):
        if not name.upper().endswith('.HMP'):
            continue
        body = data[off:off + packed]
        hmp = datfile.lzw_decode(body, size) if flags & 2 else data[off:off + size]
        out = os.path.join(outdir, name.rsplit('.', 1)[0].upper() + '.MID')
        open(out, 'wb').write(convert(hmp))
        print(out)


if __name__ == '__main__':
    main()
