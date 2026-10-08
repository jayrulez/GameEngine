// Footsteps - the thief's steps: one each stride (the clips' strides, blender/thief.py), the sound
// of what the foot lands on (the physical material under it, RayCastHit.material) and a Noise for
// whoever listens (the guards, P2): a Float4 of where it was and how loud, 0 to 1. Sneaking is
// silent; a run is louder than a walk, and gravel louder than boards, grass softer.
class Footsteps
{
    private Entity@ self;

    ["asset:PhysicalMaterial", "Boards (Surfaces/Wood)"] Guid@ wood;
    ["asset:PhysicalMaterial", "Flags (Surfaces/Stone)"] Guid@ stone;
    ["asset:PhysicalMaterial", "Surfaces/Gravel"] Guid@ gravel;
    ["asset:PhysicalMaterial", "Surfaces/Grass"] Guid@ grass;
    [0.6, "Metres a step, sneaking"] float sneakStride;
    [0.8, "Metres a step, walking"] float walkStride;
    [1.35, "Metres a step, running"] float runStride;
    [3.0, "Faster than this (m/s) is a run"] float runSpeed;
    [0.4, "How loud a walking step is (0..1, on boards)"] float walkNoise;
    [1.0, "How loud a running step is (0..1, on boards)"] float runNoise;
    [0.9, "From the thief's centre down to the floor (m)"] float footDrop;

    private float m_travel = 0.0f;
    private int m_take = 0;
    private string m_surface = ""; // the last step's, and how many: what a playtest reads
    private int m_steps = 0;
    private int m_heard = 0; // steps whose sound started

    Footsteps(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        Float3 v = CharacterComponent::of(self).velocity;
        float speed = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (speed < 0.2f)
        {
            m_travel = 0.0f; // stood still: the next step is a whole stride away
            return;
        }
        bool sneaking = Input::isDown("Sneak");
        bool running = !sneaking && speed > runSpeed;
        float stride = sneaking ? sneakStride : (running ? runStride : walkStride);
        m_travel += speed * float(dt);
        if (m_travel < stride)
        {
            return;
        }
        m_travel -= stride;
        if (!sneaking)
        {
            step(running ? runNoise : walkNoise);
        }
    }

    // A step: what is underfoot, its sound at the foot, and the noise it makes.
    private void step(float noise)
    {
        Float3 at = self.worldPosition();
        RayCastHit@ hit = ScenePhysics::of(self.scene).rayCast(at, Float3(0.0f, -1.0f, 0.0f), footDrop + 0.3f);
        if (hit is null || !hit.hit)
        {
            return; // in the air
        }
        Guid@ under = hit.material();
        string surface = "Wood";
        float loudness = 1.0f;
        if (stone !is null && under == stone) { surface = "Stone"; loudness = 0.9f; }
        else if (gravel !is null && under == gravel) { surface = "Gravel"; loudness = 1.4f; }
        else if (grass !is null && under == grass) { surface = "Grass"; loudness = 0.4f; }
        m_take = m_take % 3 + 1;
        m_surface = surface;
        m_steps++;
        Float3 foot = hit.position;
        if (Audio::playOneShot3DPath("Audio/Step" + surface + m_take, foot.x, foot.y, foot.z))
        {
            m_heard++;
        }
        float level = noise * loudness;
        self.scene.events.emit("Noise", Float4(foot.x, foot.y, foot.z, level > 1.0f ? 1.0f : level));
    }
}
