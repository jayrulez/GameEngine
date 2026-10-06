// Gate - a slalom gate: two poles (each with its flag on a hinge) the rider passes between. When
// the rider crosses the gate's line down the course it reports "GatePassed" (between the poles) or
// "GateMissed" (round the outside: Snowline.as adds the penalty), with the gate's index. Placed by
// Tools/course.py, which gives each gate its index and the course's heading at it.
//
// The flags do not collide with the rider (a flag at its hinge's limit would stop it dead; see
// Tools/gates.py), so the gate swings one the rider brushes past: a push along the rider's motion,
// once each time the rider comes within reach of it. Each flag springs back to hang straight (its
// hinge has no spring, and on an upright hinge gravity does not swing it back, so a flag once
// brushed stayed turned, into the next run): the gate pushes it toward its rest angle, damped.

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
    [0.9, "The flags' spring back to rest (N m per radian)"] float flagSpring;
    [0.15, "Its damping (N m s per radian)"] float flagDamping;
    [0.3, "From a flag's hinge to its centre (m; Tools/gates.py FLAG_REACH)"] float flagReach;

    private Entity@ m_rider;
    private bool m_crossed = false;
    private bool m_seen = false;
    private float m_lastAlong = 0.0f;
    private Entity@ m_flagLeft;
    private Entity@ m_flagRight;
    private bool m_brushingLeft = false;
    private bool m_brushingRight = false;
    // Each flag at rest, in its gate's frame: the way its panel points out, and its angle last frame.
    private Float3 m_restLeft = Float3(0.0f, 0.0f, 0.0f);
    private Float3 m_restRight = Float3(0.0f, 0.0f, 0.0f);
    private float m_angleLeft = 0.0f;
    private float m_angleRight = 0.0f;

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
            m_restLeft = outward(m_flagLeft);
            m_restRight = outward(m_flagRight);
        }
    }

    void onUpdate(double dt)
    {
        if (m_rider is null || !m_rider.isValid())
        {
            return;
        }
        // The rider in the gate's frame: along the course, and across it (+ toward the right pole).
        // The rider is top-level: position() is this frame's pose (worldPosition() was last frame's).
        Float3 d = m_rider.position() - self.worldPosition();
        float fx = Math::Sin(heading);
        float fz = Math::Cos(heading);
        float along = d.x * fx + d.z * fz;
        float across = d.x * fz - d.z * fx;
        m_brushingLeft = brush(m_flagLeft, m_brushingLeft, along, -across);
        m_brushingRight = brush(m_flagRight, m_brushingRight, along, across);
        float step = float(dt);
        if (step > 0.0f)
        {
            m_angleLeft = springBack(m_flagLeft, m_restLeft, m_angleLeft, step);
            m_angleRight = springBack(m_flagRight, m_restRight, m_angleRight, step);
        }
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

    // The way a flag's panel points out from its hinge, flat, in its gate's frame (its local +X).
    private Float3 outward(Entity@ flag)
    {
        if (flag is null || !flag.isValid())
        {
            return Float3(1.0f, 0.0f, 0.0f);
        }
        Float3 o = Quaternion::RotateVector(flag.rotation(), Float3(1.0f, 0.0f, 0.0f));
        float flat = Math::Sqrt(o.x * o.x + o.z * o.z);
        return (flat > 0.001f) ? Float3(o.x / flat, 0.0f, o.z / flat) : Float3(1.0f, 0.0f, 0.0f);
    }

    // Pushes a flag back toward its rest angle about its upright hinge, as a damped spring: a push at
    // its centre along its swing, of (spring x angle + damping x its turn rate) over its reach.
    // Returns the angle now (for the next frame's turn rate).
    private float springBack(Entity@ flag, Float3 rest, float lastAngle, float dt)
    {
        if (flag is null || !flag.isValid())
        {
            return 0.0f;
        }
        Float3 now = outward(flag);
        // The angle from rest about up (+ turns +X toward -Z, as a turn about +Y does).
        float angle = Math::Atan2(rest.z * now.x - rest.x * now.z, rest.x * now.x + rest.z * now.z);
        float rate = (angle - lastAngle) / dt;
        float torque = -(flagSpring * angle + flagDamping * rate);
        // Along the swing: up x outward, the way the angle grows; into the world by the gate's turn.
        Float3 swing = Quaternion::RotateVector(self.rotation(), Float3(-now.z, 0.0f, now.x) * -1.0f);
        float push = torque * dt / flagReach;
        ScenePhysics::of(self.scene).applyImpulse(flag, Float3(swing.x * push, 0.0f, swing.z * push));
        return angle;
    }

    // A new run: the gate waits for the rider again.
    void onRunRestart(int unused)
    {
        m_crossed = false;
        m_seen = false;
    }
}
