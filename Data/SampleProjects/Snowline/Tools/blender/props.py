"""Snowline's props: the mountain's trees and rocks, modelled in Blender on the shared kit.

    blender --background --factory-startup --python props.py -- <out dir> [preview] [names]

Writes <out dir>/<Name>Model.glb for each (static meshes, no rig) and, with `preview`, a PNG of
each. The vegetation scatter instances these by the thousand, so they are low in polygons: a
tree is a trunk and a few cones, each with a cap of snow.
"""
import math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit3d
from kit3d import P, tube, ball

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = os.path.abspath(ARGS[0] if ARGS else "props-out")
PREVIEW = "preview" in ARGS
WANTED = [a for a in ARGS[1:] if a != "preview"]

kit3d.PALETTE.update({
    "Bark": (0.20, 0.12, 0.07), "Needles": (0.05, 0.16, 0.09), "NeedlesDark": (0.03, 0.11, 0.07),
    "Snow": (0.86, 0.90, 0.95), "Rock": (0.32, 0.33, 0.36),
})


def pine():
    """A snowy pine about 6 m tall: a trunk, four tiers of needles narrowing to the top, each
    with snow lying on its upper half."""
    tube("Trunk", P(0, 0, 0), P(0, 0, 1.6), 0.16, "Bark", "root", 8, 0.12)
    tiers = ((1.0, 2.6, 1.55), (2.0, 3.6, 1.2), (3.0, 4.6, 0.9), (4.0, 6.0, 0.6))
    for i, (bottom, top, radius) in enumerate(tiers):
        tube("Tier", P(0, 0, bottom), P(0, 0, top), radius, "Needles" if i % 2 == 0 else "NeedlesDark",
             "root", 10, 0.02)
        # The snow: a cone over the tier's upper part, a little proud of the needles.
        snow_bottom = bottom + (top - bottom) * 0.45
        snow_radius = radius * (top - snow_bottom) / (top - bottom) + 0.05
        tube("SnowCap", P(0, 0, snow_bottom), P(0, 0, top + 0.04), snow_radius, "Snow", "root", 10, 0.02)


def rock():
    """A boulder about 1.4 m across, half sunk in the snow, with snow on its top."""
    ball("Rock", P(0, 0, 0.2), 0.75, "Rock", scale=(1.0, 0.8, 0.6), segments=(10, 7))
    ball("RockSnow", P(0.05, 0, 0.52), 0.55, "Snow", scale=(1.0, 0.8, 0.3), segments=(10, 6), cut_below=0.0)


MODELS = {"Pine": pine, "Rock": rock}


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in (WANTED or list(MODELS)):
        kit3d.reset()
        MODELS[name]()
        if PREVIEW:
            cam = kit3d.studio()
            eye, target = (P(-6, 9, 4), P(0, 0, 3)) if name == "Pine" else (P(-2, 3, 1.5), P(0, 0, 0.3))
            kit3d.shoot(cam, os.path.join(OUT, name + ".png"), eye, target)
        kit3d.export_static(os.path.join(OUT, name + "Model.glb"), name + "Model")
        print("written", name)


main()
