// CameraRig: the camera above and behind the player. TurnLeft and TurnRight turn it a quarter at a
// time, and it swings round to the new quarter rather than cutting to it. It follows the player
// with a little lag so that a single step does not jolt the view. Behind the title screen it
// instead sweeps slowly to and fro in front of the ball's dais. Each map's dais stands against a
// wall, so a full circle would leave the hall. The scene is held at time scale 0 on the title, so
// the sweep runs on the time the game script passes in.
const float kPi = 3.14159265f;

class CameraRig
{
    private Entity@ self;

    [null, "The player to follow"] Entity@ target;
    [6.5, "Distance back from the player, along the floor (m)"] float distance;
    [7.0, "Height above the player (m)"] float height;
    [0.8, "Aim this far above the player's feet (m)"] float lookHeight;
    [6.0, "How quickly it catches up with the player"] float follow;
    [7.0, "How quickly it swings to a new quarter"] float swing;
    [12.0, "Behind the title: distance from the dais (m)"] float titleDistance;
    [7.5, "Behind the title: height above the floor (m)"] float titleHeight;
    [4.0, "Behind the title: aim this far above the dais, so it sits below the menu (m)"] float titleLook;
    [0.7, "Behind the title: how far it sweeps each side of the dais's front (radians)"] float titleSweep;
    [0.12, "Behind the title: how fast it sweeps (radians of its cycle a second)"] float titleTurn;

    private int m_quarter = 0;    // 0 = looking north (-Z), then quarters clockwise
    private float m_yaw = 0.0f;   // current angle of the swing (radians)
    private Float3 m_focus = Float3(0.0f, 0.0f, 0.0f);
    private bool m_title = false;   // circling the dais behind the title
    private float m_phase = 0.0f;   // position in the sweep (radians of the cycle)

    CameraRig(Entity@ entity) { @self = entity; }

    // Follows another entity from now on (a client's own hunter, which appears after the scene
    // loads).
    void onFollow(Entity@ who)
    {
        @target = who;
        if (target !is null && target.isValid())
        {
            m_focus = target.worldPosition();
        }
    }

    void onStart()
    {
        if (target !is null && target.isValid())
        {
            m_focus = target.worldPosition();
        }
        place();
    }

    // A frame of the title view, sent by Masquerade.as. `dt` is the real frame time while the title
    // is up; a value below zero means a round has begun, and the rig jumps back behind the player.
    void onTitleView(float dt)
    {
        if (dt < 0.0f)
        {
            m_title = false;
            if (target !is null && target.isValid())
            {
                m_focus = target.worldPosition();
            }
            m_yaw = float(m_quarter) * kPi * 0.5f;
            place();
            return;
        }
        Entity@ dais = self.scene.find("Dais");
        if (dais is null)
        {
            return;
        }
        m_title = true;
        m_phase += titleTurn * dt;
        float angle = titleSweep * Math::Sin(m_phase); // 0 is straight out from the wall (+Z)
        Float3 at = dais.worldPosition();
        look(Float3(at.x - Math::Sin(angle) * titleDistance, titleHeight, at.z + Math::Cos(angle) * titleDistance),
             Float3(at.x, at.y + titleLook, at.z));
    }

    void onUpdate(double dt)
    {
        if (m_title)
        {
            return;
        }
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
        m_yaw += delta * Math::Clamp(swing * d, 0.0f, 1.0f);
        if (target !is null && target.isValid())
        {
            Float3 at = target.worldPosition();
            float k = Math::Clamp(follow * d, 0.0f, 1.0f);
            m_focus = Float3(m_focus.x + (at.x - m_focus.x) * k, m_focus.y + (at.y - m_focus.y) * k,
                             m_focus.z + (at.z - m_focus.z) * k);
        }
        place();
    }

    // Places the camera behind the focus for the current angle: at yaw 0 it sits south of the
    // focus (+Z), looking north.
    private void place()
    {
        Float3 seat = Float3(m_focus.x - Math::Sin(m_yaw) * distance, m_focus.y + height,
                             m_focus.z + Math::Cos(m_yaw) * distance);
        look(seat, Float3(m_focus.x, m_focus.y + lookHeight, m_focus.z));
    }

    // Places the camera at `seat`, looking at `at`.
    private void look(Float3 seat, Float3 at)
    {
        self.setPosition(seat);
        Float3 dir = Float3(at.x - seat.x, at.y - seat.y, at.z - seat.z);
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
