#!/usr/bin/env python3
"""terrain.py <course> [out dir]: a course's mountain, generated: the files asset_import takes.

Writes, into <out dir> (default Tools/generated/<course>):
- <Course>Height.png: the heightfield, 16-bit grey, row = world Z, column = world X, centred on
  the origin (the engine's heightfield convention), heights MIN_Y..MAX_Y.
- <Course>Splat.png: the splatmap, RGBA = the base snow's share, then palette layers 0..2:
  the packed course, rock on steep ground, the forest floor the pines grow on.
- Snow.png, Course.png, Rock.png, ForestSnow.png: the layers' textures (generated noise, no
  third-party content).
- <Course>.json: the numbers the scene generator places things by: the heightfield's size and
  range, and the course line (points down the middle, on the snow).

The course is carved into the mountain rather than painted on it: the slope falls from the top
gate to the finish, the course runs down a shallow valley that bends left and right, the ground
rises away from it, and the trees keep off it. One generator keeps the terrain and the course in
step.
"""
import json, math, os, random, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

# P0 (Documentation/Specs/snowline.md): a 513 x 513 heightfield at 1 m, heights 0 to 120 m, a
# 400 m course from the top gate to the finish.
COURSES = {
    "Meadow": dict(size=513, world=513.0, min_y=0.0, max_y=120.0, top_z=-200.0, bottom_z=200.0,
                   bend_amplitude=30.0, bend_length=160.0, seed=7),
}
SPLAT_SIZE = 512
TEXTURE_SIZE = 256


def smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


class Mountain:
    def __init__(self, spec):
        self.spec = spec
        rnd = random.Random(spec["seed"])
        # A few long, low swells for the open snow, away from the course.
        self.swells = [(rnd.uniform(0.01, 0.035), rnd.uniform(0.01, 0.035), rnd.uniform(0, 6.3), rnd.uniform(0.6, 1.6))
                       for _ in range(6)]

    def course_x(self, z):
        s = self.spec
        return s["bend_amplitude"] * math.sin(math.pi * (z - s["top_z"]) / s["bend_length"])

    def height(self, x, z):
        s = self.spec
        half = s["world"] / 2.0
        # The fall line: high at -Z, low at +Z, a little steeper near the top.
        t = (half - z) / s["world"]
        h = 5.0 + 90.0 * t ** 1.25  # leaves room under MAX_Y for the valley sides
        # Away from the course the ground rises into a broad valley.
        d = abs(x - self.course_x(z))
        h += min(d, 140.0) ** 2 * 0.0011
        # Swells on the open snow, none on the course itself.
        swell = sum(a * math.sin(fx * x + fz * z * 1.3 + p) for fx, fz, p, a in self.swells)
        h += swell * smoothstep(12.0, 35.0, d) * 2.0
        # The course: a shallow groove, so a rider sits in it.
        h -= 0.8 * (1.0 - smoothstep(4.0, 10.0, d))
        return max(s["min_y"], min(s["max_y"], h))


def noise_texture(path, base, spread, seed, streaks=False):
    rnd = random.Random(seed)
    img = Image.new("RGB", (TEXTURE_SIZE, TEXTURE_SIZE))
    px = img.load()
    for y in range(TEXTURE_SIZE):
        row = rnd.uniform(-1, 1) if streaks else 0.0
        for x in range(TEXTURE_SIZE):
            n = rnd.uniform(-1, 1) * 0.6 + row * 0.4
            px[x, y] = tuple(max(0, min(255, int(round((c + n * spread) * 255)))) for c in base)
    img.save(path)


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "Meadow"
    spec = COURSES[name]
    out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "generated", name))
    os.makedirs(out, exist_ok=True)
    m = Mountain(spec)
    size, world, half = spec["size"], spec["world"], spec["world"] / 2.0
    span = spec["max_y"] - spec["min_y"]

    def grid_to_world(g):
        return (g / (size - 1) - 0.5) * world

    # The heightfield.
    heights = [[m.height(grid_to_world(gx), grid_to_world(gz)) for gx in range(size)] for gz in range(size)]
    hf = Image.new("I;16", (size, size))
    hf.putdata([int(round((heights[gz][gx] - spec["min_y"]) / span * 65535)) for gz in range(size) for gx in range(size)])
    hf.save(os.path.join(out, name + "Height.png"))

    # The splatmap, from the heights' slope and the distance to the course.
    rnd = random.Random(spec["seed"] + 1)
    patches = [(rnd.uniform(-half, half), rnd.uniform(-half, half), rnd.uniform(18, 45)) for _ in range(70)]
    splat = Image.new("RGBA", (SPLAT_SIZE, SPLAT_SIZE))
    px = splat.load()
    cell = world / (size - 1)
    for sy in range(SPLAT_SIZE):
        z = (sy + 0.5) / SPLAT_SIZE * world - half
        gz = min(size - 2, int((z + half) / cell))
        for sx in range(SPLAT_SIZE):
            x = (sx + 0.5) / SPLAT_SIZE * world - half
            gx = min(size - 2, int((x + half) / cell))
            dhdx = (heights[gz][gx + 1] - heights[gz][gx]) / cell
            dhdz = (heights[gz + 1][gx] - heights[gz][gx]) / cell
            slope = math.degrees(math.atan(math.hypot(dhdx, dhdz)))
            d = abs(x - m.course_x(z))
            course = 1.0 - smoothstep(6.0, 9.0, d)
            rock = smoothstep(28.0, 36.0, slope) * (1.0 - course)
            forest = 0.0
            if d > 20.0:
                for px_, pz, r in patches:
                    forest = max(forest, 1.0 - smoothstep(r * 0.6, r, math.hypot(x - px_, z - pz)))
            forest *= smoothstep(20.0, 30.0, d) * (1.0 - rock)
            base = max(0.0, 1.0 - course - rock - forest)
            total = base + course + rock + forest
            px[sx, sy] = tuple(int(round(v / total * 255)) for v in (base, course, rock, forest))
    splat.save(os.path.join(out, name + "Splat.png"))

    # The layers' textures: plain, light noise; the course a little compacted, in streaks.
    noise_texture(os.path.join(out, "Snow.png"), (0.93, 0.95, 0.99), 0.025, 11)
    noise_texture(os.path.join(out, "Course.png"), (0.83, 0.87, 0.94), 0.03, 12, streaks=True)
    noise_texture(os.path.join(out, "Rock.png"), (0.36, 0.37, 0.40), 0.08, 13)
    noise_texture(os.path.join(out, "ForestSnow.png"), (0.86, 0.89, 0.93), 0.035, 14)

    # The course line: every 20 m down the middle, a little above the snow.
    points = []
    z = spec["top_z"]
    while z <= spec["bottom_z"] + 1e-3:
        x = m.course_x(z)
        points.append([round(x, 3), round(m.height(x, z) + 0.05, 3), round(z, 3)])
        z += 20.0
    length = sum(math.dist(a, b) for a, b in zip(points, points[1:]))
    info = dict(name=name, size=size, world=world, min_y=spec["min_y"], max_y=spec["max_y"],
                course=points, course_length=round(length, 1))
    json.dump(info, open(os.path.join(out, name + ".json"), "w"), indent=1)
    print(name, "written to", out, "- course", round(length, 1), "m, drop",
          round(points[0][1] - points[-1][1], 1), "m")


main()
