// TrackMark - one stretch of the board's track in the snow (Prefabs/TrackMark, effects.py): it
// lies as it was dropped, fades over the end of its life and removes itself, so the track behind
// the rider stays a few seconds long.
class TrackMark
{
    private Entity@ self;

    [6.0, "How long the mark lasts (s)"] float life;
    [2.0, "How long it takes to fade, at the end (s)"] float fade;

    private float m_age = 0.0f;
    private float m_alpha = -1.0f; // the decal's alpha as authored, read at the first update

    TrackMark(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        m_age += float(dt);
        Entity@ decal = self.firstChild();
        if (decal !is null && decal.isValid())
        {
            DecalComponent@ mark = DecalComponent::of(decal);
            Color c = mark.color;
            if (m_alpha < 0.0f)
            {
                m_alpha = c.a;
            }
            float left = life - m_age;
            float k = (left < fade) ? ((left > 0.0f) ? left / fade : 0.0f) : 1.0f;
            mark.color = Color(c.r, c.g, c.b, m_alpha * k);
        }
        if (m_age >= life)
        {
            self.destroy();
        }
    }
}
