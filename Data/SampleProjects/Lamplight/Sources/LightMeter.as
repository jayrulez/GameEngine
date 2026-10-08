// LightMeter - how lit the thief is, on the HUD: the light reaching the chest (the renderer's own
// lights, falloff and shadows, SceneRender.lightAt), as a meter that eases toward it and a word for
// it. Darkness is cover; the guards (P2) read the same light.
class LightMeter
{
    private Entity@ self;

    ["asset:UIDocument", "The HUD with the meter (UI/Hud)"] Guid@ hud;
    [0.4, "Where the light is read, above the thief's centre (m): the chest"] float chestHeight;
    [0.15, "The light (luminance) that fills half the meter"] float halfLight;
    [6.0, "How quickly the meter follows the light"] float follow;

    private float m_shown = 0.0f;
    private ProgressBar@ m_bar;
    private Label@ m_label;

    LightMeter(Entity@ entity) { @self = entity; }

    // How lit, 0..1: the luminance over itself plus `halfLight`, so the dark between the lamps
    // reads near empty, moonlight about half and standing under a lamp near full.
    float level()
    {
        Float3 p = self.worldPosition();
        Float3 light = SceneRender::of(self.scene).lightAt(Float3(p.x, p.y + chestHeight, p.z));
        float luminance = 0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z;
        return luminance / (luminance + halfLight);
    }

    void onStart()
    {
        if (hud !is null)
        {
            ui::push(hud);
        }
        @m_bar = ui::findProgressBar("hud-light");
        @m_label = ui::findLabel("hud-light-label");
        m_shown = level();
    }

    void onUpdate(double dt)
    {
        float k = follow * float(dt);
        m_shown += (level() - m_shown) * (k > 1.0f ? 1.0f : k);
        if (m_bar !is null && m_bar.isValid())
        {
            m_bar.setValue(m_shown);
        }
        if (m_label !is null && m_label.isValid())
        {
            m_label.setText(m_shown < 0.25f ? "Hidden" : (m_shown < 0.6f ? "Dim" : "Lit"));
        }
    }
}
