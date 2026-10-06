#!/usr/bin/env python3
"""course.py <course>: a course's scene, written through the editor's MCP (scene_write).

Needs terrain.py <course> and importall.py <course> first: it places what they made. The scene:
- the sun;
- the mountain: the terrain, its heightfield as a static body, and the vegetation (pines on the
  forest floor's splat layer, rocks scattered thinly on open snow);
- the course line: a spline down the middle of the course (terrain.py's points, with Catmull-Rom
  handles, stored as written);
- the rider (Board.as on a character, the modelled rider and board under it) at the top gate;
- the slalom gates (Gate.as on each, Prefabs/GateRed and GateBlue in turn under them) every
  GATE_SPACING metres down the course, swinging side to side of the line, standing on the snow
  (terrain.py's mountain), and the finish (Finish.as, Prefabs/FinishLine) at the line's end;
- the gems (Gem.as on each, the Gem model under it): a row of GEM_ROW between each pair of gates,
  GEM_OFFSET metres out on the side of the gate before them, past the packed course, so taking
  them means holding a wider line than the gates ask for;
- the medal ghosts (Ghost.as, a path_follow on the course line, the rider model under it in a
  medal's see-through colour, ghosts.py's materials): one per medal, each riding to the finish at
  its medal's time;
- the player's ghost (PlayerGhost.as, the rider model under it in ghosts.py's GhostPlayer): the
  course's best run, saved, ridden again beside the player; hidden until there is one;
- the kickers (Models/Props/KickerModel, with its collision) on the course line between gates;
- the chase camera (FollowCamera.as).
"""
import json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import Doc, mcp, yaw, num, vec, component_removed, component_added, component_modified, settings, pitch
from look import look

name = sys.argv[1] if len(sys.argv) > 1 else "Meadow"
# course.py <course> autopilot: the rider steers itself (playtests and measurements).
AUTOPILOT = "autopilot" in sys.argv[2:]
info = json.load(open(os.path.join(HERE, "generated", name, name + ".json")))
ASSETS = mcp("asset_list", {})["assets"]


def asset(asset_type, asset_name, group=None):
    for a in ASSETS:
        if a["type"] == asset_type and a["name"] == asset_name and (group is None or a.get("group") == group):
            return a["guid"]
    raise SystemExit("no %s %s%s" % (asset_type, asset_name, " in " + group if group else ""))


def primitive(label):
    """A primitive mesh (Meshes/<label>), made the first time."""
    for a in ASSETS:
        if a["type"] == "StaticMeshAsset" and a["name"] == label:
            return a["guid"]
    guid = mcp("asset_create", {"creator": label, "name": label})["guid"]
    ASSETS.append({"type": "StaticMeshAsset", "name": label, "guid": guid})
    return guid


def model_materials(model):
    """An imported model's materials in its mesh's slot order, as its prefab's mesh lists them."""
    import xml.etree.ElementTree as ET
    prefab = next(a["guid"] for a in ASSETS if a["type"] == "PrefabDocument"
                  and a.get("group") == "Models/Props/%sModel" % model)
    root = ET.fromstring(mcp("prefab_read", {"guid": prefab})["xml"])
    slots = root.find(".//array[@name='materials']")
    return [e.text for e in slots] if slots is not None else []


TERRAIN = asset("TerrainAsset", name + "Terrain")
HEIGHTFIELD = asset("HeightfieldAsset", name + "Height")
PINE = asset("StaticMeshAsset", "PineModel.2")
ROCK = asset("StaticMeshAsset", "RockModel.2")
PINE_MATERIALS = model_materials("Pine")
ROCK_MATERIALS = model_materials("Rock")
BOARD = asset("ScriptClassAsset", "Board")
GATE = asset("ScriptClassAsset", "Gate")
FINISH = asset("ScriptClassAsset", "Finish")
GHOST = asset("ScriptClassAsset", "Ghost")
PLAYER_GHOST = asset("ScriptClassAsset", "PlayerGhost")
GEM = asset("ScriptClassAsset", "Gem")
GEM_MODEL = next(a["guid"] for a in ASSETS if a["type"] == "PrefabDocument"
                 and a.get("group", "") == "Models/Props/GemModel")
CAMERA = asset("ScriptClassAsset", "FollowCamera")
RIDER_MODEL = next(a["guid"] for a in ASSETS if a["type"] == "PrefabDocument"
                   and a.get("group") == "Models/Rider/RiderModel")
RIDER_GRAPH = asset("AnimationGraphAsset", "RiderGraph")


GATE_SPACING = 45.0   # metres down the course between gates
GATE_FIRST = 40.0     # the first gate's distance from the top
GATE_SWING = 3.5      # how far each gate stands off the course line, side to side (m)
FINISH_BEFORE = 6.0   # the finish's distance before the line's end (m)
# Each course's medal times (s, penalties included): gold, silver, bronze. Snowline.as judges a
# run by them and the medal ghosts will ride at their pace. Meadow's: the autopilot, which rides
# the course line without tucking and misses two gates, finishes in 35.7 s (31.7 s riding): a
# bronze. Silver asks for the gates; gold for the gates and a tucked line.
MEDALS = {"Meadow": (30.0, 34.0, 40.0)}
# The kickers: each midway between two gates (the gap after gate KICKER_GAPS[i]), on the course
# line, tilted to the slope under it. Meadow has one: the course that teaches carving and flags
# has one jump to learn the air on.
KICKERS = {"Meadow": (3,)}
KICKER_LENGTH = 7.8 # the kicker's base, foot to back (blender/props.py: run + table + back), m
KICKER_SINK = 0.15  # how far its foot sits in the snow, so no edge stands proud of it (m)
GEM_ROW = 3           # gems in a row
GEM_GAP = 4.0         # metres down the course between a row's gems
GEM_OFFSET = 11.0     # how far a row stands off the course line (m): the packed course is 9 m
GEM_HEIGHT = 1.0      # a gem's centre above the snow (m): about the rider's centre


def along_course(points, distance):
    """The course line `distance` metres from its top: the point and the heading (radians, 0 runs
    toward +Z), along the polyline."""
    for a, b in zip(points, points[1:]):
        step = math.dist(a, b)
        if distance <= step or b is points[-1]:
            t = min(1.0, distance / step) if step > 0 else 0.0
            p = [a[i] + (b[i] - a[i]) * t for i in range(3)]
            return p, math.atan2(b[0] - a[0], b[2] - a[2])
        distance -= step
    return list(points[-1]), 0.0


def gates(d, points, length):
    """The gates and the finish, each an entity carrying its script with its prefab under it."""
    import terrain
    mountain = terrain.Mountain(terrain.COURSES[name])
    prefabs = {a["name"]: a["guid"] for a in ASSETS if a["type"] == "PrefabDocument" and a.get("group") == "Prefabs"}
    index = 0
    distance = GATE_FIRST
    while distance < length - FINISH_BEFORE - 20.0:
        p, heading = along_course(points, distance)
        side = 1 if index % 2 == 0 else -1
        x = p[0] + math.cos(heading) * GATE_SWING * side  # +X across a heading of 0
        z = p[2] - math.sin(heading) * GATE_SWING * side
        e = d.entity("Gate%d" % index, (x, mountain.height(x, z), z), yaw(math.degrees(heading)))
        d.script(e, (GATE, {"index": index, "heading": heading, "halfWidth": 4.0}))
        d.instance(prefabs["GateRed" if index % 2 == 0 else "GateBlue"], parent=e)
        index += 1
        distance += GATE_SPACING
    gems(d, points, mountain, index)
    kickers(d, points, mountain)
    p, heading = along_course(points, length - FINISH_BEFORE)
    e = d.entity("Finish", (p[0], mountain.height(p[0], p[2]), p[2]), yaw(math.degrees(heading)))
    gold, silver, bronze = MEDALS[name]
    d.script(e, (FINISH, {"heading": heading, "gold": gold, "silver": silver, "bronze": bronze}))
    d.instance(prefabs["FinishLine"], parent=e)
    return index


def gems(d, points, mountain, gate_count):
    """A row of gems in each gap between gates, out on the side of the gate before it."""
    count = 0
    for gap in range(gate_count - 1):
        side = 1 if gap % 2 == 0 else -1  # the side gate `gap` stands on (gates() swings the same way)
        middle = GATE_FIRST + GATE_SPACING * (gap + 0.5)
        for k in range(GEM_ROW):
            p, heading = along_course(points, middle + (k - (GEM_ROW - 1) / 2) * GEM_GAP)
            x = p[0] + math.cos(heading) * GEM_OFFSET * side
            z = p[2] - math.sin(heading) * GEM_OFFSET * side
            e = d.entity("Gem%d" % count, (x, mountain.height(x, z) + GEM_HEIGHT, z))
            d.script(e, (GEM, {}))
            d.instance(GEM_MODEL, parent=e)
            count += 1
    print("gems", count)


def qmul(a, b):
    """Quaternions (xyzw): `a` after `b`."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def kickers(d, points, mountain):
    """Each kicker on the course line midway between two gates, facing down it, its base tilted to
    the slope it stands on (the drop over its base's length)."""
    prefab = next(a["guid"] for a in ASSETS if a["type"] == "PrefabDocument"
                  and a.get("group", "") == "Models/Props/KickerModel")
    for i, gap in enumerate(KICKERS.get(name, ())):
        middle = GATE_FIRST + GATE_SPACING * (gap + 0.5)
        p, heading = along_course(points, middle)
        q, _ = along_course(points, middle + KICKER_LENGTH)
        drop = mountain.height(p[0], p[2]) - mountain.height(q[0], q[2])
        slope = math.degrees(math.atan2(drop, KICKER_LENGTH))
        e = d.entity("Kicker%d" % i, (p[0], mountain.height(p[0], p[2]) - KICKER_SINK, p[2]),
                     qmul(yaw(math.degrees(heading)), pitch(slope)))
        d.instance(prefab, parent=e)


def aim_bone(name, share):
    """One AimIkBone, as a list element's fields (written flat in the array)."""
    return '<string name="bone">%s</string><f32 name="share">%r</f32>' % (name, share)


def rider_parts():
    """The rider model prefab's clip animator (its entity and skeleton) and skinned mesh (its
    entity, mesh and material slots)."""
    import xml.etree.ElementTree as ET
    root = ET.fromstring(mcp("prefab_read", {"guid": RIDER_MODEL})["xml"])
    parts = {}
    for comp in root.find("array[@name='components']"):
        kind = comp.find("string[@name='type']").text
        owner = comp.find("string[@name='owner']").text
        data = comp.find("object[@name='data']")
        if kind == "skeletal_animation":
            parts["animator"] = owner
            parts["skeleton"] = data.find("string[@name='skeleton']").text
        elif kind == "mesh":
            parts["meshOwner"] = owner
            parts["mesh"] = data.find("string[@name='mesh']").text
            parts["slots"] = len(list(data.find("array[@name='materials']")))
    return parts


def rider_ops(material=None):
    """The rider model's import plays one clip; the board drives an animation graph (graph.py)
    instead. Its prefab's root loses the clip animator, and the skinned mesh gains the graph (an
    animator with no mesh entities of its own feeds its own entity's mesh). With a `material`, the
    mesh takes it in every slot (a ghost)."""
    parts = rider_parts()
    ops = [component_removed(parts["animator"], "skeletal_animation"),
           component_added(parts["meshOwner"], "animation_graph", skeleton=parts["skeleton"], graph=RIDER_GRAPH)]
    if material:
        ops.append(component_modified(parts["meshOwner"], "mesh", mesh=parts["mesh"],
                                      materials=["<string>%s</string>" % material] * parts["slots"]))
    return ops


# The medal ghosts, in Ghost.as's order (0 gold, 1 silver, 2 bronze), each with its material.
GHOSTS = (("GhostGold", 0), ("GhostSilver", 1), ("GhostBronze", 2))


def ghosts(d, course, length):
    """A ghost per medal: on the course line from its top (path_follow, stopped until Ghost.as sets
    its pace), the rider model under it in the medal's material. The follower turns its -Z down
    the line; the model faces +Z, so it is turned about."""
    for label, medal in GHOSTS:
        e = d.entity(label, tuple(info["course"][0]))
        d.add(e, "path_follow", spline=course, speed=0.0, loop=False, playing=True, alignToTangent=True)
        d.script(e, (GHOST, {"medal": medal, "finishDistance": length - FINISH_BEFORE}))
        d.instance(RIDER_MODEL, rot=yaw(180), parent=e, ops=rider_ops(asset("MaterialAsset", label)))

# The sun: low and from the side, so the slope's relief reads.
SUN_ROT = (-0.4229, 0.2418, 0.1162, 0.8653)


def spline_points(points):
    """The course line's points with Catmull-Rom handles: each a sixth of the chord between its
    neighbours (Auto's rule), in and out mirrored. An array of structs is written flat, each
    element's fields one after another, as the engine's serializer writes it."""
    items = []
    for i, p in enumerate(points):
        a = points[max(0, i - 1)]
        b = points[min(len(points) - 1, i + 1)]
        h = [(b[k] - a[k]) / 6.0 for k in range(3)]
        items.append('<object name="position">%s</object><object name="in">%s</object>'
                     '<object name="out">%s</object><u8 name="mode">0</u8>' % (
                         "".join('<f32 name="%s">%s</f32>' % (k, num(float(v))) for k, v in zip("xyz", p)),
                         "".join('<f32 name="%s">%s</f32>' % (k, num(-float(v))) for k, v in zip("xyz", h)),
                         "".join('<f32 name="%s">%s</f32>' % (k, num(float(v))) for k, v in zip("xyz", h))))
    return items


def vegetation_layer(label, mesh, materials, placement, density, scale, slope, fade, splat_layer=0,
                     threshold=0.25, shadows=False):
    """One procedural layer (the component's ProceduralVegetationLayer, field by field); its
    materials one per mesh slot, as the model's prefab lists them."""
    return ('<string name="name">%s</string><string name="mesh">%s</string>'
            '<array name="materials" count="%d">%s</array>'
            '<object name="scaleRange"><f32 name="x">%s</f32><f32 name="y">%s</f32></object>'
            '<f32 name="maxSlopeDegrees">%s</f32>'
            '<object name="heightRange"><f32 name="x">-1000000</f32><f32 name="y">1000000</f32></object>'
            '<bool name="alignToNormal">false</bool><f32 name="fadeStart">%s</f32><f32 name="fadeEnd">%s</f32>'
            '<bool name="castShadows">%s</bool><u32 name="maxInstancesPerChunk">4096</u32><bool name="visible">true</bool>'
            '<u8 name="placement">%d</u8><u32 name="splatLayer">%d</u32><f32 name="splatThreshold">%s</f32>'
            '<u32 name="maskPlane">0</u32><f32 name="density">%s</f32>' % (
                label, mesh, len(materials), "".join("<string>%s</string>" % m for m in materials), num(scale[0]), num(scale[1]), num(slope), num(fade[0]), num(fade[1]),
                num(shadows), placement, splat_layer, num(threshold), num(density)))


UNIFORM, SPLAT = 0, 1
BASE_LAYER = 0xFFFFFFFF  # the splat's unpainted base (the vegetation's kSplatBaseLayer)


def build():
    d = Doc(name)
    look(d)
    # Collision groups: 0 everything, 1 the gates' flags (gates.py's FLAG_GROUP), which collide with
    # nothing. A flag at its hinge's limit would stop the rider dead; Gate.as swings it instead.
    d.settings.append(settings("physics", groupNames=["<string>Default</string>", "<string>Flags</string>"],
                               groupCollides=["<u32>%d</u32>" % (0xFFFFFFFF & ~(1 << 1)), "<u32>0</u32>"]))
    sun = d.entity("Sun", rot=SUN_ROT)
    d.add(sun, "light", type=0, intensity=4.0, castsShadows=True)

    mountain = d.entity("Mountain")
    d.add(mountain, "terrain", terrain=TERRAIN)
    d.add(mountain, "physics.RigidBody", motion=0, layer=0, shape=5, heightfield=HEIGHTFIELD)
    d.add(mountain, "terrainVegetation", proceduralLayers=[
        # Pines where terrain.py laid the forest floor (palette layer 2), sparse enough to ride
        # between; rocks thinly everywhere the slope allows, the course's own splat aside.
        vegetation_layer("Pines", PINE, PINE_MATERIALS, SPLAT, 0.025, (0.8, 1.3), 30.0, (250.0, 320.0), splat_layer=2,
                         threshold=0.5, shadows=True),
        vegetation_layer("Rocks", ROCK, ROCK_MATERIALS, SPLAT, 0.0015, (0.6, 1.5), 40.0, (120.0, 160.0), splat_layer=BASE_LAYER,
                         threshold=0.9),
    ])

    course = d.entity("Course")
    d.add(course, "spline", points=spline_points(info["course"]))

    top = info["course"][0]
    nxt = info["course"][1]
    heading = math.degrees(math.atan2(nxt[0] - top[0], nxt[2] - top[2]))
    # The capsule's centre 0.9 m up (half height 0.55 + radius 0.35): on the snow, not above it. A
    # rider placed higher drops onto the slope as the run starts, and the board turns the drop into
    # speed down it, which a restarted run (put back on the snow) never had.
    rider = d.entity("Rider", (top[0], top[1] + 0.9, top[2]), yaw(heading))
    d.add(rider, "physics.Character", radius=0.35, halfHeight=0.55, maxSlopeDegrees=60.0)
    d.script(rider, (BOARD, {"course": ("entity", course), "autopilot": AUTOPILOT,
                             "trackMark": ("asset", asset("PrefabDocument", "TrackMark", "Prefabs"))}))
    # The rider's head looks down the course line ahead (Board.as sets the point each frame; the
    # next gate once the course has gates): an aim on the rider, which drives the graph below it.
    # The rig's spine and head face +Z, up +Y (blender/rider.py), the aim's defaults.
    d.add(rider, "aim_ik", bones=[aim_bone("spine", 0.3), aim_bone("head", 1.0)], maxAngle=70.0, fadeSeconds=0.5)
    # The model (blender/rider.py): its origin is the snow under the board, the character's is its
    # capsule's centre, 0.9 m up.
    d.instance(RIDER_MODEL, (0, -0.9, 0), parent=rider, ops=rider_ops())
    # The board's snow (particles.py), at the board: spray off its edge, powder as it lands.
    for fx in ("Spray", "Powder"):
        e = d.entity(fx, (0, -0.85, 0), parent=rider)
        d.add(e, "particle_effect", effect=asset("ParticleEffectAsset", fx))
    # The wind in the rider's ears (sounds.py): looping, not placed, silent until Board.as sets
    # its volume by the speed.
    wind = d.entity("Wind", parent=rider)
    d.add(wind, "audio.Source", clip=asset("AudioClipAsset", "Wind"), loop=True, spatial=False, autoPlay=True,
          volume=0.0)

    print("gates", gates(d, info["course"], info["course_length"]))
    ghosts(d, course, info["course_length"])
    # The player's best run: it rides where the rider rode, so it sits as the rider does, the model
    # 0.9 m below the capsule's centre it was recorded at.
    player_ghost = d.entity("PlayerGhost", (top[0], top[1] + 0.9, top[2]), yaw(heading))
    d.script(player_ghost, (PLAYER_GHOST, {}))
    d.instance(RIDER_MODEL, (0, -0.9, 0), parent=player_ghost, ops=rider_ops(asset("MaterialAsset", "GhostPlayer")))

    cam = d.entity("Camera", (top[0], top[1] + 4.0, top[2] - 8.0))
    d.add(cam, "camera", farZ=1200.0, fovYRadians=1.05)
    d.script(cam, (CAMERA, {"target": ("entity", rider), "distance": 5.5, "height": 2.0, "lookHeight": 0.9}))
    return d


scenes = [a for a in ASSETS if a["type"] == "SceneDocument" and a["name"] == name]
doc = build()
guid = doc.write(scenes[0]["guid"] if scenes else None, group="Scenes")
print(name, guid)
