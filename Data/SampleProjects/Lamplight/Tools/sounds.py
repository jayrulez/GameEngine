#!/usr/bin/env python3
"""sounds.py: Lamplight's sounds, generated here (no third-party content) and imported through the
editor's MCP into Audio/.

Footsteps, three takes of each surface so a walk does not repeat (Footsteps.as picks one by the
physical material under the foot):
- Audio/StepWood1..3: a heel on boards, a hollow knock (a low ring and a short click).
- Audio/StepStone1..3: a hard tap on flags, a sharp click with a little ring.
- Audio/StepGravel1..3: a crunch, a scatter of tiny clicks under noise.
- Audio/StepGrass1..3: a soft brush of noise.
"""
import math, os, random, struct, sys, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

RATE = 22050
OUT = os.path.join(HERE, "generated", "Sounds")


def write(path, samples, level=0.8):
    peak = max(1e-6, max(abs(v) for v in samples))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * level * 32767)) for v in samples))


def lowpass(samples, k):
    out, y = [], 0.0
    for v in samples:
        y += k * (v - y)
        out.append(y)
    return out


def wood(rnd):
    n = int(0.22 * RATE)
    f1, f2 = 120 * rnd.uniform(0.9, 1.1), 260 * rnd.uniform(0.9, 1.1)
    click = lowpass([rnd.uniform(-1, 1) for _ in range(n)], 0.35)
    return [math.exp(-t / (0.035 * RATE)) * (math.sin(2 * math.pi * f1 * t / RATE)
            + 0.4 * math.sin(2 * math.pi * f2 * t / RATE)) + 0.5 * click[t] * math.exp(-t / (0.006 * RATE))
            for t in range(n)]


def stone(rnd):
    n = int(0.16 * RATE)
    ring = 2400 * rnd.uniform(0.9, 1.15)
    noise = [rnd.uniform(-1, 1) for _ in range(n)]
    high = [noise[t] - (noise[t - 1] if t else 0.0) for t in range(n)]  # a crude high-pass
    return [high[t] * math.exp(-t / (0.004 * RATE)) + 0.15 * math.sin(2 * math.pi * ring * t / RATE)
            * math.exp(-t / (0.02 * RATE)) + 0.3 * math.sin(2 * math.pi * 90 * t / RATE) * math.exp(-t / (0.02 * RATE))
            for t in range(n)]


def gravel(rnd):
    n = int(0.26 * RATE)
    out = [0.0] * n
    for _ in range(70):  # the stones shifting, each a tiny click
        at = int(rnd.betavariate(2, 4) * n * 0.8)
        amp = rnd.uniform(0.3, 1.0)
        for t in range(int(0.003 * RATE)):
            if at + t < n:
                out[at + t] += amp * rnd.uniform(-1, 1) * math.exp(-t / (0.0008 * RATE))
    hiss = lowpass([rnd.uniform(-1, 1) for _ in range(n)], 0.5)
    env = lambda t: math.sin(math.pi * min(1.0, t / (0.9 * n))) ** 2
    return [out[t] + 0.25 * hiss[t] * env(t) for t in range(n)]


def grass(rnd):
    n = int(0.24 * RATE)
    brush = lowpass(lowpass([rnd.uniform(-1, 1) for _ in range(n)], 0.25), 0.4)
    return [brush[t] * math.sin(math.pi * t / n) ** 1.5 for t in range(n)]


SURFACES = {"Wood": (wood, 0.8), "Stone": (stone, 0.7), "Gravel": (gravel, 0.75), "Grass": (grass, 0.5)}


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, (make, level) in SURFACES.items():
        for take in (1, 2, 3):
            path = os.path.join(OUT, "Step%s%d.wav" % (name, take))
            write(path, make(random.Random(sum(map(ord, name)) * 10 + take)), level)  # the same takes each run
            guid = mcp("asset_import", {"source": path, "group": "Audio", "importer": "Audio"})["guid"]
            print("Step%s%d" % (name, take), guid)
    print(mcp("asset_cook", {}))


if __name__ == "__main__":
    main()
