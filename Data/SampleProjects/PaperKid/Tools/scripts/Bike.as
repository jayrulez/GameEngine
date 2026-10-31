// Bike: the player's ride, on the Bike entity (a Character component, which is a kinematic
// capsule).
//
// "Move": Y is throttle and brake, X steers. The bike keeps a heading and a signed speed, turns
// the heading (harder the faster it goes, and reversed while backing up), drives the character
// along it, and points the entity the same way.
//
// "Throw" launches a paper along the aim: the bike's forward, biased toward the nearest delivery
// zone in front (the soft auto-aim), with an upward arc. The Level owns the paper count: each
// throw is "PaperThrown", and the Level answers with "PapersLeft".
//
// A crash ("Crashed", sent by an Obstacle) knocks the bike back against the way it was going and
// leaves the steering and throttle weak for a moment; the Level takes the time penalty.
//
// The feel: the bike leans into its turns (harder the faster it goes) and wobbles while it
// recovers from a crash, which kicks up road dust. A throw and a crash each have a sound, pitched a
// little at random so repeats do not sound the same, and each rumbles the pad (a heavy jolt for a
// crash, a flick for a throw). A cleared block bursts confetti over the rider. Near a subscriber,
// the throw is previewed before it is made: a trail of glowing dots along the path a paper would
// take, drifting forward, and a ring spinning on the porch the throw is pulled toward. These are
// the AimDot and TargetRing prefabs, spawned once and moved each frame.

Guid kPaper = Guid("{{Prefab:Newspaper}}");
Guid kAimDot = Guid("{{Prefab:AimDot}}");
Guid kTargetRing = Guid("{{Prefab:TargetRing}}");
Guid kFxDust = Guid("{{Prefab:FxDust}}");
Guid kFxConfetti = Guid("{{Prefab:FxConfetti}}");

const int kAimDots = 14;
const float kAimStep = 0.09f; // flight seconds between dots
Guid kThrowSound = Guid("{{Throw}}");
// The kid's clips (Models/KidBike, from Tools/blender/kid_bike.py). Ride turns the wheels twice and
// the pedals once a second, covering 4.4 m of road, so it plays at the bike's speed divided by
// that. Each throw clip is the same one-second cycle with that arm's throw in its first half, so
// Ride picks up where it ends. He throws with the hand on the side the paper goes.
Guid kRideClip = Guid("{{Clip:Ride}}");
Guid kThrowLeftClip = Guid("{{Clip:ThrowLeft}}");
Guid kThrowRightClip = Guid("{{Clip:ThrowRight}}");
const float kRideMetres = 4.4f;
Guid kCrashSound = Guid("{{Crash}}");

class Bike
{
    private Entity@ self;

    [11.0, "Top forward speed (m/s)"] float maxSpeed;
    [3.5, "Top reverse speed (m/s)"] float reverseSpeed;
    [10.0, "Throttle ramp (m/s^2)"] float acceleration;
    [22.0, "Brake ramp (m/s^2)"] float braking;
    [5.0, "Roll-down when coasting (m/s^2)"] float coastDeceleration;
    [120.0, "Turn rate at full speed (deg/s)"] float turnSpeedDegrees;
    [0.3, "Steering authority at a crawl (0..1)"] float minSteerFraction;

    [9.0, "Throw speed (m/s)"] float throwSpeed;
    [0.55, "Throw arc (upward share)"] float throwArc;
    [0.85, "Auto-aim strength (0 = straight ahead, 1 = lands on the zone)"] float autoAim;
    [16.0, "Auto-aim reach (m)"] float aimRange;
    [2, "The delivery zones' collision group"] int zoneGroup;

    [1.5, "After a crash, how long the controls stay weak (s)"] float crashTime;
    [0.25, "The controls' strength while recovering (0..1)"] float crashControl;
    [4.0, "The speed a crash knocks the bike back at (m/s)"] float knockback;
    [14.0, "Lean into a turn at full speed (deg)"] float maxLean;

    private float m_heading = 0.0f; // radians; 0 faces +Z
    private float m_speed = 0.0f;
    private int m_papers = -1;      // -1 until the Level reports the count
    private float m_aimX = 0.0f;
    private float m_aimZ = 1.0f;
    private bool m_hasTarget = false;
    private Float3 m_target = Float3(0.0f, 0.0f, 0.0f);
    private float m_recovering = 0.0f; // seconds of weak controls left after a crash
    private float m_lean = 0.0f;       // degrees, eased toward the steering
    private array<Entity@> m_dots;
    private Entity@ m_ring;
    private float m_clock = 0.0f;      // drives the dots' drift and the ring's spin
    private Entity@ m_kid;             // the kid on his bike, the model whose clips play
    private float m_throwing = 0.0f;   // seconds of the Throw clip left to play

    Bike(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_kid = self.findChildByName("KidBike");
        if (m_kid !is null && m_kid.isValid())
        {
            SceneAnimation anim = SceneAnimation::of(self.scene);
            anim.setClip(m_kid, kRideClip);
            anim.play(m_kid);
        }
        // Start facing the way the scene placed the bike.
        Float3 forward = Quaternion::RotateVector(self.rotation(), Float3(0.0f, 0.0f, 1.0f));
        m_heading = Math::Atan2(forward.x, forward.z);
        // The throw's guides, hidden until there is a throw to show.
        Float3 at = self.position();
        for (int i = 0; i < kAimDots; i++)
        {
            Entity@ dot = ScenePrefabs::of(self.scene).spawn(kAimDot, at);
            if (dot !is null && dot.isValid())
            {
                dot.setActive(false);
                m_dots.insertLast(dot);
            }
        }
        @m_ring = ScenePrefabs::of(self.scene).spawn(kTargetRing, at);
        if (m_ring !is null && m_ring.isValid())
        {
            m_ring.setActive(false);
        }
    }

    void onPapersLeft(int papers)
    {
        m_papers = papers;
    }

    void onCrashed(int unused)
    {
        if (m_recovering > 0.0f)
        {
            return; // still down from the last one
        }
        m_recovering = crashTime;
        // Bounce back off whatever it ran into, against the way the bike was going.
        m_speed = (m_speed >= 0.0f) ? -knockback : knockback;
        Audio::playOneShot(kCrashSound, AudioBus::Effects, 1.0f, Random::range(0.9f, 1.1f));
        Input::rumble(0.9f, 0.5f, 0.35f); // the jolt of a crash
        // Road dust off the wheels (the bike's centre is 0.9 above them).
        ScenePrefabs::of(self.scene).spawn(kFxDust, self.position() + Float3(0.0f, -0.8f, 0.0f));
        self.scene.events.emit("BikeCrashed", 1);
    }

    // The block is cleared: confetti bursts over the rider and hangs in the slow motion that
    // follows.
    void onQuotaMet(int secondsLeft)
    {
        ScenePrefabs::of(self.scene).spawn(kFxConfetti, self.position() + Float3(0.0f, 1.6f, 0.0f));
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (d <= 0.0f)
        {
            return;
        }
        Float2 move = Input::value2D("Move");
        if (m_recovering > 0.0f)
        {
            m_recovering -= d;
            move = move * crashControl;
        }
        updateSpeed(move.y, d);
        updateHeading(move.x, d);
        Float3 forward = facing();
        CharacterComponent::of(self).move(forward.x * m_speed, forward.z * m_speed);
        // Lean into the turn (seen from behind, a positive roll tips the top toward screen right,
        // the way a right turn leans), eased so it settles rather than snaps, and wobble while
        // recovering.
        float authority = Math::Abs(m_speed) / maxSpeed;
        if (authority > 1.0f) { authority = 1.0f; }
        float lean = move.x * maxLean * authority;
        m_lean += (lean - m_lean) * clamp01(8.0f * d);
        float wobble = (m_recovering > 0.0f) ? Math::Sin(m_recovering * 24.0f) * 9.0f * (m_recovering / crashTime) : 0.0f;
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_heading), m_lean + wobble);

        m_clock += d;
        if (m_papers == 0)
        {
            hideGuides();
        }
        else
        {
            computeAim();
            placeGuides();
            if (Input::wasPressed("Throw") && throwPaper())
            {
                startThrowClip();
                Audio::playOneShot(kThrowSound, AudioBus::Effects, 0.8f, Random::range(0.9f, 1.15f));
                Input::rumble(0.0f, 0.25f, 0.05f); // the flick of a throw
                self.scene.events.emit("PaperThrown", 1);
            }
        }
        animateKid(d);
    }

    // The kid pedals as fast as the bike goes (and stops pedalling when it stops); a throw plays
    // at least at walking pace, so it is seen even from a standstill.
    private void animateKid(float d)
    {
        if (m_kid is null || !m_kid.isValid())
        {
            return;
        }
        float pace = Math::Abs(m_speed) / kRideMetres;
        if (m_throwing > 0.0f)
        {
            if (pace < 1.0f) { pace = 1.0f; }
            m_throwing -= d * pace;
            if (m_throwing <= 0.0f)
            {
                SceneAnimation anim = SceneAnimation::of(self.scene);
                anim.setClip(m_kid, kRideClip);
                anim.play(m_kid);
            }
        }
        SkeletalAnimationComponent::of(m_kid).speed = pace;
    }

    private void startThrowClip()
    {
        if (m_kid is null || !m_kid.isValid())
        {
            return;
        }
        // Which side of the bike the paper goes: toward the porch it is aimed at, otherwise along
        // the aim. The bike's +X is the kid's left (it turns by m_heading about +Y, which takes +X
        // to (cos, 0, -sin)). Straight ahead, he throws right-handed.
        float toX = m_aimX;
        float toZ = m_aimZ;
        if (m_hasTarget)
        {
            Float3 at = self.worldPosition();
            toX = m_target.x - at.x;
            toZ = m_target.z - at.z;
        }
        float across = toX * Math::Cos(m_heading) - toZ * Math::Sin(m_heading);
        SceneAnimation anim = SceneAnimation::of(self.scene);
        anim.setClip(m_kid, (across > 0.05f) ? kThrowLeftClip : kThrowRightClip);
        anim.play(m_kid);
        m_throwing = 1.0f;
    }

    private float clamp01(float v)
    {
        if (v < 0.0f) { return 0.0f; }
        if (v > 1.0f) { return 1.0f; }
        return v;
    }

    private Float3 facing()
    {
        return Float3(Math::Sin(m_heading), 0.0f, Math::Cos(m_heading));
    }

    private void updateSpeed(float throttle, float d)
    {
        if (throttle > 0.0f)
        {
            m_speed += throttle * ((m_speed < 0.0f) ? braking : acceleration) * d;
        }
        else if (throttle < 0.0f)
        {
            m_speed += throttle * ((m_speed > 0.0f) ? braking : acceleration) * d;
        }
        else if (m_speed > 0.0f)
        {
            m_speed -= coastDeceleration * d;
            if (m_speed < 0.0f) { m_speed = 0.0f; }
        }
        else if (m_speed < 0.0f)
        {
            m_speed += coastDeceleration * d;
            if (m_speed > 0.0f) { m_speed = 0.0f; }
        }
        if (m_speed > maxSpeed) { m_speed = maxSpeed; }
        if (m_speed < -reverseSpeed) { m_speed = -reverseSpeed; }
    }

    // A positive yaw turns +Z toward +X, which is screen left from behind, so steering right
    // lowers it.
    private void updateHeading(float steer, float d)
    {
        if (steer == 0.0f || m_speed == 0.0f)
        {
            return;
        }
        float authority = Math::Abs(m_speed) / maxSpeed;
        if (authority > 1.0f) { authority = 1.0f; }
        if (authority < minSteerFraction) { authority = minSteerFraction; }
        float direction = (m_speed >= 0.0f) ? 1.0f : -1.0f;
        m_heading -= steer * direction * Math::DegreesToRadians(turnSpeedDegrees) * authority * d;
    }

    // The throw's horizontal aim: forward, pulled toward the nearest zone in front.
    private void computeAim()
    {
        Float3 pos = self.worldPosition();
        Float3 f = facing();
        float aimX = f.x;
        float aimZ = f.z;
        m_hasTarget = false;
        array<Entity@>@ zones = ScenePhysics::of(self.scene).overlapSphere(pos.x, pos.y, pos.z, aimRange,
                                                                          1 << zoneGroup);
        float best = aimRange * aimRange + 1.0f;
        for (uint i = 0; i < zones.length(); i++)
        {
            Float3 z = zones[i].worldPosition();
            float dx = z.x - pos.x;
            float dz = z.z - pos.z;
            float dist2 = dx * dx + dz * dz;
            if (dist2 < 0.0001f)
            {
                continue;
            }
            float len = Math::Sqrt(dist2);
            if ((dx / len) * f.x + (dz / len) * f.z > 0.1f && dist2 < best)
            {
                best = dist2;
                aimX = f.x + (dx / len - f.x) * autoAim;
                aimZ = f.z + (dz / len - f.z) * autoAim;
                m_hasTarget = true;
                m_target = z;
            }
        }
        float l = Math::Sqrt(aimX * aimX + aimZ * aimZ);
        m_aimX = aimX / l;
        m_aimZ = aimZ / l;
    }

    private Float3 launchPoint()
    {
        Float3 pos = self.worldPosition();
        Float3 f = facing();
        // Above the rider's head, so the paper never meets the bike's own capsule on the way out.
        return Float3(pos.x + f.x * 0.6f, pos.y + 1.6f, pos.z + f.z * 0.6f);
    }

    // The paper's launch velocity: along the aim at the throw speed, plus the bike's own speed.
    // With a zone locked, the horizontal part leans (by autoAim) toward the velocity that lands the
    // paper on the zone in its flight time. The soft auto-aim corrects the range as well as the
    // direction, so a throw at a marked porch from a moving bike usually lands.
    private Float3 launchVelocity()
    {
        float mag = Math::Sqrt(1.0f + throwArc * throwArc);
        Float3 carry = facing() * m_speed;
        float vx = m_aimX / mag * throwSpeed + carry.x;
        float vy = throwArc / mag * throwSpeed;
        float vz = m_aimZ / mag * throwSpeed + carry.z;
        if (m_hasTarget)
        {
            Float3 from = launchPoint();
            float g = -ScenePhysics::of(self.scene).gravity().y;
            float drop = from.y - (m_target.y + 0.4f);
            float flight = (vy + Math::Sqrt(vy * vy + 2.0f * g * drop)) / g;
            if (flight > 0.05f)
            {
                vx += ((m_target.x - from.x) / flight - vx) * autoAim;
                vz += ((m_target.z - from.z) / flight - vz) * autoAim;
            }
        }
        return Float3(vx, vy, vz);
    }

    // Places the throw preview under the scene's gravity: a dot every kAimStep seconds of flight.
    // The row drifts forward one step each half second, shrinks toward its end, and stops where the
    // path meets the ground. The ring sits on the zone the throw is pulled toward. Both show only
    // while a subscriber is in reach, so the road stays clear between houses.
    private void placeGuides()
    {
        if (!m_hasTarget)
        {
            hideGuides();
            return;
        }
        Float3 p = launchPoint();
        Float3 v = launchVelocity();
        float g = ScenePhysics::of(self.scene).gravity().y;
        float drift = (m_clock * 2.0f) - float(int(m_clock * 2.0f));
        bool landed = false;
        for (uint i = 0; i < m_dots.length(); i++)
        {
            float t = kAimStep * (float(i) + drift + 0.5f);
            float y = p.y + v.y * t + 0.5f * g * t * t;
            if (landed || y < 0.0f)
            {
                landed = true;
                m_dots[i].setActive(false);
                continue;
            }
            m_dots[i].setActive(true);
            m_dots[i].setPosition(Float3(p.x + v.x * t, y, p.z + v.z * t));
            float size = 1.0f - 0.45f * (float(i) / float(m_dots.length()));
            m_dots[i].setScale(size, size, size);
        }
        if (m_ring is null || !m_ring.isValid())
        {
            return;
        }
        m_ring.setActive(true);
        m_ring.setPosition(Float3(m_target.x, 0.02f, m_target.z));
        m_ring.setRotationEuler(0.0f, m_clock * 70.0f, 0.0f);
        float pulse = 1.0f + 0.08f * Math::Sin(m_clock * 6.0f);
        m_ring.setScale(pulse, 1.0f, pulse);
    }

    private void hideGuides()
    {
        for (uint i = 0; i < m_dots.length(); i++)
        {
            m_dots[i].setActive(false);
        }
        if (m_ring !is null && m_ring.isValid())
        {
            m_ring.setActive(false);
        }
    }

    private bool throwPaper()
    {
        Entity@ paper = ScenePrefabs::of(self.scene).spawn(kPaper, launchPoint());
        if (paper is null || !paper.isValid())
        {
            return false;
        }
        // An impulse is mass times the change in speed; the mass comes from the paper's rigid body.
        RigidBodyComponent@ body = RigidBodyComponent::of(paper);
        float mass = (body !is null && body.mass > 0.0f) ? body.mass : 1.0f;
        ScenePhysics::of(self.scene).applyImpulse(paper, launchVelocity() * mass);
        return true;
    }
}
