// Alarm - the upper floor's bell: when a guard gives chase (Spotted), it rings, and every guard on
// the floor hears it and comes to where the thief was seen (a Noise there, loud enough for the whole
// floor; a guard already giving chase keeps chasing). The game says so (Alarm). Once rung it rests
// a while before it can ring again.
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
