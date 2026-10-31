// Pebble: a stone thrown by Thief.as. Its first hard landing clacks and emits a Noise there, loud
// enough to draw away a guard who hears it (Guard.as) to look. It disappears a while later.
class Pebble
{
    private Entity@ self;

    [1.0, "How loud its landing is (0..1)"] float loudness;
    [1.5, "Landing slower than this (m/s) is not a clack"] float hardLanding;
    [6.0, "Seconds before the pebble is gone"] float lifetime;

    private float m_age = 0.0f;
    private bool m_landed = false;

    Pebble(Entity@ entity) { @self = entity; }

    void onContactBegin(Entity@ other, Float3 point, Float3 normal, float speed)
    {
        if (m_landed || speed < hardLanding)
        {
            return;
        }
        m_landed = true;
        Audio::playOneShot3DPath("Audio/StepStone2", point.x, point.y, point.z);
        self.scene.events.emit("Noise", Float4(point.x, point.y, point.z, loudness));
    }

    void onUpdate(double dt)
    {
        m_age += float(dt);
        if (m_age >= lifetime)
        {
            self.destroy();
        }
    }
}
