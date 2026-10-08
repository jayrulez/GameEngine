// Thief - the player: moves on the floor relative to where the camera looks (up on the stick is
// away from the camera, whichever quarter it has turned to), at a sneak (crouched), a walk or a
// run, and turns to face where it goes. The character controller keeps it on the floor and out of
// the walls; the model's graph (graph.py) follows by Speed and Crouched.

const float kPi = 3.14159265f;

float Saturate(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

class Thief
{
    private Entity@ self;

    [null, "The camera whose facing makes forward"] Entity@ camera;
    [1.0, "Sneaking speed (m/s): crouched, silent"] float sneakSpeed;
    [1.6, "Walking speed (m/s)"] float walkSpeed;
    [4.5, "Running speed (m/s): loud"] float runSpeed;
    [10.0, "How fast the body turns toward where it goes"] float turnRate;

    private float m_yaw = 0.0f;
    private Entity@ m_figure;

    Thief(Entity@ entity) { @self = entity; }

    // The model's skinned mesh, which carries the graph: Thief (the instance) / ThiefRig / Thief.
    private Entity@ figure()
    {
        if (m_figure is null || !m_figure.isValid())
        {
            Entity@ model = self.findChildByName("Thief");
            Entity@ rig = (model !is null && model.isValid()) ? model.findChildByName("ThiefRig") : null;
            @m_figure = (rig !is null && rig.isValid()) ? rig.findChildByName("Thief") : null;
        }
        return m_figure;
    }

    void onUpdate(double dt)
    {
        CharacterComponent@ body = CharacterComponent::of(self);
        bool crouched = Input::isDown("Sneak");
        animate(body, crouched);

        Float2 stick = Input::value2D("Move");
        float amount = Math::Sqrt(stick.x * stick.x + stick.y * stick.y);
        if (amount < 0.05f || camera is null || !camera.isValid())
        {
            body.move(0.0f, 0.0f);
            return;
        }
        if (amount > 1.0f)
        {
            stick = Float2(stick.x / amount, stick.y / amount);
        }
        // The camera's facing along the floor: forward, and right of it.
        Float3 look = Quaternion::RotateVector(camera.rotation(), Float3(0.0f, 0.0f, -1.0f));
        float flat = Math::Sqrt(look.x * look.x + look.z * look.z);
        if (flat < 0.0001f)
        {
            return;
        }
        Float3 forward = Float3(look.x / flat, 0.0f, look.z / flat);
        Float3 right = Float3(-forward.z, 0.0f, forward.x);
        float speed = crouched ? sneakSpeed : (Input::isDown("Run") ? runSpeed : walkSpeed);
        float vx = (forward.x * stick.y + right.x * stick.x) * speed;
        float vz = (forward.z * stick.y + right.z * stick.x) * speed;
        body.move(vx, vz);

        // Face where it goes (the model faces +Z), turning the short way round.
        float want = Math::Atan2(vx, vz);
        float delta = want - m_yaw;
        while (delta > kPi) delta -= 2.0f * kPi;
        while (delta < -kPi) delta += 2.0f * kPi;
        m_yaw += delta * Saturate(turnRate * float(dt));
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }

    // A door's lock being picked (Door.as): face it and stand still, the hand on it.
    void onPicking(Float3 at)
    {
        Float3 p = self.worldPosition();
        m_yaw = Math::Atan2(at.x - p.x, at.z - p.z);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }

    // The graph by what the body did: its speed along the floor (so a thief pressed against a wall
    // stands rather than runs on the spot), and crouched while sneaking.
    private void animate(CharacterComponent@ body, bool crouched)
    {
        Entity@ fig = figure();
        if (fig is null || !fig.isValid())
        {
            return;
        }
        Float3 v = body.velocity;
        SceneAnimation anim = SceneAnimation::of(self.scene);
        anim.setFloat(fig, "Speed", Math::Sqrt(v.x * v.x + v.z * v.z));
        anim.setBool(fig, "Crouched", crouched);
    }
}
