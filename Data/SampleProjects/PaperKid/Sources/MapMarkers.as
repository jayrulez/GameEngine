// MapMarkers: sits on the block's top-down camera, which draws the Minimap texture the HUD shows,
// and places the HUD's markers over that image from world positions: a dot per subscriber (dimmed
// once it has its paper) and the bike, turned to its heading. The camera is orthographic and looks
// straight down, so world to map is a scale and an offset: map right is +X, map up is -Z.
// The HUD has this many subscriber dots (map-sub-0 ...).
const uint kDots = 8;

// Finds a HUD element, searching the top screen first. A HUD just replaced stays in the tree while
// it leaves, with the same ids, and would otherwise be found first.
Node@ hud(string id)
{
    ScreenStack@ stack = ScreenStack::of(gui::root().find("screens"));
    Screen@ top = stack !is null ? stack.top() : null;
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}

class MapMarkers
{
    private Entity@ self;

    [null, "The bike"] Entity@ bike;
    [84.0, "World metres the map spans (the camera's orthoHeight)"] float span;
    [200.0, "The map view's size in pixels"] float mapSize;
    [2, "The delivery zones' collision group"] int zoneGroup;

    private array<Entity@> m_zones;
    private array<int> m_waiting; // the state each dot shows: 1 waiting, 0 delivered, -1 not yet shown
    private bool m_found = false;

    MapMarkers(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        // The zones are static triggers that exist only once physics has built them, so look them
        // up once on the first update rather than in onStart.
        if (!m_found)
        {
            findZones();
        }
        for (uint i = 0; i < m_zones.length(); i++)
        {
            Node@ dot = hud("map-sub-" + i);
            if (dot is null)
            {
                continue;
            }
            Entity@ marker = m_zones[i].findChildByName("Marker");
            int waiting = marker !is null && marker.isValid() && marker.active() ? 1 : 0;
            if (waiting != m_waiting[i]) // the style sheet's opacity 0 until placed, then full or dimmed
            {
                m_waiting[i] = waiting;
                dot.tween("opacity", waiting == 1 ? "1" : "0.3", 0.0f, "linear");
            }
        }
        if (bike is null || !bike.isValid())
        {
            return;
        }
        Node@ arrow = hud("map-bike");
        if (arrow is null)
        {
            return;
        }
        // The bike faces its local +Z. On the map that direction is (x, z) with z down the screen,
        // and a view's rotation runs clockwise from up. It is set every frame; a tween of zero
        // length replaces the previous one.
        Float3 f = Quaternion::RotateVector(bike.rotation(), Float3(0.0f, 0.0f, 1.0f));
        arrow.tween("transform", at(bike.position(), 10.0f, 16.0f) + " rotate(" + Math::RadiansToDegrees(Math::Atan2(f.x, -f.z)) + "deg)",
                    0.0f, "linear");
        arrow.tween("opacity", "1", 0.0f, "linear");
    }

    private void findZones()
    {
        Float3 c = self.position();
        array<Entity@>@ zones = ScenePhysics::of(self.scene).overlapSphere(c.x, 0.0f, c.z, span,
                                                                          1 << zoneGroup);
        if (zones.length() == 0)
        {
            return;
        }
        m_found = true;
        for (uint i = 0; i < zones.length() && i < kDots; i++)
        {
            m_zones.insertLast(zones[i]);
            m_waiting.insertLast(-1);
            Node@ dot = hud("map-sub-" + i);
            if (dot !is null)
            {
                dot.tween("transform", at(zones[i].worldPosition(), 12.0f, 12.0f), 0.0f, "linear");
            }
        }
    }

    // The CSS transform, from the map's corner, that centres a marker of the given pixel size on a
    // world position.
    private string at(Float3 world, float width, float height)
    {
        Float3 c = self.position();
        float u = ((world.x - c.x) / span + 0.5f) * mapSize;
        float v = ((world.z - c.z) / span + 0.5f) * mapSize;
        return "translate(" + (u - width * 0.5f) + "px, " + (v - height * 0.5f) + "px)";
    }
}
