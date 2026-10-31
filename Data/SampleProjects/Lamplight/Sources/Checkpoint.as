// Checkpoint: a trigger volume. When the thief walks into it, it emits a Checkpoint event with the
// position to stand at; Thief.as keeps it, and from then on a catch sends him back here. Each
// checkpoint fires once.
class Checkpoint
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;

    private bool m_reached = false;

    Checkpoint(Entity@ entity) { @self = entity; }

    void onTriggerEnter(Entity@ other)
    {
        if (m_reached || thief is null || other is null || !other.isValid() || other.id() != thief.id())
        {
            return;
        }
        m_reached = true;
        self.scene.events.emit("Checkpoint", self.worldPosition());
    }
}
