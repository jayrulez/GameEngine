// Expire - removes its entity once it has lived `life` seconds: on a one-shot effect's prefab
// (Prefabs/GemSparkle, gems.py), so a spawned burst does not stay in the scene after it is spent.
class Expire
{
    private Entity@ self;

    [1.5, "How long the entity lasts (s)"] float life;

    private float m_age = 0.0f;

    Expire(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        m_age += float(dt);
        if (m_age >= life)
        {
            self.destroy();
        }
    }
}
