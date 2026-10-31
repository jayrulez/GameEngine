// Paper: a thrown newspaper. When it enters any trigger it tells that trigger's entity it arrived
// ("PaperArrived"). Only a subscriber's delivery zone responds, so nothing here needs to know which
// triggers are porches. Its first hard landing kicks up a puff of dust. It disappears a while after
// it is thrown, delivered or not.

Guid kFxPuff = Guid("9f997649-9c7f-48ed-99c4-6465890a41e9");

class Paper
{
    private Entity@ self;

    [6.0, "Seconds before the paper is gone"] float lifetime;

    private float m_age = 0.0f;
    private bool m_landed = false;

    Paper(Entity@ entity) { @self = entity; }

    void onTriggerEnter(Entity@ other)
    {
        other.send("PaperArrived", 0);
    }

    void onContactBegin(Entity@ other, Float3 point, Float3 normal, float speed)
    {
        if (m_landed || speed < 2.0f)
        {
            return;
        }
        m_landed = true;
        ScenePrefabs::of(self.scene).spawn(kFxPuff, point);
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
