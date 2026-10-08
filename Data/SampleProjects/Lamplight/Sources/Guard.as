// Guard - walks a round with a lantern, and sees and hears the thief. On the guard's entity, which
// carries a navigation agent and the model (its graph: Speed, Searching); the lantern's spot light
// is a child, and what it lights is what he sees.
//
// - Patrol: the route's points (the children of `route`) in turn, round and round.
// - Seeing: the thief inside the lantern's cone and range, nothing between his eye and the thief's
//   chest (a ray that ignores the guards' own group), fills `awareness` by how lit the thief is
//   (SceneRender.lightAt, his own lantern included) and faster the nearer; unseen, it drains.
// - Within `touch` he notices the thief whatever the light (bumped into).
// - Suspicious at half: he stops and turns to look. Chase when full: he runs at the thief and
//   catches him within `catchDistance` (a Caught event: the thief to his checkpoint, the guards to
//   their rounds). Lose him, or hear a Noise within its reach: Search, go there and look round,
//   then back to the round.

const int kPatrol = 0;
const int kSuspicious = 1;
const int kChase = 2;
const int kSearch = 3;

class Guard
{
    private Entity@ self;

    [null, "The thief"] Entity@ thief;
    [null, "The round: its children are the points walked in turn"] Entity@ route;
    [null, "The lantern's spot light (a child): its cone and range are what he sees"] Entity@ lantern;
    [null, "The meter over his head (a UI billboard: a mark and a bar)"] Entity@ meter;
    [1.2, "Walking speed (m/s): the Walk clip's"] float walkSpeed;
    [4.0, "Running speed (m/s): the Run clip's"] float runSpeed;
    [1.6, "Eye height (m)"] float eyeHeight;
    [1.2, "Seconds to see a fully lit thief at arm's length"] float noticeSeconds;
    [0.15, "The light (luminance) that counts as half lit"] float halfLight;
    [0.4, "How fast awareness drains, unseen (per second)"] float forget;
    [1.0, "Catches the thief within this (m)"] float catchDistance;
    [14.0, "How far away a noise of loudness 1 is heard (m)"] float hearing;
    [4.0, "Seconds spent looking round at a searched place"] float searchSeconds;
    [1.2, "Within this (m) he feels the thief, cone or no cone: bumped into, he notices"] float touch;
    [3.0, "Seconds after a catch the guards leave the thief be (he is back at his checkpoint)"] float grace;
    [2, "The guards' collision group: their own rays look past it"] int group;

    private int m_state = kPatrol;
    private float awareness = 0.0f;
    private Entity@ m_point;
    private Float3 m_spot = Float3(0.0f, 0.0f, 0.0f);
    private float m_timer = 0.0f;
    private Float3 m_home = Float3(0.0f, 0.0f, 0.0f);
    private Float3 m_last = Float3(0.0f, 0.0f, 0.0f);
    private float m_yaw = 0.0f;
    private Entity@ m_figure;
    private float m_calm = 0.0f; // after a catch: not looking for the thief yet
    private Float3 m_heard = Float3(0.0f, 0.0f, 0.0f); // the last noise he went to look at

    Guard(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_home = self.worldPosition();
        m_last = m_home;
        patrol();
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (d <= 0.0f)
        {
            return;
        }
        see(d);
        NavAgentComponent@ agent = NavAgentComponent::of(self);
        if (m_state == kPatrol)
        {
            if (awareness >= 0.5f)
            {
                m_state = kSuspicious;
                agent.stop();
            }
            else if (agent.finished())
            {
                next();
            }
        }
        else if (m_state == kSuspicious)
        {
            face(thief.worldPosition(), d);
            if (awareness >= 1.0f)
            {
                m_state = kChase;
            }
            else if (awareness < 0.25f)
            {
                search(m_spot);
            }
        }
        else if (m_state == kChase)
        {
            Float3 t = thief.worldPosition();
            agent.navigateAt(t.x, t.y, t.z, runSpeed, 0.5f);
            if (flatDistance(self.worldPosition(), t) <= catchDistance)
            {
                // Caught: once (stand down now; the event reaches everyone next frame).
                self.scene.events.emit("Caught", self.worldPosition());
                standDown();
            }
            else if (awareness < 0.5f)
            {
                search(m_spot); // lost him: where he was last seen
            }
        }
        else if (m_state == kSearch)
        {
            if (awareness >= 0.5f)
            {
                m_state = kSuspicious;
                agent.stop();
            }
            else if (agent.finished())
            {
                m_timer -= d;
                if (m_timer <= 0.0f)
                {
                    patrol();
                }
            }
        }
        animate(d);
        show();
    }

    // The meter over his head: hidden while he has seen nothing, then the bar fills with his
    // awareness under a "?", and a "!" once he is after the thief.
    private void show()
    {
        if (meter is null || !meter.isValid())
        {
            return;
        }
        UIBillboardComponent@ board = UIBillboardComponent::of(meter);
        bool shown = awareness > 0.02f || m_state == kChase;
        board.visible = shown;
        if (!shown)
        {
            return;
        }
        ViewGroup@ panel = board.root();
        panel.findProgressBar("meter").setValue(m_state == kChase ? 1.0 : double(awareness));
        panel.findLabel("mark").setText(m_state == kChase ? "!" : "?");
    }

    // A noise: heard within its reach, it is searched (unless he is already after the thief).
    void onNoise(Float4 n)
    {
        if (m_state == kChase || m_state == kSuspicious)
        {
            return;
        }
        if (flatDistance(self.worldPosition(), Float3(n.x, n.y, n.z)) <= hearing * n.w)
        {
            m_heard = Float3(n.x, n.y, n.z);
            search(m_heard);
        }
    }

    // The thief caught (by any guard): back to the round, forgetting him for a while.
    void onCaught(Float3 at)
    {
        standDown();
    }

    private void standDown()
    {
        awareness = 0.0f;
        m_calm = grace;
        NavAgentComponent::of(self).stop();
        patrol();
    }

    // How much of the thief this guard sees this frame, into awareness.
    private void see(float d)
    {
        float rate = -forget;
        if (m_calm > 0.0f)
        {
            m_calm -= d;
            awareness = 0.0f;
            return;
        }
        if (thief !is null && thief.isValid() && lantern !is null && lantern.isValid())
        {
            Float3 eye = self.worldPosition();
            eye = Float3(eye.x, eye.y + eyeHeight, eye.z);
            Float3 t = thief.worldPosition();
            Float3 chest = Float3(t.x, t.y + 0.4f, t.z);
            Float3 to = Float3(chest.x - eye.x, chest.y - eye.y, chest.z - eye.z);
            float dist = Math::Sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
            LightComponent@ cone = LightComponent::of(lantern);
            Float3 ahead = Quaternion::RotateVector(lantern.worldRotation(), Float3(0.0f, 0.0f, -1.0f));
            float cosAngle = dist > 0.001f ? (to.x * ahead.x + to.y * ahead.y + to.z * ahead.z) / dist : 1.0f;
            uint mask = 0xFFFFFFFF & ~(uint(1) << uint(group));
            if (dist < cone.range && cosAngle >= Math::Cos(cone.outerAngle))
            {
                RayCastHit@ hit = ScenePhysics::of(self.scene).rayCast(eye, Float3(to.x / dist, to.y / dist, to.z / dist),
                                                                       dist, mask);
                if (hit is null || !hit.hit)
                {
                    Float3 light = SceneRender::of(self.scene).lightAt(chest, mask);
                    float lum = 0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z;
                    float lit = lum / (lum + halfLight);
                    float near = 1.0f - dist / cone.range;
                    rate = lit * (0.4f + 0.6f * near) / noticeSeconds;
                    m_spot = t;
                }
            }
        }
        if (thief !is null && thief.isValid() && flatDistance(self.worldPosition(), thief.worldPosition()) <= touch)
        {
            rate = 2.0f; // too close to miss
            m_spot = thief.worldPosition();
        }
        awareness += rate * d;
        awareness = awareness < 0.0f ? 0.0f : (awareness > 1.0f ? 1.0f : awareness);
    }

    private void patrol()
    {
        m_state = kPatrol;
        if (m_point is null || !m_point.isValid())
        {
            @m_point = (route !is null && route.isValid()) ? route.firstChild() : null;
        }
        go(m_point, walkSpeed);
    }

    private void next()
    {
        if (m_point !is null && m_point.isValid())
        {
            @m_point = m_point.nextSibling();
        }
        if ((m_point is null || !m_point.isValid()) && route !is null && route.isValid())
        {
            @m_point = route.firstChild();
        }
        go(m_point, walkSpeed);
    }

    private void go(Entity@ point, float speed)
    {
        if (point is null || !point.isValid())
        {
            return;
        }
        Float3 p = point.worldPosition();
        NavAgentComponent::of(self).navigateAt(p.x, p.y, p.z, speed, 0.3f);
    }

    private void search(Float3 at)
    {
        m_state = kSearch;
        m_timer = searchSeconds;
        NavAgentComponent::of(self).navigateAt(at.x, at.y, at.z, walkSpeed, 0.6f);
    }

    // The body faces where it goes, or (suspicious) the thief; the graph follows the speed.
    private void animate(float d)
    {
        Float3 p = self.worldPosition();
        float vx = (p.x - m_last.x) / d;
        float vz = (p.z - m_last.z) / d;
        m_last = p;
        float speed = Math::Sqrt(vx * vx + vz * vz);
        if (speed > 0.2f && m_state != kSuspicious)
        {
            face(Float3(p.x + vx, p.y, p.z + vz), d);
        }
        Entity@ fig = figure();
        if (fig !is null && fig.isValid())
        {
            SceneAnimation anim = SceneAnimation::of(self.scene);
            anim.setFloat(fig, "Speed", speed);
            anim.setBool(fig, "Searching", (m_state == kSearch && speed < 0.2f) || m_state == kSuspicious);
        }
    }

    private void face(Float3 at, float d)
    {
        Float3 p = self.worldPosition();
        float want = Math::Atan2(at.x - p.x, at.z - p.z); // the model faces +Z
        float delta = want - m_yaw;
        while (delta > 3.14159265f) delta -= 6.2831853f;
        while (delta < -3.14159265f) delta += 6.2831853f;
        float k = 8.0f * d;
        m_yaw += delta * (k > 1.0f ? 1.0f : k);
        self.setRotationEuler(0.0f, m_yaw * 57.2957795f, 0.0f);
    }

    private float flatDistance(Float3 a, Float3 b)
    {
        float dx = a.x - b.x;
        float dz = a.z - b.z;
        return Math::Sqrt(dx * dx + dz * dz);
    }

    // The model's skinned mesh, which carries the graph: Guard (the instance) / GuardRig / Guard.
    private Entity@ figure()
    {
        if (m_figure is null || !m_figure.isValid())
        {
            Entity@ model = self.findChildByName("Guard");
            Entity@ rig = (model !is null && model.isValid()) ? model.findChildByName("GuardRig") : null;
            @m_figure = (rig !is null && rig.isValid()) ? rig.findChildByName("Guard") : null;
        }
        return m_figure;
    }
}
