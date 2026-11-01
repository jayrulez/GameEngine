// Coin: a pickup that spins and bobs. When the player comes within reach it is collected
// ("CoinCollected") and disappears. The script sits on the coin's entity, whose first child is the
// Coin model.
// FX/FxCoinSparkle: the burst where a coin was taken.
Guid kCoinSparkle = Guid("245b459c-4b93-4876-9496-6e4eb3daa145");

// Audio/powerUp7 (Kenney Digital Audio, CC0): the chime of a coin taken.
Guid kCoinSound = Guid("6d963695-67f8-4b7f-9cf4-d234f8034116");

// The hero looks at the nearest coin within reach (through an AimIkComponent on the Player) that it
// can turn its head to: one in front of it, not too steeply above or below. A coin behind it would
// twist the head over backwards, so it does not count. All coins share these globals, since they
// are one script module; the first coin to update in a frame resets them.
double g_lookFrame = -1.0;
float g_lookNearest = 0.0f;

class Coin
{
    private Entity@ self;

    [1.3, "Pickup reach from the player's centre (m)"] float radius;
    [3.0, "Spin rate (rad/s)"] float spinSpeed;
    [0.2, "Bob height (m)"] float bobHeight;
    [1, "Points"] int value;
    [7.0, "The hero looks at the nearest coin within this (m)"] float lookRadius;
    [55.0, "... and within this turn from where the hero faces (degrees)"] float lookTurn;
    [35.0, "... and within this angle above or below its eyes (degrees)"] float lookPitch;

    private Entity@ m_player;
    private Float3 m_home = Float3(0.0f, 0.0f, 0.0f);
    private float m_time = 0.0f;

    Coin(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_player = self.scene.find("Player");
        m_home = self.position();
        self.scene.events.emit("CoinRegistered", 1);
    }

    void onUpdate(double dt)
    {
        m_time += float(dt);
        self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_time * spinSpeed));
        self.setPosition(Float3(m_home.x, m_home.y + Math::Sin(m_time * 2.5f) * bobHeight, m_home.z));
        if (m_player is null || !m_player.isValid())
        {
            return;
        }
        float distance = Float3::Distance(m_player.worldPosition(), self.worldPosition());
        look(distance);
        if (distance < radius)
        {
            self.scene.events.emit("CoinCollected", value);
            ScenePrefabs::of(self.scene).spawn(kCoinSparkle, self.worldPosition());
            Audio::playOneShot(kCoinSound, AudioBus::Effects, 0.7f);
            AimIkComponent@ aim = AimIkComponent::of(m_player);
            if (aim !is null)
            {
                aim.active = false; // a coin still in reach takes over the look next frame
            }
            self.destroy();
        }
    }

    // The nearest coin in reach this frame turns the hero's head toward it; with none, the head
    // eases back.
    private void look(float distance)
    {
        AimIkComponent@ aim = AimIkComponent::of(m_player);
        if (aim is null)
        {
            return;
        }
        double frame = SceneScripts::of(self.scene).elapsed();
        if (frame != g_lookFrame)
        {
            g_lookFrame = frame;
            g_lookNearest = lookRadius;
            aim.active = false;
        }
        if (distance < g_lookNearest && inView())
        {
            g_lookNearest = distance;
            SceneAnimation::of(self.scene).setIkTarget(m_player, self.worldPosition());
            aim.active = true;
        }
    }

    // Whether the hero can turn its head to this coin: both the turn away from its facing and the
    // angle above or below must be within their limits. The hero's centre stands in for its eyes
    // (the head is about 0.1 m below it).
    private bool inView()
    {
        Float3 to = self.worldPosition() - m_player.worldPosition();
        float flat = Math::Sqrt(to.x * to.x + to.z * to.z);
        if (flat < 0.01f)
        {
            return false; // straight above or below
        }
        float degrees = 3.14159265f / 180.0f;
        if (Math::Abs(Math::Atan2(to.y, flat)) > lookPitch * degrees)
        {
            return false;
        }
        Float3 facing = Quaternion::RotateVector(m_player.worldRotation(), Float3(0.0f, 0.0f, 1.0f));
        float facingFlat = Math::Sqrt(facing.x * facing.x + facing.z * facing.z);
        if (facingFlat < 0.01f)
        {
            return false;
        }
        float along = (facing.x * to.x + facing.z * to.z) / (facingFlat * flat);
        return along >= Math::Cos(lookTurn * degrees);
    }
}
