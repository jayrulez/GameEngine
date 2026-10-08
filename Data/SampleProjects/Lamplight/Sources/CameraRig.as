// CameraRig - the camera above and behind the thief, turned a quarter at a time (TurnLeft and
// TurnRight), swinging round to the new quarter rather than cutting to it, and following the thief
// with a little lag so a step does not jolt the view.
const float kPi = 3.14159265f;

float Saturate(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

class CameraRig
{
    private Entity@ self;

    [null, "The thief to follow"] Entity@ target;
    [6.5, "Distance back from the thief, along the floor (m)"] float distance;
    [7.0, "Height above the thief (m)"] float height;
    [0.8, "Aim this far above the thief's feet (m)"] float lookHeight;
    [6.0, "How quickly it catches up with the thief"] float follow;
    [7.0, "How quickly it swings to a new quarter"] float swing;

    private int m_quarter = 0;    // 0 = looking north (-Z), then quarters clockwise
    private float m_yaw = 0.0f;   // the swing's current angle (radians)
    private Float3 m_focus = Float3(0.0f, 0.0f, 0.0f);

    CameraRig(Entity@ entity) { @self = entity; }

    void onStart()
    {
        if (target !is null && target.isValid())
        {
            m_focus = target.worldPosition();
        }
        place();
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (Input::wasPressed("TurnLeft"))
        {
            m_quarter = (m_quarter + 3) % 4;
        }
        if (Input::wasPressed("TurnRight"))
        {
            m_quarter = (m_quarter + 1) % 4;
        }
        float want = float(m_quarter) * kPi * 0.5f;
        float delta = want - m_yaw;
        while (delta > kPi) delta -= 2.0f * kPi;
        while (delta < -kPi) delta += 2.0f * kPi;
        m_yaw += delta * Saturate(swing * d);
        if (target !is null && target.isValid())
        {
            Float3 at = target.worldPosition();
            float k = Saturate(follow * d);
            m_focus = Float3(m_focus.x + (at.x - m_focus.x) * k, m_focus.y + (at.y - m_focus.y) * k,
                             m_focus.z + (at.z - m_focus.z) * k);
        }
        place();
    }

    // Behind the focus for the current angle: yaw 0 sits south of it (+Z) looking north.
    private void place()
    {
        Float3 seat = Float3(m_focus.x - Math::Sin(m_yaw) * distance, m_focus.y + height,
                             m_focus.z + Math::Cos(m_yaw) * distance);
        self.setPosition(seat);
        Float3 dir = Float3(m_focus.x - seat.x, m_focus.y + lookHeight - seat.y, m_focus.z - seat.z);
        float len = Math::Sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 0.0001f)
        {
            return;
        }
        float yaw = Math::RadiansToDegrees(Math::Atan2(-dir.x, -dir.z));
        float pitch = Math::RadiansToDegrees(Math::Asin(dir.y / len));
        self.setRotationEuler(pitch, yaw, 0.0f);
    }
}
