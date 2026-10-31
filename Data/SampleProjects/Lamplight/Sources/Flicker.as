// Flicker: makes a flame's light waver. Its intensity wanders around the authored value, following
// two slow waves and a quick one, so a torch breathes rather than burns flat. A light that is off
// (a torch put out through Lamp.as) stays off.
class Flicker
{
    private Entity@ self;

    [0.18, "How far the intensity wanders, as a share of its own"] float amount;
    [1.0, "How fast it wanders"] float rate;

    private float m_base = 0.0f;
    private float m_time = 0.0f;

    Flicker(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_base = LightComponent::of(self).intensity;
        m_time = self.worldPosition().x * 1.7f + self.worldPosition().z * 0.9f; // a different phase for each torch
    }

    void onUpdate(double dt)
    {
        m_time += float(dt) * rate;
        float wave = 0.5f * Math::Sin(m_time * 2.3f) + 0.3f * Math::Sin(m_time * 5.1f + 1.3f)
                     + 0.2f * Math::Sin(m_time * 13.7f + 0.4f);
        LightComponent::of(self).intensity = m_base * (1.0f + amount * wave);
    }
}
