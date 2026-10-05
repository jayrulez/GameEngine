// Board - the rider on the snow (P0: on autopilot down the course, for the first scene's
// measurements; P1 gives it the player's stick).
//
// A grounded character moves only by its input, so the board drives the whole velocity
// (CharacterComponent.drive): each frame, gravity less its part along the ground's normal is added
// to the velocity the rider has, which is kept along the ground; snow friction and air drag take
// some back. Steering turns that velocity toward a point down the course line, at a carve rate,
// keeping its speed. In the air, gravity alone. At the finish, back to the top.
class Board
{
    private Entity@ self;

    [null, "The course line (an entity with a spline)"] Entity@ course;
    [15.0, "How far down the course line the autopilot aims (m)"] float lookAhead;
    [70.0, "Fastest turn of the velocity (degrees a second)"] float carveRate;
    [0.04, "Snow friction (share of gravity's pull into the slope)"] float friction;
    [0.0025, "Air drag (per m/s of speed, per second)"] float drag;
    [6.0, "Distance before the end of the course line that counts as the finish (m)"] float finishMargin;

    private Float3 m_start;
    private float m_yaw = 0.0f; // radians; 0 faces +Z

    Board(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_start = self.position();
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (d <= 0.0f)
        {
            return;
        }
        CharacterComponent@ c = CharacterComponent::of(self);
        Float3 v = c.velocity;
        Float3 at = self.position();
        const float g = 9.81f;
        if (c.grounded())
        {
            Float3 n = c.groundNormal;
            // Keep the velocity along the ground, then add gravity's pull along it.
            float vn = v.x * n.x + v.y * n.y + v.z * n.z;
            v = Float3(v.x - n.x * vn, v.y - n.y * vn, v.z - n.z * vn);
            float gn = -g * n.y; // gravity (0, -g, 0) along the normal
            Float3 along = Float3(-n.x * gn, -g - n.y * gn, -n.z * gn);
            v = Float3(v.x + along.x * d, v.y + along.y * d, v.z + along.z * d);
            // Friction (from the press into the slope) and drag, both against the motion.
            float speed = Math::Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > 0.01f)
            {
                float slow = (friction * g * n.y + drag * speed * speed) * d;
                float keep = (speed > slow) ? (speed - slow) / speed : 0.0f;
                v = Float3(v.x * keep, v.y * keep, v.z * keep);
            }
            v = steer(v, at, travelled(at), d);
        }
        else
        {
            v = Float3(v.x, v.y - g * d, v.z);
        }
        c.drive(v);
        face(v, d);
        if (course !is null && course.isValid() &&
            travelled(at) > SceneSplines::of(self.scene).length(course) - finishMargin)
        {
            c.setPosition(m_start);
        }
    }

    // How far down the course line the rider is (m): the closest point's distance along it.
    private float travelled(Float3 at)
    {
        if (course is null || !course.isValid())
        {
            return 0.0f;
        }
        SplineHit@ here = SceneSplines::of(self.scene).closestPoint(course, at);
        return (here !is null && here.valid) ? here.distance : 0.0f;
    }

    // Turns the velocity's ground direction toward a point down the course line, keeping its speed.
    private Float3 steer(Float3 v, Float3 at, float down, float d)
    {
        if (course is null || !course.isValid())
        {
            return v;
        }
        SceneSplines@ splines = SceneSplines::of(self.scene);
        float length = splines.length(course);
        float aim = down + lookAhead;
        SplineHit@ ahead = splines.sampleAtDistance(course, (aim < length) ? aim : length);
        float flat = Math::Sqrt(v.x * v.x + v.z * v.z);
        float want = Math::Atan2(ahead.position.x - at.x, ahead.position.z - at.z);
        float have = (flat > 0.2f) ? Math::Atan2(v.x, v.z) : want;
        float turn = want - have;
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        float most = Math::DegreesToRadians(carveRate) * d;
        if (turn > most) { turn = most; }
        if (turn < -most) { turn = -most; }
        float heading = have + turn;
        return Float3(Math::Sin(heading) * flat, v.y, Math::Cos(heading) * flat);
    }

    // The rider faces where it goes, easing round.
    private void face(Float3 v, float d)
    {
        if (v.x * v.x + v.z * v.z < 0.04f)
        {
            return;
        }
        float turn = Math::Atan2(v.x, v.z) - m_yaw;
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        float ease = 8.0f * d;
        m_yaw += turn * ((ease < 1.0f) ? ease : 1.0f);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }
}
