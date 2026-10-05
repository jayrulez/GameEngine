// Gate - a slalom gate: two poles (each with its flag on a hinge) the rider passes between. When
// the rider crosses the gate's line down the course it reports "GatePassed" (between the poles) or
// "GateMissed" (round the outside: Snowline.as adds the penalty), with the gate's index. Placed by
// Tools/course.py, which gives each gate its index and the course's heading at it.

class Gate
{
    private Entity@ self;

    [0, "This gate's place down the course (0 = the first)"] int index;
    [4.0, "Half the gap between the poles (m)"] float halfWidth;
    [0.0, "The course's heading at the gate (radians; 0 runs toward +Z)"] float heading;

    private Entity@ m_rider;
    private bool m_crossed = false;
    private bool m_seen = false;
    private float m_lastAlong = 0.0f;

    Gate(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
        self.scene.events.emit("GateRegistered", index);
    }

    void onUpdate(double dt)
    {
        if (m_crossed || m_rider is null || !m_rider.isValid())
        {
            return;
        }
        // The rider in the gate's frame: along the course, and across it.
        Float3 d = m_rider.worldPosition() - self.worldPosition();
        float fx = Math::Sin(heading);
        float fz = Math::Cos(heading);
        float along = d.x * fx + d.z * fz;
        float across = d.x * fz - d.z * fx;
        if (m_seen && m_lastAlong < 0.0f && along >= 0.0f)
        {
            m_crossed = true;
            bool between = across >= -halfWidth && across <= halfWidth;
            self.scene.events.emit(between ? "GatePassed" : "GateMissed", index);
        }
        m_lastAlong = along;
        m_seen = true;
    }

    // A new run: the gate waits for the rider again.
    void onRunRestart(int unused)
    {
        m_crossed = false;
        m_seen = false;
    }
}
