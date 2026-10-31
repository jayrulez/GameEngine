// Pedestrian: wanders the block on the navmesh. It picks a spot around the ring road (on a verge
// or across it), walks there, then picks another. The Level sets the walking speed
// ("PedestrianSpeed") and says where the ring road runs ("BlockRing"), which sets the band the
// walks stay in.
//
// The figure (Models/Town/PedestrianModel) turns toward where it walks, easing round rather than
// snapping, and its Walk clip (two 0.65 m steps a second) plays at the pace it walks.


class Pedestrian
{
    private Entity@ self;

    [1.6, "Walking speed until the Level sets one (m/s)"] float speed;
    [18.0, "Nearest the middle a walk may lead (m)"] float inner;
    [31.0, "Farthest from the middle a walk may lead (m)"] float outer;
    ["asset:AnimationClip", "The figure's walk clip"] Guid@ walkClip;
    [1.3, "Metres the walk clip covers at speed 1"] float walkMetres;

    private bool m_walking = false;
    private Entity@ m_figure;
    private float m_yaw = 0.0f; // radians; 0 faces +Z

    Pedestrian(Entity@ entity) { @self = entity; }

    void onStart()
    {
        NavAgentComponent::of(self).setSpeed(speed);
        pickTarget();
        @m_figure = self.findChildByName("PedestrianModel");
        if (m_figure !is null && m_figure.isValid() && walkClip !is null && !walkClip.IsNil())
        {
            SceneAnimation anim = SceneAnimation::of(self.scene);
            anim.setClip(m_figure, walkClip);
            anim.play(m_figure);
        }
    }

    void onPedestrianSpeed(float s)
    {
        speed = s;
        NavAgentComponent::of(self).setSpeed(speed);
    }

    void onBlockRing(float ring)
    {
        inner = ring - 6.0f;
        outer = ring + 7.0f;
        pickTarget();
    }

    void onUpdate(double dt)
    {
        NavAgentComponent@ agent = NavAgentComponent::of(self);
        if (m_walking && agent.finished())
        {
            pickTarget();
        }
        // Face the way the walk goes (turning the short way round, a few times a second) and step
        // at its pace; when not moving, the figure stands still.
        float vx = agent.velocityX();
        float vz = agent.velocityZ();
        float pace = Math::Sqrt(vx * vx + vz * vz);
        if (pace > 0.05f)
        {
            float turn = Math::Atan2(vx, vz) - m_yaw;
            while (turn > 3.14159f) { turn -= 6.28318f; }
            while (turn < -3.14159f) { turn += 6.28318f; }
            float ease = 8.0f * float(dt);
            m_yaw += turn * ((ease < 1.0f) ? ease : 1.0f);
            self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
        }
        if (m_figure !is null && m_figure.isValid())
        {
            SkeletalAnimationComponent::of(m_figure).speed = pace / walkMetres;
        }
    }

    // A point in the band around the ring road, on one of its four sides.
    private void pickTarget()
    {
        float along = Random::range(-outer, outer);
        float across = Random::range(inner, outer);
        if (Random::boolean())
        {
            across = -across;
        }
        float x = along;
        float z = across;
        if (Random::boolean())
        {
            x = across;
            z = along;
        }
        NavAgentComponent::of(self).navigate(x, 0.0f, z);
        m_walking = true;
    }
}
