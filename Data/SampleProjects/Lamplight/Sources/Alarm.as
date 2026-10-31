// Alarm: the bell on the upper floor. When a guard gives chase (the Spotted event), it rings. It
// emits a Noise where the thief was seen, loud enough for every guard on the floor to hear and come
// to look; a guard who is already chasing keeps chasing. It also emits Alarm so the game can tell
// the player. After ringing it rests for `rest` seconds before it can ring again.
class Alarm
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;
    [40.0, "How loud: a noise of loudness 1 is heard 14 m away"] float loudness;
    [8.0, "Seconds before it can ring again"] float rest;

    private float m_resting = 0.0f;

    Alarm(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        if (m_resting > 0.0f)
        {
            m_resting -= float(dt);
        }
    }

    void onSpotted(int unused)
    {
        if (m_resting > 0.0f || thief is null || !thief.isValid())
        {
            return;
        }
        m_resting = rest;
        Float3 at = thief.worldPosition();
        Audio::playOneShot3DPath("Audio/Bell", at.x, at.y, at.z);
        self.scene.events.emit("Noise", Float4(at.x, at.y, at.z, loudness));
        self.scene.events.emit("Alarm", at);
    }
}
