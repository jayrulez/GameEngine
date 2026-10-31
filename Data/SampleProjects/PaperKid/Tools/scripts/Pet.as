// Pet: a dog or a cat on a lawn of the title backdrop. It trots to a spot on its lawn (the
// rectangle xMin..xMax by zMin..zMax), does something there (stands about, sits, lies down, or its
// own special action: the dog sniffs the grass, the cat grooms), then trots to another spot.
//
// Its model is its first child, with the clips the slots name (Tools/blender/animals.py). Every
// clip starts and ends in the same standing pose, so a switch at a clip's end does not snap; the
// held actions last their clip's length, and standing about loops Idle for a while.


class Pet
{
    private Entity@ self;

    [-12.0, "Lawn's left edge (m)"] float xMin;
    [-6.0, "Lawn's right edge (m)"] float xMax;
    [9.0, "Lawn's far edge (m)"] float zMin;
    [13.0, "Lawn's near edge (m)"] float zMax;
    [0.8, "Walking speed (m/s)"] float speed;
    [0.55, "Metres the walk clip covers at speed 1"] float walkMetres;
    ["asset:AnimationClip", "Walk (loops)"] Guid@ walkClip;
    ["asset:AnimationClip", "Standing about (loops)"] Guid@ idleClip;
    ["asset:AnimationClip", "Sitting down and up again"] Guid@ sitClip;
    [4.0, "Sit clip's length (s)"] float sitSeconds;
    ["asset:AnimationClip", "Lying down and up again"] Guid@ lieClip;
    [5.0, "Lie clip's length (s)"] float lieSeconds;
    ["asset:AnimationClip", "Its own thing (sniffing, grooming)"] Guid@ ownClip;
    [3.0, "Own clip's length (s)"] float ownSeconds;

    private Entity@ m_model;
    private bool m_walking = false;
    private float m_toX = 0.0f;
    private float m_toZ = 0.0f;
    private float m_yaw = 0.0f; // radians; 0 faces +Z
    private float m_left = 0.0f; // seconds left of the current action where it stands

    Pet(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_model = self.firstChild();
        m_yaw = Random::range(-3.14159f, 3.14159f);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
        // Start in the middle of something, not all walking off at once.
        doSomething();
        m_left = Random::range(0.5f, m_left);
    }

    void onUpdate(double dt)
    {
        if (!m_walking)
        {
            m_left -= float(dt);
            if (m_left <= 0.0f)
            {
                walkSomewhere();
            }
            return;
        }
        Float3 p = self.position();
        float dx = m_toX - p.x;
        float dz = m_toZ - p.z;
        float far = Math::Sqrt(dx * dx + dz * dz);
        if (far < 0.05f)
        {
            doSomething();
            return;
        }
        // Turn toward the spot (the short way round, easing), and walk the way it faces, so it
        // curves round rather than sliding sideways.
        float turn = Math::Atan2(dx, dz) - m_yaw;
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        float ease = 4.0f * float(dt);
        m_yaw += turn * ((ease < 1.0f) ? ease : 1.0f);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
        float step = speed * float(dt);
        if (step > far)
        {
            step = far;
        }
        // Slow while still turning a long way round, so it does not orbit the spot.
        float facing = Math::Cos(turn);
        if (facing < 0.2f)
        {
            facing = 0.2f;
        }
        step *= facing;
        self.setPosition(p.x + Math::Sin(m_yaw) * step, p.y, p.z + Math::Cos(m_yaw) * step);
        setPace(speed * facing);
    }

    private void walkSomewhere()
    {
        m_toX = Random::range(xMin, xMax);
        m_toZ = Random::range(zMin, zMax);
        m_walking = true;
        playClip(walkClip);
        setPace(speed);
    }

    private void doSomething()
    {
        m_walking = false;
        int pick = Random::intRange(0, 3);
        if (pick == 1 && hasClip(sitClip))
        {
            playClip(sitClip);
            m_left = sitSeconds;
        }
        else if (pick == 2 && hasClip(lieClip))
        {
            playClip(lieClip);
            m_left = lieSeconds;
        }
        else if (pick == 3 && hasClip(ownClip))
        {
            playClip(ownClip);
            m_left = ownSeconds;
        }
        else
        {
            playClip(idleClip);
            m_left = Random::range(2.0f, 5.0f);
        }
        if (m_model !is null && m_model.isValid())
        {
            SkeletalAnimationComponent::of(m_model).speed = 1.0f;
        }
    }

    private bool hasClip(Guid@ clip)
    {
        return clip !is null && !clip.IsNil();
    }

    private void playClip(Guid@ clip)
    {
        if (m_model is null || !m_model.isValid() || !hasClip(clip))
        {
            return;
        }
        SceneAnimation anim = SceneAnimation::of(self.scene);
        anim.setClip(m_model, clip);
        anim.play(m_model);
    }

    private void setPace(float pace)
    {
        if (m_model !is null && m_model.isValid())
        {
            SkeletalAnimationComponent::of(m_model).speed = pace / walkMetres;
        }
    }
}
