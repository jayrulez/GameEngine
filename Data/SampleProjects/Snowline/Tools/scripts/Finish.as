// Finish - the finish line: when the rider crosses it down the course, "RunFinished". It holds
// the course's medal times too, and announces them at the start ("MedalGold", "MedalSilver",
// "MedalBronze", each in hundredths of a second) for Snowline.as to judge a run by. Placed by
// Tools/course.py at the course line's end, facing up the course, with its course's times.

class Finish
{
    private Entity@ self;

    [0.0, "The course's heading at the finish (radians; 0 runs toward +Z)"] float heading;
    [50.0, "A gold medal's time, penalties included (s)"] float gold;
    [56.0, "A silver medal's time (s)"] float silver;
    [64.0, "A bronze medal's time (s)"] float bronze;

    private Entity@ m_rider;
    private bool m_crossed = false;
    private bool m_seen = false;
    private float m_lastAlong = 0.0f;

    Finish(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
        self.scene.events.emit("MedalGold", int(gold * 100.0f + 0.5f));
        self.scene.events.emit("MedalSilver", int(silver * 100.0f + 0.5f));
        self.scene.events.emit("MedalBronze", int(bronze * 100.0f + 0.5f));
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

    // A new run: the line waits for the rider again.
    void onRunRestart(int unused)
    {
        m_crossed = false;
        m_seen = false;
    }
}
