// Coin - a pickup: it spins and bobs, and when the player comes within reach it is collected
// ("CoinCollected") and gone. On the coin's entity, whose first child is the Coin model.
// FX/FxCoinSparkle: the burst where a coin was taken.
Guid kCoinSparkle = Guid("245b459c-4b93-4876-9496-6e4eb3daa145");

// Audio/powerUp7 (Kenney Digital Audio, CC0): the chime of a coin taken.
Guid kCoinSound = Guid("6d963695-67f8-4b7f-9cf4-d234f8034116");

// The hero looks at the nearest coin within its reach (an AimIkComponent on the Player). Coins
// share these, one script module: the first coin to update in a frame starts it over.
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
                aim.active = false; // a coin still in reach takes the look back next frame
            }
            self.destroy();
        }
    }

    // The nearest coin in reach this frame turns the hero's head to it; none, and the head eases back.
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
        if (distance < g_lookNearest)
        {
            g_lookNearest = distance;
            SceneAnimation::of(self.scene).setIkTarget(m_player, self.worldPosition());
            aim.active = true;
        }
    }
}
