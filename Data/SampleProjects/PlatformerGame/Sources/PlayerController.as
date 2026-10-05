// PlayerController - the player's movement, jump, facing and animation.
//
// On the Player entity, which carries a Character component (a kinematic capsule: its
// position is the capsule's centre). The model is the entity's first child, the Character
// prefab instance, whose root plays the skeletal clips.
//
// The camera looks down -Z from behind, so the "Move" axis maps straight to the ground plane:
// W (+Y) walks toward -Z, D (+X) toward +X.
//
// A fall or a hit is paced so the player can find themselves again: a poof where they went, a
// moment out of sight while the camera glides back to the safe ground (the player already moved
// there, facing the way they stood on it), a poof as they reappear, then a moment of blinking
// in which nothing can hurt. Safe ground is a spot stood still on with ground all round it: near
// an edge the nearest spot that has is kept instead, so a respawn never starts half off a ledge.
//
// The pad rumbles with the game: a heavy jolt for a fall or a hit, a sharp kick off a stomped
// enemy, a thud for a hard landing, and a happy pulse at the flag.

// The Character model's clips (Models/Character/Character).
Guid kIdleClip = Guid("9208bf59-c1d7-4556-baf2-4535e81488c9");
Guid kRunClip = Guid("b1c122af-808a-4559-b67a-cf0c4616f66d");
Guid kJumpClip = Guid("beb6416f-984c-4697-a5d5-44174e71a131");

// The effects (FX/*): dust at the feet on a jump and a hard landing, a poof on a respawn.
Guid kDust = Guid("58fc6a1d-7c94-421a-a779-bc20eca57828");
Guid kPoof = Guid("7e495983-084c-4088-b6a6-aacfc9147209");
/// From the capsule's centre to its feet: half height plus radius.
const float kFeet = 0.95f;

// The sounds (Audio/*).
Guid kJumpSound = Guid("5469a0b2-67e2-44e1-ba92-8d9e4704bb4f");
Guid kHurtSound = Guid("cff3d6f1-4a6a-44e3-8a80-c0ebca52913a");
Guid kFallSound = Guid("b002f30b-91a4-42a5-9ee7-34755d09b468");

class PlayerController
{
    private Entity@ self;

    [7.0, "Top run speed (m/s)"] float moveSpeed;
    [40.0, "Ground acceleration (m/s^2)"] float acceleration;
    [12.0, "Air acceleration (m/s^2)"] float airAcceleration;
    [9.5, "Jump launch speed (m/s)"] float jumpSpeed;
    [0.12, "Coyote time: a jump still counts this long after leaving a ledge (s)"] float coyoteTime;
    [0.12, "Jump buffer: a press this early before landing still jumps (s)"] float jumpBuffer;
    [14.0, "Turn rate toward the move direction (rad/s)"] float turnSpeed;
    [-15.0, "Below this height the player falls out and respawns"] float killHeight;
    [0.4, "Standing still this long on ground makes it the respawn point (s)"] float safeGroundTime;
    [8.0, "Bounce speed off a stomped enemy (m/s)"] float bounceSpeed;
    [0.8, "Out of sight after a fall or a hit, while the camera glides back (s)"] float respawnDelay;
    [1.2, "Blinking after reappearing, when nothing can hurt (s)"] float graceTime;
    [0.7, "Safe ground reaches at least this far round the spot on every side (m)"] float safeMargin;

    // ---- runtime ----
    private float m_velX = 0.0f;
    private float m_velZ = 0.0f;
    private float m_yaw = 0.0f;
    private float m_sinceGrounded = 0.0f;
    private float m_sinceJumpPressed = 1000.0f;
    private int m_anim = -1; // 0 idle, 1 run, 2 jump
    private Float3 m_spawn = Float3(0.0f, 0.0f, 0.0f);
    private Entity@ m_model;
    private int m_deaths = 0;
    /// Seconds left of the grace after a respawn, while a hazard still touching cannot hurt.
    private float m_invulnerable = 0.0f;
    /// How long the player has stood grounded; past a moment, where it stands is safe ground.
    private float m_groundedFor = 0.0f;
    /// Grounded last frame, and how long the current time in the air has lasted.
    private bool m_wasGrounded = true;
    private float m_airTime = 0.0f;
    /// The level is won: no more input, the player stands and enjoys it.
    private bool m_celebrating = false;
    /// Seconds left out of sight before reappearing (below 0: not respawning).
    private float m_respawning = -1.0f;
    /// The current still moment has been looked at for safe ground (once per stand).
    private bool m_safeChecked = false;
    /// The facing the player had on the safe ground, which a respawn gives back.
    private float m_spawnYaw = 0.0f;
    private Entity@ m_camera;

    PlayerController(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_spawn = self.worldPosition();
        @m_model = self.firstChild();
        @m_camera = self.scene.find("Camera");
        playAnim(0);
    }

    void onUpdate(double delta)
    {
        float dt = float(delta);
        if (dt <= 0.0f)
        {
            return;
        }
        if (m_invulnerable > 0.0f)
        {
            m_invulnerable -= dt;
        }
        CharacterComponent@ character = CharacterComponent::of(self);
        if (m_respawning >= 0.0f)
        {
            // Out of sight at the safe ground, standing still while the camera arrives.
            character.move(0.0f, 0.0f);
            m_respawning -= dt;
            if (m_respawning < 0.0f)
            {
                showModel(true);
                ScenePrefabs::of(self.scene).spawn(kPoof, self.worldPosition());
            }
            return;
        }
        if (m_invulnerable > 0.0f)
        {
            // The grace after reappearing: a blink, ten times a second.
            showModel(int(m_invulnerable * 10.0f) % 2 == 0);
        }
        else
        {
            showModel(true);
        }
        bool grounded = character.grounded();
        // The feet stand on the ground under them only while the hero stands on it: in the air the
        // jump pose holds them at the model's ground, and they would reach for the floor below.
        FootIkComponent@ feet = FootIkComponent::of(self);
        if (feet !is null)
        {
            feet.active = grounded;
        }
        m_sinceGrounded = grounded ? 0.0f : m_sinceGrounded + dt;
        // A hard landing kicks up dust; a hop off a step does not.
        if (grounded && !m_wasGrounded && (m_airTime > 0.35f))
        {
            dust();
            Input::rumble(0.3f, 0.1f, 0.08f); // the thud of landing
        }
        m_airTime = grounded ? 0.0f : m_airTime + dt;
        m_wasGrounded = grounded;
        // Safe ground: a spot stood STILL on for a moment becomes the respawn point, so a fall
        // costs one jump, not the level. Still, because walking into a hazard passes over
        // ground that is not safe; standing still inside a hazard's reach hurts at once.
        bool still = (m_velX * m_velX + m_velZ * m_velZ) < 0.25f;
        m_groundedFor = (grounded && still) ? m_groundedFor + dt : 0.0f;
        if (m_groundedFor <= 0.0f)
        {
            m_safeChecked = false;
        }
        if ((m_groundedFor > safeGroundTime) && (m_invulnerable <= 0.0f) && !m_safeChecked)
        {
            m_safeChecked = true;
            Float3 spot = self.worldPosition();
            if (findSafeSpot(self.worldPosition(), spot))
            {
                m_spawn = spot + Float3(0.0f, 0.1f, 0.0f);
                m_spawnYaw = m_yaw;
            }
        }
        m_sinceJumpPressed = Input::wasPressed("Jump") ? 0.0f : m_sinceJumpPressed + dt;

        // Steer the horizontal velocity toward the stick, snappier on the ground.
        Float2 move = m_celebrating ? Float2(0.0f, 0.0f) : Input::value2D("Move");
        float targetX = move.x * moveSpeed;
        float targetZ = -move.y * moveSpeed;
        float rate = (grounded ? acceleration : airAcceleration) * dt;
        m_velX = approach(m_velX, targetX, rate);
        m_velZ = approach(m_velZ, targetZ, rate);
        character.move(m_velX, m_velZ);

        // Jump: buffered, and allowed a moment after walking off a ledge.
        if (!m_celebrating && (m_sinceJumpPressed <= jumpBuffer) && (m_sinceGrounded <= coyoteTime))
        {
            character.jump(jumpSpeed);
            dust();
            Audio::playOneShot(kJumpSound, AudioBus::Effects, 0.8f);
            m_sinceJumpPressed = 1000.0f;
            m_sinceGrounded = 1000.0f;
        }

        // Face the way we move.
        float speed = Math::Sqrt(m_velX * m_velX + m_velZ * m_velZ);
        if (speed > 0.5f)
        {
            float wanted = Math::Atan2(m_velX, m_velZ);
            m_yaw = turnToward(m_yaw, wanted, turnSpeed * dt);
            self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_yaw));
        }

        if (!grounded)
        {
            playAnim(2);
        }
        else if (speed > 0.5f)
        {
            playAnim(1);
        }
        else
        {
            playAnim(0);
        }

        // The grace after a respawn also covers the frame or two before the teleport lands,
        // when the player is still below the kill height.
        if ((m_invulnerable <= 0.0f) && (self.worldPosition().y < killHeight))
        {
            Audio::playOneShot(kFallSound);
            respawn();
        }
    }

    // Came down on an enemy: bounce off it, in the air as on the ground.
    void onBounce()
    {
        CharacterComponent::of(self).launch(bounceSpeed);
        m_sinceGrounded = 1000.0f; // the bounce is not a coyote jump
        Input::rumble(0.25f, 0.6f, 0.12f); // a sharp kick off the enemy
    }

    // A hazard or an enemy touched the player: the same as a fall, with a jolt of the camera.
    void onHurt()
    {
        if ((m_invulnerable <= 0.0f) && !m_celebrating)
        {
            shake(0.35f);
            Audio::playOneShot(kHurtSound);
            respawn();
        }
    }

    // The flag is reached: stop taking input and stand still for the celebration.
    void onCelebrate()
    {
        m_celebrating = true;
        Input::rumble(0.2f, 0.5f, 0.35f); // a happy pulse
    }

    // A fall or a hit: a poof where the player went, then back to the last safe ground with no
    // momentum, out of sight until the camera is there (onUpdate brings them back).
    void respawn()
    {
        if (m_respawning >= 0.0f)
        {
            return; // already on the way back
        }
        m_deaths += 1;
        m_velX = 0.0f;
        m_velZ = 0.0f;
        ScenePrefabs::of(self.scene).spawn(kPoof, self.worldPosition());
        Input::rumble(0.8f, 0.5f, 0.3f); // the jolt of a fall or a hit
        CharacterComponent::of(self).setPosition(m_spawn);
        m_yaw = m_spawnYaw;
        self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_yaw));
        showModel(false);
        playAnim(0);
        m_respawning = respawnDelay;
        m_invulnerable = respawnDelay + graceTime;
        self.scene.events.emit("PlayerDied", m_deaths);
    }

    private void showModel(bool shown)
    {
        if (m_model !is null && m_model.isValid() && m_model.active() != shown)
        {
            m_model.setActive(shown);
        }
    }

    // The spot nearest `at` (searched on a 0.4 m grid out to 0.8 m) where the ground reaches
    // safeMargin past it on all four sides, level with the player's feet: false when there is none.
    private bool findSafeSpot(Float3 at, Float3 &out spot)
    {
        array<float> steps = {0.0f, -0.4f, 0.4f, -0.8f, 0.8f};
        float feet = at.y - kFeet;
        float best = 1000.0f;
        bool found = false;
        for (uint i = 0; i < steps.length(); i++)
        {
            for (uint j = 0; j < steps.length(); j++)
            {
                float x = at.x + steps[i];
                float z = at.z + steps[j];
                float distance = steps[i] * steps[i] + steps[j] * steps[j];
                if (distance < best && groundAt(x, z, feet) && groundAt(x + safeMargin, z, feet)
                    && groundAt(x - safeMargin, z, feet) && groundAt(x, z + safeMargin, feet)
                    && groundAt(x, z - safeMargin, feet))
                {
                    best = distance;
                    spot = Float3(x, at.y, z);
                    found = true;
                }
            }
        }
        return found;
    }

    // Flat ground under (x, z) level with the player's feet. The islands' tops are flat and their
    // edges rounded, so a probe on the bevel (a little lower) counts as the edge, not as ground.
    private bool groundAt(float x, float z, float feet)
    {
        RayCastHit@ hit = ScenePhysics::of(self.scene).rayCast(Float3(x, feet + 0.6f, z),
                                                                 Float3(0.0f, -1.0f, 0.0f), 1.2f);
        return hit !is null && hit.hit && Math::Abs(hit.position.y - feet) < 0.08f;
    }

    private void dust()
    {
        ScenePrefabs::of(self.scene).spawn(kDust, self.worldPosition() - Float3(0.0f, kFeet - 0.05f, 0.0f));
    }

    private void shake(float strength)
    {
        if (m_camera !is null && m_camera.isValid())
        {
            m_camera.send("Shake", strength);
        }
    }

    private void playAnim(int which)
    {
        if (which == m_anim || m_model is null || !m_model.isValid())
        {
            return;
        }
        m_anim = which;
        Guid clip = kIdleClip;
        if (which == 1)
        {
            clip = kRunClip;
        }
        else if (which == 2)
        {
            clip = kJumpClip;
        }
        SceneAnimation animation = SceneAnimation::of(self.scene);
        animation.setClip(m_model, clip);
        animation.play(m_model);
    }

    private float approach(float value, float target, float step)
    {
        if (value < target)
        {
            return (value + step > target) ? target : value + step;
        }
        return (value - step < target) ? target : value - step;
    }

    // Turn an angle toward another the short way round, by at most `step`.
    private float turnToward(float from, float to, float step)
    {
        float pi = 3.14159265f;
        float delta = to - from;
        while (delta > pi) { delta -= 2.0f * pi; }
        while (delta < -pi) { delta += 2.0f * pi; }
        if (Math::Abs(delta) <= step)
        {
            return to;
        }
        return from + ((delta > 0.0f) ? step : -step);
    }
}
