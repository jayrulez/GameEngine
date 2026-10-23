// MapMarkers - on the block's top-down camera, which draws the Minimap texture the HUD shows. Places
// the HUD's markers over that image from world positions: a dot per subscriber (dimmed once it has
// its paper) and the bike, turned to its heading. The camera is orthographic and looks straight
// down, so world to map is a scale and an offset: map right is +X, map up is -Z.
// The HUD has this many subscriber dots (map-sub-0 ...).
const uint kDots = 8;

class MapMarkers
{
    private Entity@ self;

    [null, "The bike"] Entity@ bike;
    [84.0, "World metres the map spans (the camera's orthoHeight)"] float span;
    [200.0, "The map view's size in pixels"] float mapSize;
    [2, "The delivery zones' collision group"] int zoneGroup;

    private array<Entity@> m_zones;
    private bool m_found = false;

    MapMarkers(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        // The zones are static triggers, there once physics has built them, so look once on the
        // first update rather than in onStart.
        if (!m_found)
        {
            findZones();
        }
        for (uint i = 0; i < m_zones.length(); i++)
        {
            UIView@ dot = ui::find("map-sub-" + i);
            if (dot is null || !dot.isValid())
            {
                continue;
            }
            Entity@ marker = m_zones[i].findChildByName("Marker");
            bool waiting = marker !is null && marker.isValid() && marker.active();
            dot.setOpacity(waiting ? 1.0f : 0.3f);
        }
        if (bike is null || !bike.isValid())
        {
            return;
        }
        UIView@ arrow = ui::find("map-bike");
        if (arrow is null || !arrow.isValid())
        {
            return;
        }
        place(arrow, bike.position(), 10.0f, 16.0f);
        // The bike faces its local +Z; on the map that points (x, z) with z down the screen, and a
        // view turns clockwise from up.
        Float3 f = Quaternion::RotateVector(bike.rotation(), Float3(0.0f, 0.0f, 1.0f));
        arrow.setRotation(Math::RadiansToDegrees(Math::Atan2(f.x, -f.z)));
        arrow.setVisible(true);
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
            UIView@ dot = ui::find("map-sub-" + i);
            if (dot !is null && dot.isValid())
            {
                place(dot, zones[i].worldPosition(), 12.0f, 12.0f);
                dot.setVisible(true);
            }
        }
    }

    // Centre a marker of the given pixel size on a world position.
    private void place(UIView@ marker, Float3 world, float width, float height)
    {
        Float3 c = self.position();
        float u = ((world.x - c.x) / span + 0.5f) * mapSize;
        float v = ((world.z - c.z) / span + 0.5f) * mapSize;
        marker.setTranslation(u - width * 0.5f, v - height * 0.5f);
    }
}
