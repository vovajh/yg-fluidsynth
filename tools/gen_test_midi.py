#!/usr/bin/env python3
"""Генерирует тестовый MIDI (type 1): мелодия на пианино, бас, струнные и ударные на канале 10."""
import struct, sys

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))

def track(events):
    data = b""
    last = 0
    for t, msg in sorted(events, key=lambda e: e[0]):
        data += vlq(t - last) + msg
        last = t
    data += vlq(0) + b"\xFF\x2F\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data

def build(bars=8):
    ppq = 480
    bar = ppq * 4
    tempo = [(0, b"\xFF\x51\x03" + struct.pack(">I", 500000)[1:])]  # 120 bpm
    ch = lambda c, s, a, b=None: bytes([s | c, a] + ([b] if b is not None else []))
    mel, bass, strg, drums = [], [], [], []
    mel.append((0, ch(0, 0xC0, 0)))
    bass.append((0, ch(1, 0xC0, 33)))
    strg.append((0, ch(2, 0xC0, 48)))
    notes = [60, 62, 64, 67, 69, 67, 64, 62]
    for b in range(bars):
        base = b * bar
        for i, n in enumerate(notes):
            t = base + i * ppq // 2
            mel += [(t, ch(0, 0x90, n + (12 if b % 2 else 0), 96)), (t + ppq // 2 - 10, ch(0, 0x80, n + (12 if b % 2 else 0), 0))]
        root = [36, 41, 43, 36][b % 4]
        for i in range(4):
            t = base + i * ppq
            bass += [(t, ch(1, 0x90, root, 100)), (t + ppq - 20, ch(1, 0x80, root, 0))]
        for n in (root + 24, root + 28, root + 31):
            strg += [(base, ch(2, 0x90, n, 70)), (base + bar - 30, ch(2, 0x80, n, 0))]
        for i in range(8):
            t = base + i * ppq // 2
            drums += [(t, ch(9, 0x90, 42, 70)), (t + 30, ch(9, 0x80, 42, 0))]
        for i in (0, 2):
            t = base + i * ppq
            drums += [(t, ch(9, 0x90, 36, 110)), (t + 30, ch(9, 0x80, 36, 0))]
        for i in (1, 3):
            t = base + i * ppq
            drums += [(t, ch(9, 0x90, 38, 100)), (t + 30, ch(9, 0x80, 38, 0))]
    hdr = b"MThd" + struct.pack(">IHHH", 6, 1, 5, ppq)
    return hdr + track(tempo) + track(mel) + track(bass) + track(strg) + track(drums)

if __name__ == "__main__":
    bars = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    open(sys.argv[1], "wb").write(build(bars))
