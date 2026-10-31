// Stroller: crosses the title backdrop along X, from `xFrom` to `xTo`, then after a pause starts
// again from the beginning: a car on the road or a person on the pavement. It moves itself rather
// than through a navigation agent (the backdrop has no navmesh), and faces the way it goes.
//
// Its model is its first child. With a walk clip, the clip plays at the pace it moves (over the
// metres the clip covers at speed 1); a car has none.


class Stroller
{
    private Entity@ self;

    [-30.0, "Where each crossing starts (m along X)"] float xFrom;
    [30.0, "Where each crossing ends (m along X)"] float xTo;
    [1.4, "Speed (m/s)"] float speed;
    [6.0, "Longest pause between crossings (s)"] float pause;
    ["asset:AnimationClip", "The model's walk clip, if it walks"] Guid@ walkClip;
    [1.3, "Metres the walk clip covers at speed 1"] float walkMetres;

    private float m_x = 0.0f;
    private float m_wait = 0.0f;
    private Entity@ m_model;

    Stroller(Entity@ entity) { @self = entity; }

    void onStart()
    {
        // Start somewhere along the way, so the backdrop is busy from the first frame.
        m_x = Random::range(xFrom, xTo);
        float heading = (xTo > xFrom) ? 90.0f : -90.0f;
        self.setRotationEuler(0.0f, heading, 0.0f);
        place();
        @m_model = self.firstChild();
        if (m_model !is null && m_model.isValid() && walkClip !is null && !walkClip.IsNil())
        {
            SceneAnimation anim = SceneAnimation::of(self.scene);
            anim.setClip(m_model, walkClip);
            anim.play(m_model);
            SkeletalAnimationComponent::of(m_model).speed = speed / walkMetres;
        }
    }

    void onUpdate(double dt)
    {
        if (m_wait > 0.0f)
        {
            m_wait -= float(dt);
            if (m_wait <= 0.0f)
            {
                m_x = xFrom;
                place();
            }
            return;
        }
        float way = (xTo > xFrom) ? 1.0f : -1.0f;
        m_x += way * speed * float(dt);
        if ((m_x - xTo) * way >= 0.0f)
        {
            m_wait = Random::range(0.5f, pause);
        }
        place();
    }

    private void place()
    {
        Float3 p = self.position();
        self.setPosition(m_x, p.y, p.z);
    }
}
