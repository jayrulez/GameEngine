// Loot - something to take: walked over, it is taken (a Loot event with its value, a chime, and it
// is gone). The level's target is taken the same way and says so instead (Taken), so the level
// script (Level.as) knows the thief may now leave by the exit.
class Loot
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;
    [10, "What it is worth"] int value;
    [false, "The level's target: carried to the exit, it ends the level"] bool target;
    [0.9, "How near the thief takes it (m)"] float reach;
    [0.6, "How fast it turns, to catch the eye (turns a second)"] float spin;

    private float m_turn = 0.0f;

    Loot(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        m_turn += spin * 360.0f * float(dt);
        self.setRotationEuler(0.0f, m_turn, 0.0f);
        if (thief is null || !thief.isValid())
        {
            return;
        }
        Float3 a = self.worldPosition();
        Float3 b = thief.worldPosition();
        float dx = a.x - b.x;
        float dz = a.z - b.z;
        if (dx * dx + dz * dz > reach * reach)
        {
            return;
        }
        Audio::playOneShot3DPath(target ? "Audio/Taken" : "Audio/Coins", a.x, a.y, a.z);
        if (target)
        {
            self.scene.events.emit("Taken", value);
        }
        else
        {
            self.scene.events.emit("Loot", value);
        }
        self.destroy();
    }
}
