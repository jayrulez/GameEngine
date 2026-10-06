// FollowCamera - the chase camera: it springs toward a seat behind and above the target and aims at
// it. "Behind" is the camera's own lag: as the rider goes on, the camera falls back, so the ground
// direction from the rider back to the camera already points the right way.
class FollowCamera
{
    private Entity@ self;

    [null, "The entity to follow (the rider)"] Entity@ target;
    [8.0, "Distance behind the target (m)"] float distance;
    [3.4, "Height above the target (m)"] float height;
    [1.2, "Aim this far above the target (m)"] float lookHeight;
    [4.5, "Position spring rate (higher = snappier)"] float positionSmoothing;
    [0.35, "How long a crash shakes the camera (s)"] float shakeTime;
    [0.35, "How far a crash shakes it at first (m)"] float shakeAmount;
    [40.0, "A target this far from the seat has jumped: the camera snaps to it (m)"] float snapDistance;

    // The follow works on an unshaken position; a crash's shake is added only to where the camera
    // is drawn, dying away, so it never pulls the follow off course.
    private Float3 m_base = Float3(0.0f, 0.0f, 0.0f);
    private float m_shake = 0.0f;
    private bool m_reseat = false; // a new run: seat behind the rider again on the next update

    FollowCamera(Entity@ entity) { @self = entity; }

    void onStart()
    {
        seatBehind();
    }

    // A new run puts the rider back at the top: the camera goes straight behind it again. Its lag
    // would otherwise seat it on the side it was last on (downhill, at the finish), facing the
    // rider. On the next update, once the board has turned the rider back down the course.
    void onRunRestart(int unused)
    {
        m_reseat = true;
    }

    // Straight behind the rider's facing, not wherever the scene or the last run left the camera.
    private void seatBehind()
    {
        if (target is null || !target.isValid())
        {
            return;
        }
        Float3 at = target.position();
        Float3 back = Quaternion::RotateVector(target.rotation(), Float3(0.0f, 0.0f, -1.0f));
        Float3 seat = Float3(at.x + back.x * distance, at.y + height, at.z + back.z * distance);
        self.setPosition(seat);
        m_base = seat;
        faceToward(seat, Float3(at.x, at.y + lookHeight, at.z));
    }

    void onRiderCrashed(int count)
    {
        m_shake = shakeTime;
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (d <= 0.0f || target is null || !target.isValid())
        {
            return;
        }
        if (m_reseat)
        {
            m_reseat = false;
            seatBehind();
            return;
        }
        // The target's LOCAL position (the rider is a root): physics has already written this
        // frame's interpolated pose there, while worldPosition() is last frame's until the scene
        // updates its transforms after every script, and following it makes the rider shake
        // against the camera at speed.
        Float3 at = target.position();
        Float3 cam = m_base;
        float backX = cam.x - at.x;
        float backZ = cam.z - at.z;
        float flat = Math::Sqrt(backX * backX + backZ * backZ);
        if (flat < 0.001f)
        {
            backX = 0.0f;
            backZ = 1.0f;
            flat = 1.0f;
        }
        Float3 seat = Float3(at.x + backX / flat * distance, at.y + height, at.z + backZ / flat * distance);
        // A jump of the target (a respawn back at the top) snaps the seat rather than flying the
        // camera the length of the course behind it.
        float jx = seat.x - cam.x;
        float jy = seat.y - cam.y;
        float jz = seat.z - cam.z;
        if (jx * jx + jy * jy + jz * jz > snapDistance * snapDistance)
        {
            cam = seat;
        }
        Float3 next = Float3::Lerp(cam, seat, clamp01(positionSmoothing * d));
        m_base = next;
        if (m_shake > 0.0f)
        {
            m_shake -= d;
            float k = shakeAmount * clamp01(m_shake / shakeTime);
            next = next + Float3(Random::range(-k, k), Random::range(-k, k), Random::range(-k, k));
        }
        self.setPosition(next);
        faceToward(next, Float3(at.x, at.y + lookHeight, at.z));
    }

    // Engine forward is -Z: yaw = atan2(-dir.x, -dir.z); a positive pitch looks up.
    private void faceToward(Float3 from, Float3 to)
    {
        Float3 dir = to - from;
        float len = Math::Sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 0.0001f)
        {
            return;
        }
        float yaw = Math::RadiansToDegrees(Math::Atan2(-dir.x, -dir.z));
        float pitch = Math::RadiansToDegrees(Math::Asin(dir.y / len));
        self.setRotationEuler(pitch, yaw, 0.0f);
    }

    private float clamp01(float v)
    {
        if (v < 0.0f) { return 0.0f; }
        if (v > 1.0f) { return 1.0f; }
        return v;
    }
}
