// Gem - a pickup off the safe line: it spins and bobs, and when the rider comes within reach it is
// taken ("GemCollected"), with a sparkle and a chime. Taken, its model hides; the gem itself stays,
// so a new run ("RunRestart") brings it back. On the gem's entity, whose first child is the Gem
// model. Placed by Tools/course.py, in rows beside the course between the gates.

// Prefabs/GemSparkle (gems.py): the burst where a gem was taken.
Guid kGemSparkle = Guid("4aa1e781-c982-4a98-952a-551a1aa43865");

// Audio/GemChime (sounds.py): the chime of a gem taken.
Guid kGemChime = Guid("c47f1131-6df4-49ea-b2c7-6e3b3316a3ce");

class Gem
{
    private Entity@ self;

    [1.4, "Pickup reach from the rider's centre (m)"] float radius;
    [2.5, "Spin rate (rad/s)"] float spinSpeed;
    [0.15, "Bob height (m)"] float bobHeight;
    [1, "Gems this one counts for"] int value;

    private Entity@ m_rider;
    private Entity@ m_model;
    private bool m_taken = false;
    private float m_time = 0.0f;

    Gem(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
        @m_model = self.firstChild();
        // Gems along the course bob out of step with each other.
        m_time = self.worldPosition().z * 0.37f;
        self.scene.events.emit("GemRegistered", value);
    }

    void onUpdate(double dt)
    {
        if (m_taken || m_model is null || !m_model.isValid())
        {
            return;
        }
        m_time += float(dt);
        m_model.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_time * spinSpeed));
        m_model.setPosition(Float3(0.0f, Math::Sin(m_time * 2.2f) * bobHeight, 0.0f));
        if (m_rider is null || !m_rider.isValid())
        {
            return;
        }
        // The rider is top-level: position() is this frame's pose (worldPosition() was last frame's).
        if (Float3::Distance(m_rider.position(), self.worldPosition()) < radius)
        {
            m_taken = true;
            m_model.setActive(false);
            self.scene.events.emit("GemCollected", value);
            ScenePrefabs::of(self.scene).spawn(kGemSparkle, self.worldPosition());
            Audio::playOneShot(kGemChime, AudioBus::Effects, 0.6f);
        }
    }

    // A new run: the gem is back for the taking.
    void onRunRestart(int unused)
    {
        m_taken = false;
        if (m_model !is null && m_model.isValid())
        {
            m_model.setActive(true);
        }
    }
}
