// Gallery: the camera's behaviour, with the lesson list beside the view, a free flight through the hall,
// and a glide to a lesson when its row is clicked.
//
// Fly with WASD or the left stick, Q and E (or the shoulders) to sink and rise, Shift (or pressing
// the stick) to go faster; look with the mouse while its right button is held, or the right stick.
// H (or Y) hides the list and the caption. A row glides the camera to that lesson's viewpoint
// ("ViewNN"), looking at its exhibit ("LessonNN"), and shows what the lesson teaches and its file in
// the caption at the foot of the view; flying by hand again leaves both alone.
//
// GENERATED from Tools/scripts/Gallery.as.in by Tools/ui.py, which fills in the lessons' handlers
// from Tools/lessons.py: edit the template and the table, not the generated copy.
const float kPi = 3.14159265f;
const string kGalleryDoc = "{{{GUI:Gallery}}}"; // the lesson list's document, by its asset id

// A GUI event's handler (a row's click).
funcdef bool EventHandler(GuiEvent e);

// Smooth start and stop for the glide: 0 to 1 over t in 0 to 1.
float SmoothGlide(float t)
{
    t = Math::Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class Gallery
{
    private Entity@ self;

    [4.0, "Flying speed (m/s)"] float speed;
    [3.0, "How much faster with Fast held"] float fastFactor;
    [0.15, "Mouse look (degrees per pixel moved)"] float mouseLook;
    [110.0, "Stick look (degrees per second at full tilt)"] float stickLook;
    [1.2, "How long a glide to a lesson takes (s)"] float glideTime;
    [0.0, "Starting yaw (degrees; 0 looks down -Z)"] float startYaw;
    [-8.0, "Starting pitch (degrees; up is positive)"] float startPitch;
    [12.0, "How far a glide turns left of its lesson while the list shows (degrees), so the lesson sits in the free view"] float listYaw;

    private float m_yaw = 0.0f;
    private float m_pitch = 0.0f;
    private bool m_gliding = false;
    private float m_glideT = 0.0f;
    private Float3 m_fromPos = Float3(0.0f, 0.0f, 0.0f);
    private Float3 m_toPos = Float3(0.0f, 0.0f, 0.0f);
    private float m_fromYaw = 0.0f;
    private float m_toYaw = 0.0f;
    private float m_fromPitch = 0.0f;
    private float m_toPitch = 0.0f;
    private bool m_panelShown = true;
    private bool m_captioned = false;

    Gallery(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_yaw = startYaw;
        m_pitch = startPitch;
        // The lesson list on the GUI's screen stack, over the hall (its document says it blocks none
        // of the hall's input).
        ScreenStack::of(gui::root().find("screens")).push(kGalleryDoc);
        gui::root().find("gallery-caption").visible = false; // until a lesson is picked
        gui::root().find("lesson-1").on("click", EventHandler(this.onLesson1));
        gui::root().find("lesson-2").on("click", EventHandler(this.onLesson2));
        gui::root().find("lesson-3").on("click", EventHandler(this.onLesson3));
        gui::root().find("lesson-4").on("click", EventHandler(this.onLesson4));
        gui::root().find("lesson-5").on("click", EventHandler(this.onLesson5));
        gui::root().find("lesson-6").on("click", EventHandler(this.onLesson6));
        gui::root().find("lesson-7").on("click", EventHandler(this.onLesson7));
        gui::root().find("lesson-8").on("click", EventHandler(this.onLesson8));
        gui::root().find("lesson-9").on("click", EventHandler(this.onLesson9));
        gui::root().find("lesson-10").on("click", EventHandler(this.onLesson10));
        gui::root().find("lesson-11").on("click", EventHandler(this.onLesson11));
        place(self.worldPosition());
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (Input::wasPressed("TogglePanel"))
        {
            m_panelShown = !m_panelShown;
            gui::root().find("gallery-panel").visible = m_panelShown;
            gui::root().find("gallery-caption").visible = m_panelShown && m_captioned;
        }

        // Looking: the mouse while its right button is held, or the right stick.
        Float2 look = Float2(0.0f, 0.0f);
        if (Input::isDown("LookHold"))
        {
            Float2 m = Input::value2D("Look");
            look = Float2(-m.x * mouseLook, -m.y * mouseLook);
        }
        Float2 stick = Input::value2D("LookStick");
        look = Float2(look.x - stick.x * stickLook * d, look.y + stick.y * stickLook * d);

        // Flying: along where the camera looks, and straight up or down.
        Float2 move = Input::value2D("Move");
        float rise = (Input::isDown("Rise") ? 1.0f : 0.0f) - (Input::isDown("Sink") ? 1.0f : 0.0f);
        bool manual = Math::Abs(look.x) + Math::Abs(look.y) + Math::Abs(move.x) + Math::Abs(move.y) +
                          Math::Abs(rise) > 0.0001f;

        if (m_gliding && !manual)
        {
            m_glideT += d / glideTime;
            float k = SmoothGlide(m_glideT);
            Float3 at = Float3(m_fromPos.x + (m_toPos.x - m_fromPos.x) * k,
                               m_fromPos.y + (m_toPos.y - m_fromPos.y) * k,
                               m_fromPos.z + (m_toPos.z - m_fromPos.z) * k);
            m_yaw = m_fromYaw + (m_toYaw - m_fromYaw) * k;
            m_pitch = m_fromPitch + (m_toPitch - m_fromPitch) * k;
            m_gliding = m_glideT < 1.0f;
            place(at);
            return;
        }
        m_gliding = false; // a hand on the controls takes over from a glide

        m_yaw += look.x;
        m_pitch = Math::Clamp(m_pitch + look.y, -85.0f, 85.0f);
        float fast = Input::isDown("Fast") ? fastFactor : 1.0f;
        float yaw = Math::DegreesToRadians(m_yaw);
        float pitch = Math::DegreesToRadians(m_pitch);
        // Forward from yaw and pitch (yaw 0 looks down -Z); right is level.
        Float3 forward = Float3(-Math::Sin(yaw) * Math::Cos(pitch), Math::Sin(pitch),
                                -Math::Cos(yaw) * Math::Cos(pitch));
        Float3 right = Float3(Math::Cos(yaw), 0.0f, -Math::Sin(yaw));
        float step = speed * fast * d;
        Float3 p = self.worldPosition();
        place(Float3(p.x + (forward.x * move.y + right.x * move.x) * step,
                     p.y + (forward.y * move.y + rise) * step,
                     p.z + (forward.z * move.y + right.z * move.x) * step));
    }

    // Glides to lesson `n`: its viewpoint ("ViewNN"), looking at its exhibit ("LessonNN").
    private void showLesson(int n, string title, string summary, string file)
    {
        string id = (n < 10 ? "0" : "") + n;
        Entity@ view = self.scene.find("View" + id);
        Entity@ exhibit = self.scene.find("Lesson" + id);
        Label::of(gui::root().find("gallery-title")).text = n + ". " + title;
        Label::of(gui::root().find("gallery-about")).text = summary;
        Label::of(gui::root().find("gallery-file")).text = file;
        m_captioned = true;
        gui::root().find("gallery-caption").visible = m_panelShown;
        if (view is null || !view.isValid() || exhibit is null || !exhibit.isValid())
        {
            return;
        }
        m_fromPos = self.worldPosition();
        m_toPos = view.worldPosition();
        Float3 target = exhibit.worldPosition();
        Float3 dir = Float3(target.x - m_toPos.x, target.y - m_toPos.y, target.z - m_toPos.z);
        float len = Math::Sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 0.0001f)
        {
            return;
        }
        m_fromYaw = m_yaw;
        m_fromPitch = m_pitch;
        // Turned a little left while the list covers the left of the screen, so the lesson sits in
        // the middle of what is left.
        m_toYaw = Math::RadiansToDegrees(Math::Atan2(-dir.x, -dir.z)) + (m_panelShown ? listYaw : 0.0f);
        m_toPitch = Math::RadiansToDegrees(Math::Asin(dir.y / len));
        // The short way round.
        while (m_toYaw - m_fromYaw > 180.0f) m_toYaw -= 360.0f;
        while (m_toYaw - m_fromYaw < -180.0f) m_toYaw += 360.0f;
        m_glideT = 0.0f;
        m_gliding = true;
    }

    private void place(Float3 at)
    {
        self.setPosition(at);
        self.setRotationEuler(m_pitch, m_yaw, 0.0f);
    }

    private bool onLesson1(GuiEvent e) { showLesson(1, "Unlit Colour", "The smallest shader: a properties block and Surface().", "Sources/01_UnlitColour.hlsl"); return true; }
    private bool onLesson2(GuiEvent e) { showLesson(2, "Textured", "A texture and its sampler, the uv, the vertex colour; lit by the scene.", "Sources/02_Textured.hlsl"); return true; }
    private bool onLesson3(GuiEvent e) { showLesson(3, "Lava", "Time() makes it move; emission makes it glow.", "Sources/03_Lava.hlsl"); return true; }
    private bool onLesson4(GuiEvent e) { showLesson(4, "Hologram", "viewDir makes a fresnel rim, screenPos scanlines; alpha on a blended material.", "Sources/04_Hologram.hlsl"); return true; }
    private bool onLesson5(GuiEvent e) { showLesson(5, "Triplanar Rock", "Mapped by world position, no uv; a normal map turned into tangent space.", "Sources/05_TriplanarRock.hlsl"); return true; }
    private bool onLesson6(GuiEvent e) { showLesson(6, "Dissolve", "alpha on a masked material cuts holes, in the shadow too; a burning edge.", "Sources/06_Dissolve.hlsl"); return true; }
    private bool onLesson7(GuiEvent e) { showLesson(7, "Waves", "Vertex() moves the vertices into waves; the shadow and the light follow.", "Sources/07_Waves.hlsl"); return true; }
    private bool onLesson8(GuiEvent e) { showLesson(8, "Wind", "Vertex() by localPosition: the hoist stays on the pole, the fly end flaps.", "Sources/08_Wind.hlsl"); return true; }
    private bool onLesson9(GuiEvent e) { showLesson(9, "Built-in PBR", "The engine's own material written as a surface: the reference to start from.", "Sources/09_BuiltInPbr.hlsl"); return true; }
    private bool onLesson10(GuiEvent e) { showLesson(10, "Toon", "Both stages written out: the light in bands, a shadow colour, a rim.", "Sources/10_Toon.hlsl"); return true; }
    private bool onLesson11(GuiEvent e) { showLesson(11, "Custom Lighting", "Walking the lights itself (LightsAt, LightAt, ShadowOf): half-Lambert and Blinn-Phong.", "Sources/11_CustomLighting.hlsl"); return true; }
}
