// Enemy: a patroller that walks back and forth along X between two points, facing the way it
// walks. Touching it hurts the player, unless the player comes down on it from the air, which
// defeats it ("EnemyDefeated") and bounces the player off it. The script sits on the enemy's
// entity, whose first child is its model.
//
// Touching is a box, not a sphere: within `radius` across the ground and with the player's centre
// no higher than `touchHeight` above the enemy's origin (and not below it). Only a player in the
// air and on the way down stomps; one on the ground, walking or standing into the enemy, is hurt,
// however the enemy's origin sits against the ground the player walks on.
//
// A flier (the bee) is the same enemy in the air: it hovers, bobbing `hoverHeight` up and down,
// and may patrol across the path (along Z) rather than along it. It is stomped and hurts the same
// way, measured from where it flies.
// FX/FxStompStars: the burst where an enemy was stomped.
Guid kStompStars = Guid("ab197a34-58e6-4f23-add1-0f2a90f00ca6");

// Audio/impactSoft_heavy_000 (Kenney Impact Sounds, CC0): an enemy stomped flat.
Guid kSquashSound = Guid("134e50c9-8003-4065-a81f-69cf5e0d9dc7");

class Enemy
{
    private Entity@ self;

    [3.0, "Patrol half-width along X (m)"] float patrolDistance;
    [2.0, "Walk speed (m/s)"] float speed;
    [1.1, "Touch reach across the ground, enemy to player (m)"] float radius;
    [1.8, "Touching while the player's centre is at most this far above the enemy's origin (m)"] float touchHeight;
    [1.0, "Falling at least this fast counts as coming down on the enemy (m/s)"] float stompSpeed;
    ["asset:AnimationClip", "The model's walk clip; none keeps its idle"] Guid@ walkClip;
    [0.0, "Hover: bob this far up and down as it patrols (m); 0 walks"] float hoverHeight;
    [2.0, "Hover bob rate (rad/s)"] float hoverSpeed;
    [false, "Patrol along Z (across the path) rather than X"] bool patrolAlongZ;

    private Entity@ m_player;
    private Float3 m_home = Float3(0.0f, 0.0f, 0.0f);
    private float m_offset = 0.0f;
    private float m_direction = 1.0f;
    private float m_lastPlayerY = 0.0f;
    private float m_time = 0.0f;

    Enemy(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_player = self.scene.find("Player");
        m_home = self.position();
        Entity@ model = self.firstChild();
        if (model.isValid() && walkClip !is null && !walkClip.IsNil())
        {
            SceneAnimation animation = SceneAnimation::of(self.scene);
            animation.setClip(model, walkClip);
            animation.play(model);
        }
        if (m_player !is null && m_player.isValid())
        {
            m_lastPlayerY = m_player.worldPosition().y;
        }
    }

    void onUpdate(double delta)
    {
        float dt = float(delta);
        m_offset += m_direction * speed * dt;
        if (m_offset > patrolDistance)
        {
            m_offset = patrolDistance;
            m_direction = -1.0f;
        }
        else if (m_offset < -patrolDistance)
        {
            m_offset = -patrolDistance;
            m_direction = 1.0f;
        }
        m_time += dt;
        float bob = Math::Sin(m_time * hoverSpeed) * hoverHeight;
        if (patrolAlongZ)
        {
            self.setPosition(Float3(m_home.x, m_home.y + bob, m_home.z + m_offset));
            self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f),
                                                       (m_direction > 0.0f) ? 0.0f : 3.14159f));
        }
        else
        {
            self.setPosition(Float3(m_home.x + m_offset, m_home.y + bob, m_home.z));
            self.setRotation(Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f),
                                                       (m_direction > 0.0f) ? 1.5708f : -1.5708f));
        }

        if (m_player is null || !m_player.isValid())
        {
            return;
        }
        Float3 at = m_player.worldPosition();
        float fallSpeed = (dt > 0.0f) ? (m_lastPlayerY - at.y) / dt : 0.0f;
        m_lastPlayerY = at.y;
        Float3 here = self.worldPosition();
        float dx = at.x - here.x;
        float dz = at.z - here.z;
        float above = at.y - here.y;
        if ((dx * dx + dz * dz >= radius * radius) || (above > touchHeight) || (above < 0.0f))
        {
            return;
        }
        bool airborne = !CharacterComponent::of(m_player).grounded();
        if (airborne && (fallSpeed >= stompSpeed))
        {
            self.scene.events.emit("EnemyDefeated", 1);
            m_player.send("Bounce");
            ScenePrefabs::of(self.scene).spawn(kStompStars, here + Float3(0.0f, 0.4f, 0.0f));
            Audio::playOneShot(kSquashSound);
            self.destroy();
        }
        else
        {
            m_player.send("Hurt");
        }
    }
}
