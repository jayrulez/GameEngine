// Gap: judges a jump over a crevasse. The script sits on an entity at the crevasse's lip, facing
// down the course (placed by Tools/course.py); a kicker launches the rider off the edge (Kicker.as).
//
// Once the rider passes the lip within `reach` of it, their first touch of the snow is judged.
// Landing less than `short` metres past the lip (in the crevasse, before its ramp up) is
// "GapShort", which the board takes as a crash. Landing at or beyond it is "GapCleared", with the
// distance flown in centimetres, which the game scores. A rider who rolls in without leaving the
// snow is also short, once past the crevasse's wall. A rider flying over is judged where they
// touch down, so the distance is the one actually flown.

class Gap
{
    private Entity@ self;

    [0.0, "The course's heading at the lip (radians; 0 runs toward +Z)"] float heading;
    [13.5, "Past the lip, how far a landing is still in the crevasse (m)"] float short;
    [30.0, "How far either side of the course the crevasse spans (m)"] float reach;

    private Entity@ m_rider;
    private float m_lastAlong = -1000.0f;
    private bool m_armed = false; // past the lip on this pass and not judged yet
    private bool m_flew = false;  // off the snow since the lip

    Gap(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
    }

    void onUpdate(double dt)
    {
        if (m_rider is null || !m_rider.isValid())
        {
            return;
        }
        // The rider is a root entity, so position() holds this frame's pose; worldPosition() still
        // holds last frame's.
        Float3 d = m_rider.position() - self.worldPosition();
        float along = d.x * Math::Sin(heading) + d.z * Math::Cos(heading);
        float across = d.x * Math::Cos(heading) - d.z * Math::Sin(heading);
        if (!m_armed && m_lastAlong < 0.0f && along >= 0.0f && Math::Abs(across) <= reach)
        {
            m_armed = true;
            m_flew = false;
        }
        m_lastAlong = along;
        if (!m_armed)
        {
            return;
        }
        bool grounded = CharacterComponent::of(m_rider).grounded();
        if (!grounded)
        {
            m_flew = true;
        }
        if (grounded && (m_flew || along > 2.0f))
        {
            judge(along); // if the rider flew over, they are past the crevasse by now
        }
    }

    private void judge(float along)
    {
        m_armed = false;
        if (along < short)
        {
            self.scene.events.emit("GapShort", int(along * 100.0f + 0.5f));
        }
        else
        {
            self.scene.events.emit("GapCleared", int(along * 100.0f + 0.5f));
        }
    }

    // A new run: the gap waits for the rider again.
    void onRunRestart(int unused)
    {
        m_lastAlong = -1000.0f;
        m_armed = false;
        m_flew = false;
    }
}
