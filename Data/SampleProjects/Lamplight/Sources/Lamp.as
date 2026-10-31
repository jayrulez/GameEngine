// Lamp: an oil lamp the thief can put out and light again. In reach, the HUD shows which action
// Interact will do. When the lamp is out its light is off and its flame and chimney stop glowing
// (their materials are swapped for dark ones), so the room really goes dark for both the renderer
// and the light meter. It starts lit unless `lit` is off.
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

    // Shows the HUD prompt while the thief is in reach. On leaving, only the lamp that set the
    // prompt clears it.
    private void prompt(bool near)
    {
        if (near)
        {
            Node@ panel = gui::root().find("hud-prompt-panel");
            if (panel is null)
            {
                return; // the HUD is not up
            }
            Label::of(gui::root().find("hud-prompt")).text = lit ? "Interact: put out the lamp" : "Interact: light the lamp";
            panel.visible = true;
            gui::root().find("hud-pick").visible = false; // the pick bar belongs to doors
            m_prompting = true;
        }
        else if (m_prompting)
        {
            Node@ panel = gui::root().find("hud-prompt-panel");
            if (panel !is null)
            {
                panel.visible = false;
            }
            m_prompting = false;
        }
    }
}
