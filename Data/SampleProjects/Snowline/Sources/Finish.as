// Finish - the finish line: when the rider crosses it down the course, "RunFinished". Placed by
// Tools/course.py at the course line's end, facing up the course.

class Finish
{
    private Entity@ self;

    [0.0, "The course's heading at the finish (radians; 0 runs toward +Z)"] float heading;

    private Entity@ m_rider;
    private bool m_crossed = false;
    private bool m_seen = false;
    private float m_lastAlong = 0.0f;

    Finish(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
    }

    void onUpdate(double dt)
    {
        if (m_crossed || m_rider is null || !m_rider.isValid())
        {
            return;
        }
        Float3 d = m_rider.worldPosition() - self.worldPosition();
        float along = d.x * Math::Sin(heading) + d.z * Math::Cos(heading);
        if (m_seen && m_lastAlong < 0.0f && along >= 0.0f)
        {
            m_crossed = true;
            self.scene.events.emit("RunFinished", 0);
        }
        m_lastAlong = along;
        m_seen = true;
    }

    void onRunRestart(int unused)
    {
        m_crossed = false;
        m_seen = false;
    }
}
