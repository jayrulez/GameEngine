// LightMeter: shows on the HUD how lit the thief is. It measures the light reaching his chest with
// SceneRender.lightAt, which uses the renderer's own lights, falloff and shadows, and shows it as a
// meter that eases toward the value, with a word for it. Darkness is cover, and the guards read the
// same light. In the dark the player would lose sight of him too, so as the meter empties his
// figure takes on a faint cold glow (the EmissiveColor of its materials, set for this mesh alone).
// The glow is seen by the camera but lights nothing, so the meter and the guards still read dark.
class LightMeter
{
    private Entity@ self;

    [0.4, "Where the light is read, above the thief's centre (m): the chest"] float chestHeight;
    [0.15, "The light (luminance) that fills half the meter"] float halfLight;
    [6.0, "How quickly the meter follows the light"] float follow;
    [0.3, "Below this much light (the meter's 0..1) the glow comes up"] float glowBelow;
    [0.03, "The glow's strength in full dark (its EmissiveColor intensity)"] float glowStrength;
    [7, "How many material slots the figure has"] int slots;

    private float m_shown = 0.0f;
    private Entity@ m_figure;
    private float m_glow = -1.0f; // the glow value last set

    LightMeter(Entity@ entity) { @self = entity; }

    // How lit the thief is, 0..1: luminance / (luminance + `halfLight`). The dark between the lamps
    // reads near empty, moonlight about half, and standing under a lamp near full.
    float level()
    {
        Float3 p = self.worldPosition();
        Float3 light = SceneRender::of(self.scene).lightAt(Float3(p.x, p.y + chestHeight, p.z));
        float luminance = 0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z;
        return luminance / (luminance + halfLight);
    }

    void onStart()
    {
        m_shown = level();
    }

    void onUpdate(double dt)
    {
        float k = follow * float(dt);
        m_shown += (level() - m_shown) * Math::Min(k, 1.0f);
        // The HUD belongs to the game (Lamplight.as pushes it before the level loads). Look it up
        // each frame, because each new level builds a new HUD tree.
        Node@ bar = gui::root().find("hud-light");
        if (bar !is null)
        {
            ProgressBar::of(bar).value = m_shown;
            Label::of(gui::root().find("hud-light-label")).text = m_shown < 0.25f ? "Hidden" : (m_shown < 0.6f ? "Dim" : "Lit");
        }
        glow();
    }

    // Sets the glow from the meter: none above `glowBelow`, rising to `glowStrength` in full dark.
    // The materials are updated only when the value has moved by a step, not every frame.
    private void glow()
    {
        float dark = (glowBelow - m_shown) / glowBelow;
        float g = glowStrength * Math::Clamp(dark, 0.0f, 1.0f);
        if (Math::Abs(g - m_glow) < 0.01f)
        {
            return;
        }
        m_glow = g;
        Entity@ fig = figure();
        if (fig is null)
        {
            return;
        }
        SceneRender@ render = SceneRender::of(self.scene);
        for (int slot = 0; slot < slots; ++slot)
        {
            render.setMaterialFloat4(fig, slot, "EmissiveColor", Float4(0.55f, 0.65f, 0.9f, g));
        }
    }

    // The figure's mesh in the model under the thief (Thief.as finds it the same way).
    private Entity@ figure()
    {
        if (m_figure is null || !m_figure.isValid())
        {
            Entity@ model = self.findChildByName("Thief");
            Entity@ rig = (model !is null && model.isValid()) ? model.findChildByName("ThiefRig") : null;
            @m_figure = (rig !is null && rig.isValid()) ? rig.findChildByName("Thief") : null;
        }
        return m_figure;
    }
}
