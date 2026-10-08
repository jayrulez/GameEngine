"""Lamplight's thief: modelled, rigged and animated in Blender.

    blender --background --factory-startup --python thief.py -- <out dir> [preview]

Writes <out dir>/Thief.glb (one skinned mesh, its armature and the clips below) and, with
`preview`, PNG renders. The thief stands on the ground at its origin facing the engine's +Z, about
1.7 m tall: hooded and masked, a dark tunic belted with a pouch, gloves and soft boots.

The clips, each a loop whose stride matches the speed Thief.as moves at, so the feet do not slide:
- Idle (2 s): standing, breathing.
- Walk (1 s): two steps of 0.8 m, 1.6 m/s.
- Sneak (1.2 s): crouched, two steps of 0.6 m, 1.0 m/s.
- Run (0.6 s): leaning in, two steps of 1.35 m, 4.5 m/s.
- Crouch (2 s): crouched still, breathing.

The arms are upper arm, forearm and hand: the chain the two-bone IK puts on a lock (the hand's head
reaches the target). Every rotation is set from the bone's own direction (a swing turns the
bone toward or away from forward), as PaperKid's pedestrian does.
"""
import bpy, math, os, sys
from mathutils import Vector, Quaternion

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit3d
from kit3d import P, box, tube, ball

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = os.path.abspath(ARGS[0] if ARGS else "thief-out")
PREVIEW = "preview" in ARGS
FPS = 24

kit3d.PALETTE.update({
    "Cloth": (0.045, 0.05, 0.065), "ClothDark": (0.025, 0.027, 0.035), "Mask": (0.02, 0.02, 0.024),
    "Skin": (0.62, 0.42, 0.30), "Leather": (0.16, 0.09, 0.05), "Buckle": (0.55, 0.42, 0.20),
    "Eye": (0.08, 0.06, 0.06),
})
kit3d.ROUGHNESS.update({"Cloth": 0.9, "ClothDark": 0.9, "Mask": 0.95, "Leather": 0.55, "Buckle": 0.35})

# The joints at rest (standing, arms a little out), side +1 the thief's left (+X; it faces -Y).
HIP_Y = 0.90
HIP = lambda side: P(side * 0.10, 0, HIP_Y)
KNEE = lambda side: P(side * 0.105, 0.02, 0.50)
ANKLE = lambda side: P(side * 0.11, 0, 0.09)
TOE = lambda side: P(side * 0.11, 0.17, 0.04)
SHOULDER = lambda side: P(side * 0.20, 0, 1.38)
ELBOW = lambda side: P(side * 0.25, -0.02, 1.10)
HAND = lambda side: P(side * 0.27, 0.03, 0.86)
NECK = P(0, 0, 1.45)
HEAD = P(0, 0.01, 1.60)


def build():
    box("Pelvis", (0.30, 0.21, 0.18), P(0, 0, 0.93), "ClothDark", "hips", bevel=0.05)
    box("Torso", (0.36, 0.22, 0.50), P(0, 0, 1.18), "Cloth", "spine", bevel=0.07)
    box("Belt", (0.37, 0.23, 0.06), P(0, 0, 0.97), "Leather", "spine", bevel=0.02)
    box("Buckle", (0.06, 0.02, 0.045), P(0, 0.12, 0.97), "Buckle", "spine")
    box("Pouch", (0.11, 0.07, 0.1), P(0.15, 0.09, 0.92), "Leather", "hips", bevel=0.02)
    tube("Neck", NECK - Vector((0, 0, 0.03)), NECK + Vector((0, 0, 0.07)), 0.05, "ClothDark", "head")
    ball("Head", HEAD, 0.17, "Skin", "head", scale=(0.95, 0.95, 1.05), segments=(28, 16))
    # The hood over the head and the mask across the lower face; the eyes show between them.
    ball("Hood", HEAD + P(0, -0.02, 0.03), 0.19, "Cloth", "head", scale=(1.04, 1.06, 1.05), segments=(28, 16),
         cut_below=-0.15)
    ball("Mask", HEAD + P(0, 0.035, -0.06), 0.165, "Mask", "head", scale=(1.0, 0.98, 0.65), segments=(24, 12))
    for side in (-1, 1):
        ball("Eye", HEAD + P(side * 0.06, 0.15, 0.025), 0.022, "Eye", "head", segments=(12, 8))
    for side in (-1, 1):
        s = "L" if side > 0 else "R"
        sh, el, ha = SHOULDER(side), ELBOW(side), HAND(side)
        ball("Shoulder" + s, sh, 0.075, "Cloth", "upperarm_" + s, segments=(14, 10))
        tube("UpperArm" + s, sh, el, 0.058, "Cloth", "upperarm_" + s, 10, 0.05)
        ball("Elbow" + s, el, 0.048, "Cloth", "forearm_" + s, segments=(10, 8))
        tube("Forearm" + s, el, ha, 0.047, "Cloth", "forearm_" + s, 10, 0.04)
        ball("Hand" + s, ha + P(0, 0, -0.03), 0.05, "Leather", "hand_" + s, segments=(12, 8))
        hp, kn, an, toe = HIP(side), KNEE(side), ANKLE(side), TOE(side)
        tube("Thigh" + s, hp, kn, 0.078, "ClothDark", "thigh_" + s, 10, 0.064)
        ball("Knee" + s, kn, 0.062, "ClothDark", "shin_" + s, segments=(10, 8))
        tube("Shin" + s, kn, an, 0.06, "Leather", "shin_" + s, 10, 0.05)
        box("Boot" + s, (0.10, (toe - an).length + 0.1, 0.09), (an + toe) / 2 + Vector((0, 0, -0.005)), "Leather",
            "foot_" + s, bevel=0.035)


def build_armature():
    arm = bpy.data.armatures.new("ThiefRig")
    rig = bpy.data.objects.new("ThiefRig", arm)
    bpy.context.scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm.edit_bones

    def bone(name, head, tail, parent=None, connect=False):
        b = eb.new(name)
        b.head, b.tail, b.roll = head, tail, 0.0
        if parent:
            b.parent = eb[parent]
            b.use_connect = connect
        return b

    bone("root", Vector((0, 0, 0)), Vector((0, 0, 0.2)))
    bone("hips", P(0, 0, HIP_Y - 0.05), P(0, 0, HIP_Y + 0.08), "root")
    bone("spine", P(0, 0, HIP_Y + 0.08), NECK, "hips")
    bone("head", NECK, NECK + Vector((0, 0, 0.32)), "spine")
    for side in (-1, 1):
        s = "L" if side > 0 else "R"
        bone("upperarm_" + s, SHOULDER(side), ELBOW(side), "spine")
        bone("forearm_" + s, ELBOW(side), HAND(side), "upperarm_" + s, True)
        bone("hand_" + s, HAND(side), HAND(side) + P(0, 0, -0.08), "forearm_" + s, True)
        bone("thigh_" + s, HIP(side), KNEE(side), "hips")
        bone("shin_" + s, KNEE(side), ANKLE(side), "thigh_" + s, True)
        bone("foot_" + s, ANKLE(side), TOE(side), "shin_" + s, True)
    bpy.ops.object.mode_set(mode="OBJECT")
    for b in rig.pose.bones:
        b.rotation_mode = "QUATERNION"
    return rig


def swing_quat(rig, bone_name, degrees):
    """A turn of the bone in the thief's forward plane: positive turns it toward forward (Blender's
    -Y), about the axis across the body; given in the bone's own frame."""
    bone = rig.data.bones[bone_name]
    rest = bone.matrix_local.to_3x3()
    direction = rest.col[1].normalized()  # a bone's Y runs head to tail
    axis = direction.cross(Vector((0, -1, 0)))
    if axis.length < 1e-6:  # a bone already pointing forward (the foot): turn about X
        axis = Vector((1, 0, 0)) if direction.y < 0 else Vector((-1, 0, 0))
    return Quaternion(rest.inverted() @ axis.normalized(), math.radians(degrees))


# A gait: how far the legs and arms swing, how deep the crouch, how much the body leans and bobs.
# The crouch's numbers keep the feet under the hips: thighs forward by `thigh0`, knees bent by
# `knee0`, the hips dropped by what that takes off the leg's height.
LEG = (HIP(1) - KNEE(1)).length, (KNEE(1) - ANKLE(1)).length


def crouch_drop(thigh0, knee0):
    a, b = math.radians(thigh0), math.radians(thigh0 - knee0)
    return LEG[0] + LEG[1] - (LEG[0] * math.cos(a) + LEG[1] * math.cos(b))


GAITS = {
    #        frames, thigh swing, knee lift, arm swing, elbow, thigh0, knee0, lean, bob, breathing
    "Idle":   dict(frames=48, thigh=0, knee=0, arm=0, elbow=10, thigh0=0, knee0=4, lean=2, bob=0.0, breath=1),
    "Walk":   dict(frames=24, thigh=28, knee=45, arm=22, elbow=14, thigh0=0, knee0=6, lean=5, bob=0.025, breath=0),
    "Sneak":  dict(frames=29, thigh=20, knee=25, arm=8, elbow=50, thigh0=40, knee0=75, lean=24, bob=0.015, breath=0),
    "Run":    dict(frames=14, thigh=45, knee=90, arm=40, elbow=75, thigh0=8, knee0=20, lean=14, bob=0.05, breath=0),
    "Crouch": dict(frames=48, thigh=0, knee=0, arm=0, elbow=55, thigh0=45, knee0=85, lean=26, bob=0.0, breath=1),
}


def clip(rig, name, g):
    action = bpy.data.actions.new(name)
    rig.animation_data.action = action
    pb = rig.pose.bones
    drop = crouch_drop(g["thigh0"], g["knee0"])
    for f in range(1, g["frames"] + 2):
        t = (f - 1) / g["frames"]
        phase = 2 * math.pi * t
        for side, offset in ((1, 0.0), (-1, math.pi)):
            s = "L" if side > 0 else "R"
            p = phase + offset
            thigh = g["thigh0"] + g["thigh"] * math.sin(p)
            knee = g["knee0"] + g["knee"] * max(0.0, math.cos(p))
            pb["thigh_" + s].rotation_quaternion = swing_quat(rig, "thigh_" + s, thigh)
            pb["shin_" + s].rotation_quaternion = swing_quat(rig, "shin_" + s, -knee)
            # The foot level: every leg bone turns about the same axis across the body, so the foot
            # sits at thigh - knee + its own turn, and knee - thigh takes it back to level.
            pb["foot_" + s].rotation_quaternion = swing_quat(rig, "foot_" + s, knee - thigh)
            arm = -g["arm"] * math.sin(p) + g["thigh0"] * 0.4  # against the leg; forward in a crouch
            pb["upperarm_" + s].rotation_quaternion = swing_quat(rig, "upperarm_" + s, arm)
            pb["forearm_" + s].rotation_quaternion = swing_quat(rig, "forearm_" + s, g["elbow"])
            for bone in ("thigh_", "shin_", "foot_", "upperarm_", "forearm_"):
                pb[bone + s].keyframe_insert("rotation_quaternion", frame=f)
        breath = 0.006 * math.sin(phase) if g["breath"] else 0.0
        # Hips' local Y is up: the crouch's drop, then the bob over each planted leg (twice a cycle).
        pb["hips"].location = Vector((0, -drop + g["bob"] * (1 - math.cos(2 * phase)) / 2 + breath, 0))
        pb["hips"].keyframe_insert("location", frame=f)
        pb["spine"].rotation_quaternion = swing_quat(rig, "spine", g["lean"] + (1.5 * math.sin(phase) if g["breath"] else 0))
        pb["spine"].keyframe_insert("rotation_quaternion", frame=f)
        pb["head"].rotation_quaternion = swing_quat(rig, "head", -g["lean"] * 0.7)  # eyes stay ahead
        pb["head"].keyframe_insert("rotation_quaternion", frame=f)
    action.use_fake_user = True
    return action


def main():
    kit3d.reset()
    scene = bpy.context.scene
    scene.render.fps = FPS
    os.makedirs(OUT, exist_ok=True)
    build()
    rig = build_armature()
    body = kit3d.skin(rig, "Thief")
    rig.animation_data_create()
    actions = {name: clip(rig, name, g) for name, g in GAITS.items()}
    if PREVIEW:
        cam = kit3d.studio()
        target = Vector((0, 0, 0.8))
        rig.animation_data.action = None
        for b in rig.pose.bones:  # back to rest: unkeyed, the pose keeps the last clip's
            b.rotation_quaternion = Quaternion()
            b.location = Vector()
        kit3d.shoot(cam, os.path.join(OUT, "preview-front34.png"), P(-1.8, 2.4, 1.4), target)
        for name, frame in (("Walk", 7), ("Sneak", 8), ("Run", 4), ("Crouch", 1)):
            rig.animation_data.action = actions[name]
            scene.frame_set(frame)
            kit3d.shoot(cam, os.path.join(OUT, "preview-%s.png" % name), P(-3.2, 0.4, 1.0), target)
    kit3d.export_rigged(os.path.join(OUT, "Thief.glb"), rig, body)
    print("Thief written to", OUT)


main()
