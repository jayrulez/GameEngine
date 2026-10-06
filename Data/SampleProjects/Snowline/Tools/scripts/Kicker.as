// Kicker - launches the rider off the lip. On the kicker's entity, whose origin is where its ramp
// comes out of the snow and whose +Z runs up the ramp (Tools/course.py, blender/props.py).
//
// The ramp is solid to the rider, but a character's contacts over a curve at speed are uneven: one
// run left the lip at 16 m/s, the next hopped on the curve, lost half its speed and barely left the
// snow. So the kicker sets the launch itself: it notes the rider's speed as it reaches the foot,
// and when the rider crosses the lip within the kicker's width it announces the launch to the board
// ("KickerLaunch", the speed in cm/s, after "KickerAngle", the lip's angle above level in tenths of
// a degree): the entry speed less what the climb takes (v^2 = v0^2 - 2 g h), off the lip's angle.

class Kicker
{
    private Entity@ self;

    [3.88, "From the origin up the ramp to the lip (m)"] float run;
    [1.4, "The lip's height above the origin (m)"] float lipHeight;
    [29.0, "The ramp's angle at the lip, against its own base (degrees)"] float lipAngle;
    [5.0, "The kicker's width (m)"] float width;

    private Entity@ m_rider;
    private float m_lastAlong = -1000.0f;
    private float m_entrySpeed = -1.0f; // the rider's speed at the foot this pass; -1 for none

    Kicker(Entity@ entity) { @self = entity; }

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
        // The rider in the kicker's frame: along its ramp (level), across it.
        Float3 forward = Quaternion::RotateVector(self.rotation(), Float3(0.0f, 0.0f, 1.0f));
        float flat = Math::Sqrt(forward.x * forward.x + forward.z * forward.z);
        float fx = forward.x / flat;
        float fz = forward.z / flat;
        Float3 d = m_rider.worldPosition() - self.worldPosition();
        float along = d.x * fx + d.z * fz;
        float across = d.x * fz - d.z * fx;
        bool within = Math::Abs(across) <= width * 0.5f;
        if (within && m_lastAlong < 0.0f && along >= 0.0f)
        {
            Float3 v = CharacterComponent::of(m_rider).velocity;
            m_entrySpeed = Math::Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }
        float lip = run * flat; // the lip's distance along, level
        if (within && m_entrySpeed > 0.0f && m_lastAlong < lip && along >= lip)
        {
            float squared = m_entrySpeed * m_entrySpeed - 2.0f * 9.81f * lipHeight;
            float speed = squared > 1.0f ? Math::Sqrt(squared) : 1.0f;
            // The lip's angle above level: its angle on the ramp less how far the kicker tips down
            // the slope it stands on.
            float tilt = Math::RadiansToDegrees(Math::Atan2(-forward.y, flat));
            self.scene.events.emit("KickerAngle", int((lipAngle - tilt) * 10.0f + 0.5f));
            self.scene.events.emit("KickerLaunch", int(speed * 100.0f + 0.5f));
            m_entrySpeed = -1.0f;
        }
        if (along > lip + 2.0f)
        {
            m_entrySpeed = -1.0f; // past the kicker (round its side): this pass is over
        }
        m_lastAlong = along;
    }

    // A new run: the kicker waits for the rider again.
    void onRunRestart(int unused)
    {
        m_lastAlong = -1000.0f;
        m_entrySpeed = -1.0f;
    }
}
