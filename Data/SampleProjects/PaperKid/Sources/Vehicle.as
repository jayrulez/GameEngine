// Vehicle: drives laps of the ring road on the navmesh, corner to corner, in its lane. The lane is
// the ring it is placed on (its distance from the middle along the nearer axis), so one prefab
// drives any block's ring. Inner lanes lap clockwise and outer ones anticlockwise (`direction`).
// The Level sets the speed ("TrafficSpeed").
class Vehicle
{
    private Entity@ self;

    [1, "1 laps clockwise (seen from above), -1 anticlockwise"] int direction;
    [6.0, "Driving speed until the Level sets one (m/s)"] float speed;

    private int m_corner = 0;
    private float lane = 22.0f;

    Vehicle(Entity@ entity) { @self = entity; }

    void onStart()
    {
        NavAgentComponent::of(self).setSpeed(speed);
        // Head for the corner ahead of where the car stands.
        Float3 at = self.worldPosition();
        lane = Math::Abs(at.x) > Math::Abs(at.z) ? Math::Abs(at.x) : Math::Abs(at.z);
        m_corner = nearestCorner(at);
        advance();
    }

    void onTrafficSpeed(float s)
    {
        speed = s;
        NavAgentComponent::of(self).setSpeed(speed);
    }

    void onUpdate(double dt)
    {
        if (NavAgentComponent::of(self).finished())
        {
            advance();
        }
        // Face the way it drives.
        NavAgentComponent@ agent = NavAgentComponent::of(self);
        float vx = agent.velocityX();
        float vz = agent.velocityZ();
        if (vx * vx + vz * vz > 0.25f)
        {
            self.setRotationEuler(0.0f, Math::RadiansToDegrees(Math::Atan2(vx, vz)), 0.0f);
        }
    }

    private void advance()
    {
        m_corner = (m_corner + direction + 4) % 4;
        Float3 c = corner(m_corner);
        NavAgentComponent::of(self).navigate(c.x, 0.0f, c.z);
    }

    // The ring's corners, clockwise seen from above (+Y): north-west, north-east, south-east,
    // south-west, on this car's lane.
    private Float3 corner(int index)
    {
        if (index == 0) { return Float3(-lane, 0.0f, -lane); }
        if (index == 1) { return Float3(lane, 0.0f, -lane); }
        if (index == 2) { return Float3(lane, 0.0f, lane); }
        return Float3(-lane, 0.0f, lane);
    }

    private int nearestCorner(Float3 at)
    {
        int best = 0;
        float bestDist = 1.0e9f;
        for (int i = 0; i < 4; i++)
        {
            Float3 c = corner(i);
            float d = (c.x - at.x) * (c.x - at.x) + (c.z - at.z) * (c.z - at.z);
            if (d < bestDist)
            {
                bestDist = d;
                best = i;
            }
        }
        return best;
    }
}
