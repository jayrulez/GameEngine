// Goal: the level's end. The player reaching the flag emits "GoalReached", once.
// FX/FxConfetti: the celebration at the flag.
Guid kConfetti = Guid("b9828141-6700-49dd-bb97-1947a7ce2d82");

class Goal
{
    private Entity@ self;

    [1.8, "Reach from the flag's base to the player's centre (m)"] float radius;

    private Entity@ m_player;
    private bool m_reached = false;

    Goal(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_player = self.scene.find("Player");
    }

    void onUpdate(double dt)
    {
        if (m_reached || m_player is null || !m_player.isValid())
        {
            return;
        }
        if (Float3::Distance(m_player.worldPosition(), self.worldPosition()) < radius)
        {
            m_reached = true;
            ScenePrefabs::of(self.scene).spawn(kConfetti, self.worldPosition() + Float3(0.0f, 1.0f, 0.0f));
            m_player.send("Celebrate");
            self.scene.events.emit("GoalReached", 0);
        }
    }
}
