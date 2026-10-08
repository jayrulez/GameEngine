// Door - a door on its hinge (this entity, at the hinge edge; the leaf and its kinematic collider
// are children, so they swing with it). Locked, it is picked by holding Interact at it: the thief's
// hand goes to the lock (two-bone IK on the thief, TwoBoneIkComponent), a bar fills on the HUD,
// and letting go starts the pick over. Unlocked, Interact opens it away from the thief and closes
// it again. A guard who comes to it (Knock) opens it, locked or not: guards have the keys.

const float kDoorPi = 3.14159265f;

class Door
{
    private Entity@ self;

    [null, "The thief (it carries the hand's two-bone IK)"] Entity@ thief;
    [true, "Locked at the start"] bool locked;
    [3.0, "Seconds of holding Interact to pick the lock"] float pickSeconds;
    [0.8, "How near the lock the thief must stand (m): within an arm's reach"] float reach;
    [0.0, "The door's yaw when shut (degrees; the leaf runs along +X from the hinge)"] float shutYaw;
    [0.94, "The lock's distance along the leaf from the hinge (m)"] float lockAlong;
    [1.0, "The lock's height (m)"] float lockHeight;
    [100.0, "How far it swings open (degrees)"] float openAngle;
    [160.0, "How fast it swings (degrees a second)"] float swingSpeed;

    private float m_angle = 0.0f;
    private float m_target = 0.0f;
    private float m_pick = 0.0f;
    private bool m_picking = false;
    private bool m_prompting = false;

    Door(Entity@ entity) { @self = entity; }

    void onStart()
    {
        self.setRotationEuler(0.0f, shutYaw, 0.0f);
    }

    void onUpdate(double dt)
    {
        if (thief is null || !thief.isValid())
        {
            return;
        }
        float d = float(dt);
        float yaw = shutYaw * kDoorPi / 180.0f;
        Float3 hinge = self.worldPosition();
        Float3 along = Float3(Math::Cos(yaw), 0.0f, -Math::Sin(yaw)); // the shut leaf, hinge to lock
        Float3 normal = Float3(Math::Sin(yaw), 0.0f, Math::Cos(yaw)); // its face
        Float3 t = thief.worldPosition();
        float side = (t.x - hinge.x) * normal.x + (t.z - hinge.z) * normal.z >= 0.0f ? 1.0f : -1.0f;
        // The lock on the thief's face of the shut leaf.
        Float3 lock = Float3(hinge.x + along.x * lockAlong + normal.x * side * 0.05f, hinge.y + lockHeight,
                             hinge.z + along.z * lockAlong + normal.z * side * 0.05f);
        float dx = lock.x - t.x;
        float dz = lock.z - t.z;
        bool near = dx * dx + dz * dz <= reach * reach && m_angle == 0.0f;

        bool picking = locked && near && Input::isDown("Interact");
        if (picking)
        {
            m_pick += d;
            SceneAnimation::of(self.scene).setIkTarget(thief, lock);
            self.scene.events.emit("Picking", lock);
            if (m_pick >= pickSeconds)
            {
                locked = false;
                picking = false;
            }
        }
        else
        {
            m_pick = 0.0f; // let go: the pick starts over
        }
        if (picking != m_picking)
        {
            m_picking = picking;
            TwoBoneIkComponent::of(thief).weight = picking ? 1.0f : 0.0f;
        }
        if (!locked && Input::wasPressed("Interact") && (near || m_target != 0.0f))
        {
            // Open away from the thief (turned by a, the leaf points along (cos a, 0, -sin a), so
            // away from the thief's side is a = side x the angle); shut again from either side.
            m_target = m_target != 0.0f ? 0.0f : side * openAngle;
        }
        float step = swingSpeed * d;
        float delta = m_target - m_angle;
        m_angle = Math::Abs(delta) <= step ? m_target : m_angle + (delta > 0.0f ? step : -step);
        self.setRotationEuler(0.0f, shutYaw + m_angle, 0.0f);
        prompt(near || (!locked && m_target != 0.0f && dx * dx + dz * dz <= 4.0f * reach * reach));
    }

    // A guard at the door (Guard.as knocks): he has the keys, so a shut door opens away from him,
    // locked or not, and stays open behind him.
    void onKnock(Float3 at)
    {
        if (m_target != 0.0f || m_angle != 0.0f)
        {
            return;
        }
        float yaw = shutYaw * kDoorPi / 180.0f;
        Float3 hinge = self.worldPosition();
        float side = (at.x - hinge.x) * Math::Sin(yaw) + (at.z - hinge.z) * Math::Cos(yaw) >= 0.0f ? 1.0f : -1.0f;
        locked = false;
        m_target = side * openAngle;
    }

    // The HUD's prompt and the pick's progress while in reach; cleared on leaving, only by the door
    // that set it.
    private void prompt(bool show)
    {
        if (show)
        {
            Label@ text = ui::findLabel("hud-prompt");
            if (text !is null && text.isValid())
            {
                text.setText(locked ? (m_picking ? "Picking the lock..." : "Hold Interact: pick the lock")
                                    : (m_target != 0.0f ? "Interact: close the door" : "Interact: open the door"));
            }
            ui::find("hud-prompt-panel").setVisible(true);
            ProgressBar@ bar = ui::findProgressBar("hud-pick");
            if (bar !is null && bar.isValid())
            {
                bar.setVisible(m_picking);
                bar.setValue(m_pick / pickSeconds);
            }
            m_prompting = true;
        }
        else if (m_prompting)
        {
            ui::find("hud-prompt-panel").setVisible(false);
            m_prompting = false;
        }
    }
}
