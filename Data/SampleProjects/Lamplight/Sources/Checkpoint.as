// Checkpoint - a trigger volume the thief walks into: from then on a catch sends him back here
// (a Checkpoint event with where to stand, which Thief.as keeps). Once each.
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
