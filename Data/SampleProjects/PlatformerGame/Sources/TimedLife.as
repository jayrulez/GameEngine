// TimedLife: removes its entity a while after it appears. One-shot effect prefabs carry it, so
// whoever spawns one can forget about it.
class TimedLife
{
    private Entity@ self;

    [3.0, "Seconds before the entity removes itself"] float seconds;

    private float m_age = 0.0f;

    TimedLife(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        m_age += float(dt);
        if (m_age >= seconds)
        {
            self.destroy();
        }
    }
}
