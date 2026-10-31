// Pickup: something worth taking that is not a coin, a heart (a life) or a gem (a bonus). It spins
// and bobs like a coin. When the player comes within reach it is taken: the event named in
// `takenEvent` goes out with `value` ("LifeCollected", "GemCollected"), a sparkle and a chime mark
// it, and it disappears. The script sits on the pickup's entity, whose first child is its model.

// FX/FxCoinSparkle: the burst where something was taken.
Guid kTakenSparkle = Guid("245b459c-4b93-4876-9496-6e4eb3daa145");

// Audio/powerUp7 (Kenney Digital Audio, CC0), played higher than a coin's.
Guid kTakenSound = Guid("6d963695-67f8-4b7f-9cf4-d234f8034116");

class Pickup
{
    private Entity@ self;

    ["LifeCollected", "The event announcing it was taken"] string takenEvent;
    [1, "The event's value"] int value;
    [1.3, "Pickup reach from the player's centre (m)"] float radius;
    [2.0, "Spin rate (rad/s)"] float spinSpeed;
    [0.25, "Bob height (m)"] float bobHeight;
    [1.3, "The chime's pitch"] float chimePitch;

    private Entity@ m_player;
    private Float3 m_home = Float3(0.0f, 0.0f, 0.0f);
    private float m_time = 0.0f;

    Pickup(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_player = self.scene.find("Player");
        m_home = self.position();
    }

    void onUpdate(double dt)
    {
        m_time += float(dt);
        self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_time * spinSpeed));
        self.setPosition(Float3(m_home.x, m_home.y + Math::Sin(m_time * 2.0f) * bobHeight, m_home.z));
        if (m_player !is null && m_player.isValid()
            && (Float3::Distance(m_player.worldPosition(), self.worldPosition()) < radius))
        {
            self.scene.events.emit(takenEvent, value);
            ScenePrefabs::of(self.scene).spawn(kTakenSparkle, self.worldPosition());
            Audio::playOneShot(kTakenSound, AudioBus::Effects, 0.8f, chimePitch);
            Input::rumble(0.1f, 0.5f, 0.12f);
            self.destroy();
        }
    }
}
