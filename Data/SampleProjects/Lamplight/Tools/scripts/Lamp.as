// Lamp - an oil lamp the thief can put out and light again: in reach, the HUD says which, and the
// Interact control does it. Out, its light is off and its flame and chimney stop glowing (their
// materials swapped for dark ones), so the room goes as dark as the renderer and the light meter
// say. Lit at the start unless `lit` is off.
class Lamp
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;
    [null, "The lamp's light"] Entity@ light;
    [1.6, "How near the thief must stand to reach it (m)"] float reach;
    [true, "Lit at the start"] bool lit;
    [1, "The material slot of the flame"] int flameSlot;
    [2, "The material slot of the chimney"] int chimneySlot;
    ["asset:Material", "The flame burning"] Guid@ flameLit;
    ["asset:Material", "The flame out"] Guid@ flameOut;
    ["asset:Material", "The chimney glowing"] Guid@ chimneyLit;
    ["asset:Material", "The chimney dark"] Guid@ chimneyOut;

    private bool m_prompting = false;

    Lamp(Entity@ entity) { @self = entity; }

    void onStart()
    {
        apply();
    }

    void onUpdate(double dt)
    {
        if (thief is null || !thief.isValid())
        {
            return;
        }
        Float3 a = self.worldPosition();
        Float3 b = thief.worldPosition();
        float dx = a.x - b.x;
        float dz = a.z - b.z;
        bool near = dx * dx + dz * dz <= reach * reach;
        if (near && Input::wasPressed("Interact"))
        {
            lit = !lit;
            apply();
        }
        prompt(near);
    }

    private void apply()
    {
        if (light !is null && light.isValid())
        {
            LightComponent::of(light).enabled = lit;
        }
        SceneRender render = SceneRender::of(self.scene);
        render.setMaterial(self, lit ? flameLit : flameOut, flameSlot);
        render.setMaterial(self, lit ? chimneyLit : chimneyOut, chimneySlot);
    }

    // The HUD's prompt while in reach; cleared on leaving, only by the lamp that set it.
    private void prompt(bool near)
    {
        if (near)
        {
            Label@ text = ui::findLabel("hud-prompt");
            if (text !is null && text.isValid())
            {
                text.setText(lit ? "Interact: put out the lamp" : "Interact: light the lamp");
            }
            ui::find("hud-prompt-panel").setVisible(true);
            m_prompting = true;
        }
        else if (m_prompting)
        {
            ui::find("hud-prompt-panel").setVisible(false);
            m_prompting = false;
        }
    }
}
