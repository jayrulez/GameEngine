// FollowCamera: a platformer camera that sits behind and above the player at a fixed offset,
// springing after it and aiming just above it. The offset never turns, so the controls stay world
// relative: forward is always away from the camera.
class FollowCamera
{
    private Entity@ self;

    [null, "The entity to follow (the player)"] Entity@ target;
    [11.0, "Distance behind the target (m)"] float distance;
    [6.0, "Height above the target (m)"] float height;
    [1.0, "Aim this far above the target (m)"] float lookHeight;
    [5.0, "Position spring rate (higher = snappier)"] float positionSmoothing;
    [6.0, "How fast a shake dies away (per second)"] float shakeDecay;

    /// The current shake strength (m), set by "Shake" and decaying to nothing.
    private float m_shake = 0.0f;
    /// The camera position the follow computes, before any shake.
    private Float3 m_followPos = Float3(0.0f, 0.0f, 0.0f);

    FollowCamera(Entity@ entity) { @self = entity; }

    // Something jolted the view, such as a hit or a landing from high up. Strength in metres.
    void onShake(float strength)
    {
        if (strength > m_shake)
        {
            m_shake = strength;
        }
    }

    void onStart()
    {
        if (target !is null && target.isValid())
        {
            Float3 at = target.worldPosition();
            Float3 start = Float3(at.x, at.y + height, at.z + distance);
            self.setPosition(start);
            m_followPos = start;
            faceToward(start, Float3(at.x, at.y + lookHeight, at.z));
        }
    }

    void onUpdate(double delta)
    {
        float dt = float(delta);
        if (dt <= 0.0f || target is null || !target.isValid())
        {
            return;
        }
        Float3 at = target.worldPosition();
        Float3 desired = Float3(at.x, at.y + height, at.z + distance);
        Float3 newPos = Float3::Lerp(m_followPos, desired, clamp01(positionSmoothing * dt));
        m_followPos = newPos;
        self.setPosition(newPos);
        faceToward(newPos, Float3(at.x, at.y + lookHeight, at.z));
        // The shake rides on top of the follow, which never sees it.
        if (m_shake > 0.001f)
        {
            self.setPosition(newPos + Float3(Random::range(-m_shake, m_shake),
                                             Random::range(-m_shake, m_shake), 0.0f));
            m_shake -= m_shake * clamp01(shakeDecay * dt);
        }
    }

    // Tilt the camera's forward (-Z) down toward `to`. The camera never turns (it always looks
    // down -Z), so the aim is a pitch alone: a rotation about +X, negative to look down.
    private void faceToward(Float3 from, Float3 to)
    {
        float dy = to.y - from.y;
        float dz = from.z - to.z;
        if (Math::Abs(dy) + Math::Abs(dz) < 0.0001f)
        {
            return;
        }
        self.setRotation(Quaternion::FromAxisAngle(Float3(1.0f, 0.0f, 0.0f), Math::Atan2(dy, dz)));
    }

    private float clamp01(float v)
    {
        if (v < 0.0f) { return 0.0f; }
        if (v > 1.0f) { return 1.0f; }
        return v;
    }
}
