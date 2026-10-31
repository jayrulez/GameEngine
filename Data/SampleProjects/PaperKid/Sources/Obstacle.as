// Obstacle: something the bike crashes into: a car, a pedestrian, the street junk. When the bike
// comes within reach it is told it crashed ("Crashed"), and the obstacle waits a moment before it
// can be hit again, so one bump counts as one crash.
class Obstacle
{
    private Entity@ self;

    [1.0, "Reach from this entity's origin to the bike's centre (m)"] float radius;
    [2.0, "Seconds before this obstacle can be hit again"] float cooldown;

    private Entity@ m_bike;
    private float m_wait = 0.0f;

    Obstacle(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_bike = self.scene.find("Bike");
    }

    void onUpdate(double dt)
    {
        if (m_wait > 0.0f)
        {
            m_wait -= float(dt);
            return;
        }
        if (m_bike is null || !m_bike.isValid())
        {
            return;
        }
        Float3 at = self.worldPosition();
        Float3 bike = m_bike.worldPosition();
        float dx = bike.x - at.x;
        float dz = bike.z - at.z;
        if (dx * dx + dz * dz < radius * radius)
        {
            m_wait = cooldown;
            m_bike.send("Crashed", 0);
        }
    }
}
