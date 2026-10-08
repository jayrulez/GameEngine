"""levels.py: Lamplight's levels as tables, which level.py writes as scenes.

Ground and walls in grid cells (a cell is 2 m; cell (i, j) spans x 2i..2i+2, z 2j..2j+2; a wall run
goes along grid lines from point to point); everything placed in the world in metres. North is -Z,
and the thief starts facing it with the camera south of him.

- ground: (kind, (i0, j0, i1, j1)) rectangles of tiles (Floor, Grass, Gravel, Flags), later ones
  over earlier ones.
- walls: (kind, (i0, j0), (i1, j1)) runs (Wall, Hedge, GardenWall), with a corner piece at each end;
  corners: (kind, (i, j)) more corner pieces (either side of a gate, where a run meets another
  mid-way).
- openings: {"at": (i, j) the segment's middle, "kind": Doorway / Gate / Gap, "locked": bool}.
- lamps: {"kind": LanternPost / OilLamp, "at": (x, z)}.
- guards: a round per guard, its points (x, z) walked in turn.
- loot: {"kind": Purse / Candlestick, "at": (x, z), "value"}; target: the one thing the level is
  for, carried to the exit (a trigger, (x0, z0, x1, z1)); checkpoints: trigger rectangles.
- start: the thief's (x, z, yaw degrees); bounds: the navigation zone's (x0, z0, x1, z1).
"""

ROOM = dict(
    name="Room",
    # The test room: 10 m by 8 m of boards inside a flagged yard, a doorway with a locked door in
    # the south wall, the oil lamp on its table, one guard, a key on the floor.
    ground=[("Flags", (0, 0, 7, 7)), ("Floor", (1, 1, 6, 5))],
    walls=[("Wall", (1, 1), (6, 1)), ("Wall", (1, 5), (6, 5)), ("Wall", (1, 1), (1, 5)), ("Wall", (6, 1), (6, 5))],
    openings=[dict(at=(3.5, 5), kind="Doorway", locked=True)],
    lamps=[dict(kind="OilLamp", at=(4.4, 4.4))],
    guards=[[(4.0, 7.0), (10.0, 7.0), (10.0, 3.5), (4.0, 3.5)]],
    loot=[dict(kind="Purse", at=(10.5, 8.5), value=10)],
    target=dict(kind="Key", at=(9.0, 3.0), value=100),
    exit=(5.5, 11.5, 8.5, 13.5),
    checkpoints=[],
    start=(8.2, 9.0, 0.0),
    bounds=(0.0, 0.0, 14.0, 14.0),
)

GARDENS = dict(
    name="Gardens",
    # Level 1: the manor's walled gardens by moonlight. Four squares of clipped yew, each open on
    # one side with something inside, around a crossing of gravel paths; lantern posts by the north
    # gate and at the crossing; one guard walking the paths. In over the south wall; the gardener's
    # key in the south-east square; out by the north gate, locked.
    ground=[("Gravel", (0, 0, 12, 1)), ("Grass", (0, 1, 12, 11)), ("Gravel", (6, 1, 7, 11)),
            ("Gravel", (1, 6, 11, 7))],
    walls=[("GardenWall", (0, 1), (12, 1)), ("GardenWall", (0, 11), (12, 11)),
           ("GardenWall", (0, 1), (0, 11)), ("GardenWall", (12, 1), (12, 11)),
           # The north-west square, open to the south.
           ("Hedge", (2, 3), (5, 3)), ("Hedge", (2, 3), (2, 5)), ("Hedge", (5, 3), (5, 5)), ("Hedge", (2, 5), (5, 5)),
           # The north-east square, open to the north.
           ("Hedge", (8, 3), (11, 3)), ("Hedge", (8, 3), (8, 5)), ("Hedge", (11, 3), (11, 5)), ("Hedge", (8, 5), (11, 5)),
           # The south-west square, open to the north.
           ("Hedge", (2, 8), (5, 8)), ("Hedge", (2, 8), (2, 10)), ("Hedge", (5, 8), (5, 10)), ("Hedge", (2, 10), (5, 10)),
           # The south-east square, open to the west.
           ("Hedge", (8, 8), (11, 8)), ("Hedge", (8, 8), (8, 10)), ("Hedge", (11, 8), (11, 10)),
           ("Hedge", (8, 10), (11, 10))],
    corners=[("GardenWall", (6, 1)), ("GardenWall", (7, 1))],  # the gate's piers
    openings=[dict(at=(6.5, 1), kind="Gate", locked=True),
              dict(at=(3.5, 5), kind="Gap"), dict(at=(9.5, 3), kind="Gap"), dict(at=(3.5, 8), kind="Gap"),
              dict(at=(8, 9.5), kind="Gap")],
    lamps=[dict(kind="LanternPost", at=(10.6, 3.4)), dict(kind="LanternPost", at=(15.4, 3.4)),
           dict(kind="LanternPost", at=(10.6, 11.4)), dict(kind="LanternPost", at=(15.4, 14.6))],
    guards=[[(3.0, 13.0), (13.0, 13.0), (13.0, 4.0), (13.0, 13.0), (23.0, 13.0), (13.0, 13.0)]],
    loot=[dict(kind="Purse", at=(7.0, 8.0), value=10), dict(kind="Purse", at=(19.0, 8.0), value=10),
          dict(kind="Purse", at=(7.0, 18.0), value=10), dict(kind="Candlestick", at=(2.4, 3.6), value=25)],
    target=dict(kind="Key", at=(19.0, 18.0), value=100),
    exit=(11.5, 0.0, 14.5, 1.5),
    checkpoints=[(12.0, 12.0, 14.0, 14.0)],
    start=(13.0, 20.5, 0.0),
    bounds=(0.0, 0.0, 24.0, 22.0),
    moon=dict(intensity=0.35, yaw=-60.0, pitch=-45.0),
)

LEVELS = {"Gardens": GARDENS, "Room": ROOM}
FIRST = "Gardens"  # the level the game boots into
