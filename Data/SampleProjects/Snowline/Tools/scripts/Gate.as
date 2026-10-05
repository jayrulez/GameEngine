// Gate - a slalom gate: two poles (each with its flag on a hinge) the rider passes between. When
// the rider crosses the gate's line down the course it reports "GatePassed" (between the poles) or
// "GateMissed" (round the outside: Snowline.as adds the penalty), with the gate's index. Placed by
// Tools/course.py, which gives each gate its index and the course's heading at it.
//
// The flags do not collide with the rider (a flag at its hinge's limit would stop it dead; see
// Tools/gates.py), so the gate swings one the rider brushes past: a push along the rider's motion,
// once each time the rider comes within reach of it.

class Gate
{
    private Entity@ self;

    [0, "This gate's place down the course (0 = the first)"] int index;
    [4.0, "Half the gap between the poles (m)"] float halfWidth;
    [0.0, "The course's heading at the gate (radians; 0 runs toward +Z)"] float heading;
    [0.9, "How far down the course a flag reaches the rider (m)"] float brushAlong;
    [0.8, "How far out from its pole a flag reaches (m)"] float brushOut;
    [0.4, "How far in from its pole a brush still swings the flag (m)"] float brushIn;
    [0.05, "The push for each m/s of the rider's speed (N s)"] float brushPush;

    private Entity@ m_rider;
    private bool m_crossed = false;
    private bool m_seen = false;
    private float m_lastAlong = 0.0f;
    private Entity@ m_flagLeft;
    private Entity@ m_flagRight;
    private bool m_brushingLeft = false;
    private bool m_brushingRight = false;

    Gate(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
        self.scene.events.emit("GateRegistered", index);
        Entity@ gate = self.firstChild(); // the GateRed or GateBlue prefab's root
        if (gate !is null && gate.isValid())
        {
            @m_flagLeft = gate.findChildByName("FlagL");
            @m_flagRight = gate.findChildByName("FlagR");
        }
    }

    void onUpdate(double dt)
    {
        if (m_rider is null || !m_rider.isValid())
        {
            return;
        }
        // The rider in the gate's frame: along the course, and across it (+ toward the right pole).
        Float3 d = m_rider.worldPosition() - self.worldPosition();
        float fx = Math::Sin(heading);
        float fz = Math::Cos(heading);
        float along = d.x * fx + d.z * fz;
        float across = d.x * fz - d.z * fx;
        m_brushingLeft = brush(m_flagLeft, m_brushingLeft, along, -across);
        m_brushingRight = brush(m_flagRight, m_brushingRight, along, across);
        if (m_crossed)
        {
            return;
        }
        if (m_seen && m_lastAlong < 0.0f && along >= 0.0f)
        {
            m_crossed = true;
            bool between = across >= -halfWidth && across <= halfWidth;
            self.scene.events.emit(between ? "GatePassed" : "GateMissed", index);
        }
        m_lastAlong = along;
        m_seen = true;
    }

    // Swings `flag` as the rider comes within its reach (`outward`: the rider's distance across
    // the gate toward the flag's side): once per pass, along the rider's motion. Whether in reach.
    private bool brush(Entity@ flag, bool wasInReach, float along, float outward)
    {
        float fromPole = outward - halfWidth;
        bool inReach = along > -brushAlong && along < brushAlong && fromPole > -brushIn && fromPole < brushOut;
        if (inReach && !wasInReach && flag !is null && flag.isValid())
        {
            Float3 v = CharacterComponent::of(m_rider).velocity;
            ScenePhysics::of(self.scene).applyImpulse(flag, Float3(v.x * brushPush, 0.0f, v.z * brushPush));
        }
        return inReach;
    }

    // A new run: the gate waits for the rider again.
    void onRunRestart(int unused)
    {
        m_crossed = false;
        m_seen = false;
    }
}
