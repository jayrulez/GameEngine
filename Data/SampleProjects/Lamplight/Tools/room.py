"""room.py: P0's lit room (Documentation/Specs/lamplight.md), written through the editor's MCP.

One room, 10 m by 8 m on the kit's 2 m grid (blender/manor.py, imported as Models/Manor/<Piece>):
wall pieces round it, a doorway in the middle of the south side, a post at each corner, and the
floor in waxed boards, so SSR shows the lamp and the lantern in it. No ceiling: the camera looks
down from above and behind, and the wall pieces between it and the thief fade (CutawayWall).
A table with an oil lamp on it (its point light 0.75 m over the tabletop, so the top is lit, not
clipped), a guard walking a loop round the room with his lantern (a spot light
with shadows, carried along a closed spline by path_follow), the thief by the door, and the camera
rig. Each piece's collider is its own unscaled entity, placed from the same sizes.

The guard is still a cylinder until his model. Run with the editor open on the
project and its MCP server on, after manor.py's pieces are imported and importscripts.py has run:
    python3 room.py
"""
import math, os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenegen import Doc, mcp, yaw, pitch, num, component_added, component_removed
from look import look
from materials import all_materials

GRID, ROOM_W, ROOM_D, WALL_H, WALL_T = 2.0, 10.0, 8.0, 3.0, 0.2  # the kit's grid and wall (manor.py)
DOOR_W, DOOR_H = 1.2, 2.2
LEAF_W, LEAF_H = 1.04, 2.12  # the door's leaf inside the frame (manor.py's Door)
TABLE = (-2.6, 0.0, -1.6)
GUARD_ROUTE = [(-3.0, 0.0, 2.5), (3.0, 0.0, 2.5), (3.0, 0.0, -2.5), (-3.0, 0.0, -2.5)]
GUARD_SPEED = 1.2  # m/s, a slow round

ASSETS = mcp("asset_list", {})["assets"]
THIEF_MODEL = next((a["guid"] for a in ASSETS if a["type"] == "PrefabDocument" and a.get("group") == "Models/Thief"),
                   None)


def mesh(name):
    found = [a["guid"] for a in ASSETS if a["type"] == "StaticMeshAsset" and a["name"] == name]
    if found:
        return found[0]
    creators = [c for c in mcp("asset_creators", {})["creators"] if c.get("label") == name]
    if not creators:
        raise SystemExit("no mesh or creator named " + name)
    return mcp("asset_create", {"creator": name, "name": name, "group": "Meshes"})["guid"]


def piece(name):
    """A kit piece's mesh and its material slots, read from its imported prefab."""
    prefab = [a["guid"] for a in ASSETS
              if a["type"] == "PrefabDocument" and a.get("group") == "Models/Manor/" + name]
    if not prefab:
        raise SystemExit("no kit piece %s (run blender/manor.py and import it into Models/Manor)" % name)
    root = ET.fromstring(mcp("prefab_read", {"guid": prefab[0]})["xml"])
    mesh_guid = root.find(".//string[@name='mesh']").text
    slots = [e.text for e in root.find(".//array[@name='materials']")]
    return mesh_guid, slots


def model_parts(prefab):
    """An imported model prefab's clip animator (its entity and skeleton) and skinned mesh owner."""
    root = ET.fromstring(mcp("prefab_read", {"guid": prefab})["xml"])
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
    return parts


def thief_ops():
    """The thief model's import plays one clip; Thief.as drives the graph instead (Snowline's rider
    does the same): the prefab's root loses the clip animator, and the skinned mesh gains the graph."""
    parts = model_parts(THIEF_MODEL)
    graph = [a["guid"] for a in ASSETS if a["type"] == "AnimationGraphAsset" and a["name"] == "ThiefGraph"]
    if not graph:
        raise SystemExit("no ThiefGraph (run graph.py first)")
    return [component_removed(parts["animator"], "skeletal_animation"),
            component_added(parts["meshOwner"], "animation_graph", skeleton=parts["skeleton"], graph=graph[0])]


def asset(asset_type, name):
    found = [a["guid"] for a in ASSETS if a["type"] == asset_type and a["name"] == name]
    if not found:
        raise SystemExit("no %s %s (run importscripts.py first)" % (asset_type, name))
    return found[0]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def spline_points(points):
    """A closed loop's points with Catmull-Rom handles (each a sixth of the chord between its
    neighbours, wrapping round), written flat as the engine's serializer writes an array of structs."""
    items = []
    n = len(points)
    for i, p in enumerate(points):
        a, b = points[(i - 1) % n], points[(i + 1) % n]
        h = [(b[k] - a[k]) / 6.0 for k in range(3)]
        f = lambda v: "".join('<f32 name="%s">%s</f32>' % (k, num(float(x))) for k, x in zip("xyz", v))
        items.append('<object name="position">%s</object><object name="in">%s</object>'
                     '<object name="out">%s</object><u8 name="mode">0</u8>' % (f(p), f([-x for x in h]), f(h)))
    return items


def build():
    mats = all_materials()
    cylinder = mesh("Cylinder")
    kit = {n: piece(n) for n in ("Wall", "Doorway", "Door", "Post", "Floor", "Table", "OilLamp")}
    d = Doc("Room")
    look(d)

    def thing(name, m, mat, pos, scale, rot=(0, 0, 0, 1), parent=None):
        e = d.stable_id("entity")
        d.entity(name, pos, rot, scale, parent=parent, eid=e)
        d.add(e, "mesh", mesh=m, materials=["<string>%s</string>" % mats[mat]])
        return e

    def place(name, kind, pos, rot=(0, 0, 0, 1), parent=None):
        """A kit piece, its materials as imported."""
        m, slots = kit[kind]
        e = d.stable_id("entity")
        d.entity(name, pos, rot, parent=parent, eid=e)
        d.add(e, "mesh", mesh=m, materials=["<string>%s</string>" % g for g in slots])
        return e

    def solid(name, pos, size):
        """A static box collider of its own, unscaled, so the shape is the size it says."""
        e = d.stable_id("entity")
        d.entity(name + "Collider", pos, eid=e)
        d.add(e, "physics.RigidBody", motion=0, layer=0, shape=0,
              halfExtents={"x": size[0] / 2, "y": size[1] / 2, "z": size[2] / 2})

    # The moon: a faint blue key from high in the west, so the room is not black between the lamps.
    moon = d.stable_id("entity")
    d.entity("Moon", rot=quat_mul(yaw(-60.0), pitch(-50.0)), eid=moon)
    d.add(moon, "light", type=0, color={"r": 0.55, "g": 0.65, "b": 1.0, "a": 1.0}, intensity=0.25,
          castsShadows=True)

    # The floor: a tile per grid cell, and one collider under the room and the yard outside the door.
    hw, hd = ROOM_W / 2, ROOM_D / 2
    for i in range(int(ROOM_W / GRID)):
        for j in range(int(ROOM_D / GRID)):
            place("Floor%d_%d" % (i, j), "Floor", (-hw + GRID * (i + 0.5), 0, -hd + GRID * (j + 0.5)))
    solid("Floor", (0, -0.25, 0), (ROOM_W + 6, 0.5, ROOM_D + 6))

    # The walls: a piece per grid edge (the doorway mid-south), a post at each corner; each piece
    # fades by its outward normal when it stands between the camera and the thief.
    walls = []  # (entity, outward normal x, z)
    lamps = []  # (lamp, its light, its material slots)
    for i in range(int(ROOM_W / GRID)):
        x = -hw + GRID * (i + 0.5)
        walls.append((place("WallN%d" % i, "Wall", (x, 0, -hd)), (0, -1)))
        kind = "Doorway" if abs(x) < 0.01 else "Wall"
        walls.append((place("%sS%d" % (kind, i), kind, (x, 0, hd)), (0, 1)))
    for j in range(int(ROOM_D / GRID)):
        z = -hd + GRID * (j + 0.5)
        walls.append((place("WallW%d" % j, "Wall", (-hw, 0, z), yaw(90.0)), (-1, 0)))
        walls.append((place("WallE%d" % j, "Wall", (hw, 0, z), yaw(90.0)), (1, 0)))
    for sx in (-1, 1):
        for sz in (-1, 1):
            walls.append((place("Post%s%s" % ("W" if sx < 0 else "E", "N" if sz < 0 else "S"), "Post",
                                (sx * hw, 0, sz * hd)), (sx * 0.7071, sz * 0.7071)))
    # Colliders by side, the south one either side of the doorway and over it.
    solid("WallNorth", (0, WALL_H / 2, -hd), (ROOM_W + WALL_T, WALL_H, WALL_T))
    solid("WallWest", (-hw, WALL_H / 2, 0), (WALL_T, WALL_H, ROOM_D))
    solid("WallEast", (hw, WALL_H / 2, 0), (WALL_T, WALL_H, ROOM_D))
    side = (ROOM_W - DOOR_W) / 2
    solid("WallSouthWest", (-hw + side / 2, WALL_H / 2, hd), (side + WALL_T, WALL_H, WALL_T))
    solid("WallSouthEast", (hw - side / 2, WALL_H / 2, hd), (side + WALL_T, WALL_H, WALL_T))
    solid("Lintel", (0, (WALL_H + DOOR_H) / 2, hd), (DOOR_W, WALL_H - DOOR_H, WALL_T))

    # The door in the doorway, locked: its hinge at the frame's west edge, the leaf and its kinematic
    # collider under it so they swing with it (Door.as).
    door = d.stable_id("entity")
    d.entity("Door", (-LEAF_W / 2, 0, hd), eid=door)
    walls.append((place("DoorLeaf", "Door", (0, 0, 0), parent=door), (0, 1)))  # fades like the wall it is in
    leaf = d.stable_id("entity")
    d.entity("DoorCollider", (LEAF_W / 2, LEAF_H / 2, 0), parent=door, eid=leaf)
    d.add(leaf, "physics.RigidBody", motion=1, layer=0, shape=0,
          halfExtents={"x": LEAF_W / 2, "y": LEAF_H / 2, "z": 0.025})

    # The table and its oil lamp. The light hangs 0.75 m over the tabletop, above the 0.45 m chimney:
    # any nearer and the inverse square makes the tabletop tens of times brighter than the walls,
    # and the exposure that shows the walls clips it white.
    place("Table", "Table", TABLE)
    solid("Table", (TABLE[0], 0.4, TABLE[2]), (1.6, 0.8, 0.9))
    lamp = place("OilLamp", "OilLamp", (TABLE[0], 0.8, TABLE[2]))
    lamp_light = d.stable_id("entity")
    d.entity("LampLight", (0, 0.75, 0), parent=lamp, eid=lamp_light)
    d.add(lamp_light, "light", type=1, color={"r": 1.0, "g": 0.72, "b": 0.42, "a": 1.0}, intensity=6.0,
          range=7.0, castsShadows=True)
    lamp_slots = kit["OilLamp"][1]  # Brass, Flame, Chimney (blender/manor.py's order)
    lamps.append((lamp, lamp_light, lamp_slots))

    # The guard: round the room on a closed spline, his lantern a spot light ahead of him and down.
    route = d.stable_id("entity")
    d.entity("GuardRoute", eid=route)
    d.add(route, "spline", points=spline_points(GUARD_ROUTE), closed=True)
    guard = d.stable_id("entity")
    d.entity("Guard", GUARD_ROUTE[0], eid=guard)
    d.add(guard, "path_follow", spline=route, speed=GUARD_SPEED, loop=True, playing=True, alignToTangent=True)
    thing("GuardBody", cylinder, "Guard", (0, 0.9, 0), (0.55, 1.8, 0.55), parent=guard)
    lantern = d.stable_id("entity")
    d.entity("Lantern", (0.3, 1.1, -0.35), pitch(-25.0), parent=guard, eid=lantern)
    d.add(lantern, "light", type=2, color={"r": 1.0, "g": 0.8, "b": 0.55, "a": 1.0}, intensity=12.0,
          range=9.0, castsShadows=True)

    # The thief by the door: a character controller (Player), the model under it (blender/thief.py,
    # its origin at the feet, the capsule's centre 0.9 m up) driven by its graph (graph.py).
    thief = d.stable_id("entity")
    d.entity("Player", (1.2, 0.9, 3.0), eid=thief)
    d.add(thief, "physics.Character", radius=0.3, halfHeight=0.6, maxSlopeDegrees=45.0, stepUp=0.3)
    d.instance(THIEF_MODEL, (0, -0.9, 0), parent=thief, ops=thief_ops())
    # The right hand reaches for a lock (Door.as sets the target and the weight while picking).
    d.add(thief, "two_bone_ik", startBone="upperarm_R", midBone="forearm_R", endBone="hand_R", weight=0.0,
          fadeSeconds=0.25)
    cam = d.stable_id("entity")
    d.entity("Camera", (1.2, 7.9, 9.5), pitch(-42.0), eid=cam)
    d.add(cam, "camera", fovYRadians=0.9)
    d.script(thief, (asset("ScriptClassAsset", "Thief"), {"camera": ("entity", cam)}),
             (asset("ScriptClassAsset", "LightMeter"), {"hud": ("asset", asset("UIDocumentAsset", "Hud"))}))
    d.script(cam, (asset("ScriptClassAsset", "CameraRig"), {"target": ("entity", thief)}))
    # Each lamp the thief can put out and light again (Lamp.as), its flame and chimney swapped for
    # the dark materials while it is out.
    for lamp, lamp_light, slots in lamps:
        d.script(lamp, (asset("ScriptClassAsset", "Lamp"), {
            "thief": ("entity", thief), "light": ("entity", lamp_light),
            "flameLit": ("asset", slots[1]), "chimneyLit": ("asset", slots[2]),
            "flameOut": ("asset", mats["FlameOut"]), "chimneyOut": ("asset", mats["ChimneyOut"])}))
    d.script(door, (asset("ScriptClassAsset", "Door"), {"thief": ("entity", thief), "locked": True}))
    cutaway = asset("ScriptClassAsset", "CutawayWall")
    for e, (nx, nz) in walls:
        d.script(e, (cutaway, {"camera": ("entity", cam), "thief": ("entity", thief),
                               "normalX": float(nx), "normalZ": float(nz)}))

    scenes = [a for a in ASSETS if a["type"] == "SceneDocument" and a["name"] == "Room"]
    return d.write(scenes[0]["guid"] if scenes else None, group="Scenes")


if __name__ == "__main__":
    guid = build()
    mcp("project_settings_set", {"defaultSceneId": guid})  # P0 has the one scene; the game boots into it
    print("Room", guid)
