#!/usr/bin/env python3
"""sounds.py: Snowline's generated sounds (no third-party audio), imported through the editor's MCP.

- Audio/Wind: four seconds of wind, looping seamlessly (its last half second crossfaded into its
  first): noise through two low-pass filters (a low roar and a thinner hiss), swelling in slow
  gusts. Board.as plays it on the rider's Wind source, louder and higher as the rider goes faster.
"""
import math, os, random, struct, sys, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

RATE = 22050


def wind(path, seconds=4.0, fade=0.5):
    rnd = random.Random(31)
    total = int((seconds + fade) * RATE)
    low = hiss = 0.0
    raw = []
    for i in range(total):
        white = rnd.uniform(-1.0, 1.0)
        low += (white - low) * 0.02    # the roar
        hiss += (white - hiss) * 0.25  # the hiss
        t = i / RATE
        gust = 0.65 + 0.35 * math.sin(2 * math.pi * t / seconds) * math.sin(2 * math.pi * t * 0.73 / seconds + 1.0)
        raw.append((low * 3.2 + hiss * 0.35) * gust)
    n, f = int(seconds * RATE), int(fade * RATE)
    out = raw[:n]
    for i in range(f):  # the tail fades into the head, so the loop has no seam
        k = i / f
        out[i] = raw[n + i] * (1.0 - k) + raw[i] * k
    peak = max(abs(v) for v in out) or 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.8 * 32767)) for v in out))


def chime(path, notes=((1318.5, 0.0), (1975.5, 0.07)), seconds=0.7):
    total = int(seconds * RATE)
    out = [0.0] * total
    for freq, start in notes:
        for i in range(int(start * RATE), total):
            t = i / RATE - start
            ring = math.exp(-t * 7.0) * min(1.0, t / 0.003)  # a quick strike, then the ring dies
            out[i] += ring * (math.sin(2 * math.pi * freq * t) + 0.3 * math.sin(4 * math.pi * freq * t))
    peak = max(abs(v) for v in out) or 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.8 * 32767)) for v in out))


def main():
    out = os.path.join(HERE, "generated", "Audio")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "Wind.wav")
    wind(path)
    print("Wind", mcp("asset_import", {"source": path, "group": "Audio", "importer": "Audio"})["guid"])
    path = os.path.join(out, "GemChime.wav")
    chime(path)
    print("GemChime", mcp("asset_import", {"source": path, "group": "Audio", "importer": "Audio"})["guid"])


main()
