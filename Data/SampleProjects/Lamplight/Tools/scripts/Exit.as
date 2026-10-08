// Exit - the way out of a level (a trigger volume): the thief walking in says so (Escaped); the
// level script (Level.as) ends the level if he carries the target, or tells him to find it.
class Exit
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;

    Exit(Entity@ entity) { @self = entity; }

    void onTriggerEnter(Entity@ other)
    {
        if (thief is null || other is null || !other.isValid() || other.id() != thief.id())
        {
            return;
        }
        self.scene.events.emit("Escaped", self.worldPosition());
    }
}
