#!/usr/bin/env python3
"""course.py <course>: a course's scene, written through the editor's MCP (scene_write).

Needs terrain.py <course> and importall.py <course> first: it places what they made. The scene:
- the sun;
- the mountain: the terrain, its heightfield as a static body, and the vegetation (pines on the
  forest floor's splat layer, rocks scattered thinly on open snow);
- the course line: a spline down the middle of the course (terrain.py's points, with Catmull-Rom
  handles, stored as written);
- the rider (Board.as on a character, the modelled rider and board under it) at the top gate;
- the chase camera (FollowCamera.as).
"""
import json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import Doc, mcp, yaw, num, vec, component_removed, component_added
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
CAMERA = asset("ScriptClassAsset", "FollowCamera")
RIDER_MODEL = next(a["guid"] for a in ASSETS if a["type"] == "PrefabDocument"
                   and a.get("group") == "Models/Rider/RiderModel")
RIDER_GRAPH = asset("AnimationGraphAsset", "RiderGraph")


def rider_ops():
    """The rider model's import plays one clip; the board drives an animation graph (graph.py)
    instead. Its prefab's root loses the clip animator, and the skinned mesh gains the graph (an
    animator with no mesh entities of its own feeds its own entity's mesh)."""
    import xml.etree.ElementTree as ET
    root = ET.fromstring(mcp("prefab_read", {"guid": RIDER_MODEL})["xml"])
    animator = mesh_owner = skeleton = None
    for comp in root.find("array[@name='components']"):
        kind = comp.find("string[@name='type']").text
        owner = comp.find("string[@name='owner']").text
        if kind == "skeletal_animation":
            animator = owner
            skeleton = comp.find("object[@name='data']/string[@name='skeleton']").text
        elif kind == "mesh":
            mesh_owner = owner
    return [component_removed(animator, "skeletal_animation"),
            component_added(mesh_owner, "animation_graph", skeleton=skeleton, graph=RIDER_GRAPH)]

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
    rider = d.entity("Rider", (top[0], top[1] + 1.0, top[2]), yaw(heading))
    d.add(rider, "physics.Character", radius=0.35, halfHeight=0.55, maxSlopeDegrees=60.0)
    d.script(rider, (BOARD, {"course": ("entity", course), "autopilot": AUTOPILOT,
                             "trackMark": ("asset", asset("PrefabDocument", "TrackMark", "Prefabs"))}))
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

    cam = d.entity("Camera", (top[0], top[1] + 4.0, top[2] - 8.0))
    d.add(cam, "camera", farZ=1200.0, fovYRadians=1.05)
    d.script(cam, (CAMERA, {"target": ("entity", rider), "distance": 5.5, "height": 2.0, "lookHeight": 0.9}))
    return d


scenes = [a for a in ASSETS if a["type"] == "SceneDocument" and a["name"] == name]
doc = build()
guid = doc.write(scenes[0]["guid"] if scenes else None, group="Scenes")
print(name, guid)
