// Board - the rider on the snow.
//
// A grounded character moves only by its input, so the board drives the whole velocity
// (CharacterComponent.drive): each frame, gravity less its part along the ground's normal is added
// to the velocity the rider has, kept along the ground; snow friction and air drag take some back.
// The stick turns that velocity (a carve), at a rate, keeping its speed but for what a hard carve
// scrubs off; a tuck cuts the drag and the turning; a jump pops the rider off the snow. In the air,
// gravity alone: the stick spins the rider instead of carving, and the grab button holds a grab.
//
// A landing after real air (`minAir`) is judged against the path: the board within
// `landTolerance` of the way the rider is travelling, forward or switch (backward), is clean and
// gives a burst of speed; anything else is a crash, which takes most of the speed and the control
// for `crashTime`. Each trick is announced for the game to score: "TrickAir" (centiseconds in the
// air), "TrickSpin" (degrees, in half turns), "TrickGrab" (centiseconds grabbed), then
// "TrickLanded" (1 clean, 0 crashed); a crash is "RiderCrashed" too.
//
// Riding into something solid at speed (a tree, a rock: what took the speed did not go into the
// snow) is a crash too. Riding past a trunk close and fast without touching it is a near miss
// ("NearMiss", once per trunk): an overlap in the trees' collision group around the rider.
//
// A kicker launches the rider itself ("KickerAngle", then "KickerLaunch", from Kicker.as): the
// board takes off along its heading at the speed and angle given. Landing short in a crevasse
// ("GapShort", from Gap.as) is a crash, and so is the avalanche catching the rider
// ("AvalancheCaught").
//
// The rider's animation graph (graph.py) follows by parameters: Lean (heel -1 to toe +1), Tuck,
// Airborne, Grab and Crashed. The lean is the turn the board makes, not the stick: as a rider leans
// into a turn by its speed times how fast it turns (the lean that balances the turn's pull,
// atan(v w / g)), full at `fullLean` degrees. Riding straight is upright whatever the stick says,
// and a tuck, turning half as hard, leans less. The rider stands left foot forward facing the
// board's right side, so a right turn is on the toe edge.
//
// With `autopilot`, the board steers itself down the course line (playtests, the measurements).
const float kStep = 1.0f / 60.0f; // the longest step the board integrates at once (s)

// The board's sounds (sounds.py, Kenney's Impact Sounds): the pop of a jump or a kicker's lip, a
// clean landing, a crash into the snow and a hit on a tree or rock.
Guid kPopSound = Guid("5abbb76b-6d21-4ca0-8d9f-988b1c4c6369");     // Audio/Pop
Guid kLandSound = Guid("73acb3f2-1241-47c2-9101-5680a9f40548");    // Audio/Land
Guid kCrashSound = Guid("d03eb225-103d-4ba3-bd28-e0fde147e313");   // Audio/Crash
Guid kTreeHitSound = Guid("a3179659-c479-447f-af6b-74ea09672ca8"); // Audio/TreeHit

class Board
{
    private Entity@ self;

    [null, "The course line (an entity with a spline)"] Entity@ course;
    [false, "Steer down the course line on its own (playtests)"] bool autopilot;
    [15.0, "How far down the course line the autopilot aims (m)"] float lookAhead;
    [0.0, "The stick the autopilot holds in the air, a spin (playtests: -1 to 1)"] float autopilotSpin;
    [false, "The autopilot holds a grab in the air (playtests)"] bool autopilotGrab;
    [12.0, "How far ahead the rider looks with nothing to look at (m)"] float gazeAhead;
    [4.0, "How quickly the rider's gaze moves to a new target (per second)"] float lookEase;
    [80.0, "Fastest turn of the velocity, a full carve (degrees a second)"] float carveRate;
    [0.35, "Speed a full carve scrubs off (share of gravity's pull into the slope)"] float carveScrub;
    [12.0, "Speed at which a carve scrubs in full; slower, in proportion (m/s)"] float scrubSpeed;
    [0.04, "Snow friction (share of gravity's pull into the slope)"] float friction;
    [0.0025, "Air drag (per m/s of speed, per second)"] float drag;
    [0.45, "Air drag in a tuck (share of the standing drag)"] float tuckDrag;
    [0.5, "Turning in a tuck (share of the full carve)"] float tuckTurn;
    [5.5, "Upward speed of a jump (m/s)"] float jumpSpeed;
    [540.0, "Fastest spin in the air, the stick full over (degrees a second)"] float spinRate;
    [0.35, "Shortest time in the air that is a trick, judged as it lands (s)"] float minAir;
    [35.0, "How far the board may be off the path, forward or switch, for a clean landing (degrees)"] float landTolerance;
    [3.0, "A clean landing's burst of speed (m/s)"] float landBurst;
    [0.25, "Share of its speed a crash keeps"] float crashKeep;
    [1.2, "How long a crash takes the control away (s)"] float crashTime;
    [5.0, "Speed lost to something solid in one frame that is a crash (m/s)"] float hitLoss;
    [2, "The physics collision group of the trees and rocks"] int treesGroup;
    [2.2, "How close a trunk passes for a near miss (m, from the rider's centre to its axis)"] float nearMissReach;
    [9.0, "Slowest speed a near miss counts at (m/s)"] float nearMissSpeed;
    ["asset:Prefab", "The board's mark in the snow (Prefabs/TrackMark)"] Guid@ trackMark;
    [1.5, "Distance between the track's marks (m)"] float trackSpacing;
    [2.0, "Slowest speed that leaves a track (m/s)"] float trackSpeed;
    [0.45, "Carve (share of a full one) above which the board throws spray"] float sprayCarve;
    [6.0, "Slowest speed that throws spray (m/s)"] float spraySpeed;
    [25.0, "Speed at which the wind is at its loudest (m/s)"] float windSpeed;
    [0.7, "The wind's loudest volume"] float windVolume;
    [40.0, "The lean into a turn that is the full carve pose (degrees)"] float fullLean;

    private Float3 m_start;
    private float m_yaw = 0.0f;   // radians; 0 faces +Z
    private float m_lean = 0.0f;  // the eased lean into the turn, -1 heel .. +1 toe
    private float m_lastHeading = 0.0f; // the travel's heading last frame (radians), for the turn rate
    private bool m_headingKnown = false;
    private Float3 m_lookAt = Float3(0.0f, 0.0f, 0.0f); // where the head looks (eased)
    private bool m_lookKnown = false;
    private bool m_airborne = false;
    private Entity@ m_figure;     // the skinned mesh the graph drives
    private float m_sinceMark = 0.0f; // metres ridden since the last mark of the track
    private bool m_spraying = false;
    private int m_nextGate = 0;   // the gate the rider looks at next
    private Quaternion m_startRotation;
    private bool m_fromRest = true; // a run begins: the next update starts from rest
    private bool m_inAir = false;   // off the snow since the last landing
    private float m_air = 0.0f;     // how long this time in the air has lasted (s)
    private float m_spin = 0.0f;    // how far the rider has spun in it (radians, signed)
    private float m_grabbed = 0.0f; // how long it has held a grab in it (s)
    private float m_crash = 0.0f;   // how long the crash still holds (s)
    private Float3 m_driven = Float3(0.0f, 0.0f, 0.0f); // the velocity the board drove last frame
    private float m_launchAngle = 0.0f; // the next kicker launch's angle above level (degrees)
    private float m_launchSpeed = 0.0f; // its speed (m/s); 0 when none is due
    private Float3 m_lastTrunk = Float3(1.0e9f, 0.0f, 0.0f); // the last trunk passed close (its centre)
    private bool m_crashDue = false; // a crash told to the board (a short gap), taken next update

    Board(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_start = self.position();
        m_startRotation = self.rotation();
        m_yaw = startYaw();
        // The run starts with the rider's first frame: the clock (Snowline.as) and the medal ghosts
        // count from here, so a rider level with a ghost crosses the line on its medal's time.
        self.scene.events.emit("RunStarted", 0);
    }

    // The skinned mesh the graph sits on: the model's root, its rig, the mesh under it. Found on
    // first use, not at start (the model's instance may not have its children yet then).
    private Entity@ figure()
    {
        if (m_figure is null || !m_figure.isValid())
        {
            Entity@ model = self.findChildByName("RiderModel");
            Entity@ rig = (model !is null && model.isValid()) ? model.findChildByName("RiderRig") : null;
            @m_figure = (rig !is null && rig.isValid()) ? rig.findChildByName("RiderModel") : null;
        }
        return m_figure;
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        if (d <= 0.0f)
        {
            return;
        }
        CharacterComponent@ c = CharacterComponent::of(self);
        bool struck = hitSomething(c);
        Float3 v = alongTheSnow(c);
        if (m_fromRest)
        {
            // Every run starts from rest, and on the frame after it begins: the clock (Snowline.as)
            // hears "RunStarted" or restarts on this frame and counts from the next, so the board
            // waits for it. Integrating this frame (the long first one after the scene loads) gave
            // the first run a head start the clock never saw, and a restarted one would carry the
            // last run's speed.
            c.drive(Float3(0.0f, 0.0f, 0.0f));
            m_driven = Float3(0.0f, 0.0f, 0.0f);
            m_fromRest = false;
            return;
        }
        Float3 at = self.position();
        float down = travelled(at);

        // What the rider asks for: the carve (-1 left .. +1 right), a tuck, a jump, a grab.
        float steer = 0.0f;
        bool tuck = false;
        bool jump = false;
        if (autopilot)
        {
            steer = autoSteer(v, at, down);
        }
        else
        {
            steer = Input::value2D("Move").x;
            tuck = Input::isDown("Tuck");
            jump = Input::wasPressed("Jump");
        }
        bool grab = autopilot ? autopilotGrab : Input::isDown("Grab");
        if (m_crash > 0.0f)
        {
            // Down in the snow: no control until the crash is over.
            m_crash -= d;
            steer = 0.0f;
            tuck = false;
            jump = false;
            grab = false;
        }

        bool grounded = c.grounded();
        if (m_launchSpeed > 0.0f)
        {
            // Off a kicker's lip: along the heading, up at the lip's angle, and into the air.
            float heading = (v.x * v.x + v.z * v.z > 0.04f) ? Math::Atan2(v.x, v.z) : m_yaw;
            float up = Math::DegreesToRadians(m_launchAngle);
            v = Float3(Math::Sin(heading) * Math::Cos(up) * m_launchSpeed, Math::Sin(up) * m_launchSpeed,
                       Math::Cos(heading) * Math::Cos(up) * m_launchSpeed);
            m_launchSpeed = 0.0f;
            grounded = false;
            Audio::playOneShot(kPopSound, AudioBus::Effects, 0.8f);
        }
        else if (jump && grounded && m_crash <= 0.0f)
        {
            Audio::playOneShot(kPopSound, AudioBus::Effects, 0.6f, 1.1f);
        }
        if (!grounded)
        {
            // In the air the stick spins the rider (the autopilot holds autopilotSpin).
            float stick = autopilot ? autopilotSpin : steer;
            float spin = (m_crash > 0.0f ? 0.0f : stick) * Math::DegreesToRadians(spinRate) * d;
            m_spin += spin;
            m_yaw += spin;
            m_air += d;
            if (grab)
            {
                m_grabbed += d;
            }
            m_inAir = true;
        }
        // The frame in steps of at most kStep: a long frame (the first after the scene loads, a
        // hitch) is integrated as the short ones it stands for, so the speed a run gathers does not
        // hang on the frame rate. One long step had given a first run a 0.4 m/s head start.
        int steps = int(Math::Ceil(d / kStep));
        float h = d / float(steps);
        // A jump pops the rider off the snow in the first step; the rest of the frame is in the
        // air. (Integrated as still on the snow, each later step laid the velocity back along the
        // slope and took the jump away: no jump at all whenever a frame ran longer than kStep.)
        bool onSnow = grounded;
        for (int i = 0; i < steps; ++i)
        {
            bool popping = jump && i == 0 && onSnow && m_crash <= 0.0f;
            v = integrate(c, v, onSnow, steer, tuck, popping, h);
            if (popping)
            {
                onSnow = false;
            }
        }
        if (grounded && m_inAir)
        {
            v = land(v);
        }
        if (struck)
        {
            Audio::playOneShot(kTreeHitSound, AudioBus::Effects, 0.9f);
        }
        if (struck || (m_crashDue && m_crash <= 0.0f))
        {
            v = crash(v);
        }
        m_crashDue = false;
        nearMiss(at, v);
        c.drive(v);
        m_driven = v;
        face(v, d, grounded);
        track(at, v, grounded, d);
        snow(v, steer, grounded);
        wind(v);
        animate(grounded ? turnLean(v, d) : 0.0f, tuck, !grounded, grab && !grounded, d);
        look(at, v, d);
    }

    // One step of the board's motion: on the snow, gravity along the slope, friction, a carve's
    // scrub, drag and the carve's turn (and a jump's pop); in the air, gravity.
    private Float3 integrate(CharacterComponent@ c, Float3 v, bool grounded, float steer, bool tuck,
                             bool jump, float d)
    {
        const float g = 9.81f;
        if (grounded)
        {
            Float3 n = c.groundNormal;
            // Keep the velocity along the ground, then add gravity's pull along it.
            float vn = v.x * n.x + v.y * n.y + v.z * n.z;
            v = Float3(v.x - n.x * vn, v.y - n.y * vn, v.z - n.z * vn);
            float gn = -g * n.y; // gravity (0, -g, 0) along the normal
            v = Float3(v.x - n.x * gn * d, v.y + (-g - n.y * gn) * d, v.z - n.z * gn * d);
            // Friction and a carve's scrub (from the press into the slope) and drag, against the motion.
            float speed = Math::Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > 0.01f)
            {
                // A carve's scrub grows with speed (the turn's sideways push), nothing at a crawl.
                float bite = (speed < scrubSpeed) ? speed / scrubSpeed : 1.0f;
                float grip = (friction + carveScrub * Math::Abs(steer) * bite) * g * n.y;
                float air = drag * (tuck ? tuckDrag : 1.0f) * speed * speed;
                float slow = (grip + air) * d;
                float keep = (speed > slow) ? (speed - slow) / speed : 0.0f;
                v = Float3(v.x * keep, v.y * keep, v.z * keep);
            }
            v = carve(v, steer * (tuck ? tuckTurn : 1.0f), d);
            if (jump)
            {
                v = Float3(v.x, v.y + jumpSpeed, v.z);
            }
            return v;
        }
        return Float3(v.x, v.y - g * d, v.z);
    }

    // Snowline.as starts the next run (the finish card's Jump): back to the top, at rest, facing
    // down the course as at the start, the first gate next.
    void onRunRestart(int unused)
    {
        CharacterComponent::of(self).setPosition(m_start);
        self.setRotation(m_startRotation);
        m_yaw = startYaw();
        m_fromRest = true;
        m_inAir = false;
        m_air = 0.0f;
        m_spin = 0.0f;
        m_grabbed = 0.0f;
        m_crash = 0.0f;
        m_crashDue = false;
        m_driven = Float3(0.0f, 0.0f, 0.0f);
        m_nextGate = 0;
        m_lean = 0.0f;
        m_headingKnown = false;
        m_lookKnown = false;
    }

    // The facing the scene started the rider with (radians; 0 faces +Z): the board turns from it,
    // not from +Z, so the rider sets off without swinging round first.
    private float startYaw()
    {
        Float3 forward = Quaternion::RotateVector(m_startRotation, Float3(0.0f, 0.0f, 1.0f));
        return Math::Atan2(forward.x, forward.z);
    }

    // A kicker's launch (Kicker.as): its angle comes first, then its speed, applied next update.
    void onKickerAngle(int tenths) { m_launchAngle = float(tenths) / 10.0f; }
    void onKickerLaunch(int centimetres) { m_launchSpeed = float(centimetres) / 100.0f; }

    // Down in the crevasse, short of its far side (Gap.as), or caught by the avalanche (Avalanche.as).
    void onGapShort(int unused) { m_crashDue = true; }
    void onAvalancheCaught(int unused) { m_crashDue = true; }

    // The gate the rider looks at next is the one after the last crossed (passed or missed).
    void onGatePassed(int index) { m_nextGate = index + 1; }
    void onGateMissed(int index) { m_nextGate = index + 1; }

    // Turns the velocity's ground direction by the carve, keeping its speed. A right turn (steer
    // > 0) turns the heading toward -X when going +Z: the yaw (atan2(x, z)) falls.
    private Float3 carve(Float3 v, float steer, float d)
    {
        float flat = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (flat < 0.2f || steer == 0.0f)
        {
            return v;
        }
        float heading = Math::Atan2(v.x, v.z) - steer * Math::DegreesToRadians(carveRate) * d;
        return Float3(Math::Sin(heading) * flat, v.y, Math::Cos(heading) * flat);
    }

    // The autopilot's carve: toward a point down the course line, -1..1.
    private float autoSteer(Float3 v, Float3 at, float down)
    {
        if (course is null || !course.isValid())
        {
            return 0.0f;
        }
        SceneSplines@ splines = SceneSplines::of(self.scene);
        float length = splines.length(course);
        float aim = down + lookAhead;
        SplineHit@ ahead = splines.sampleAtDistance(course, (aim < length) ? aim : length);
        float flat = Math::Sqrt(v.x * v.x + v.z * v.z);
        float want = Math::Atan2(ahead.position.x - at.x, ahead.position.z - at.z);
        float have = (flat > 0.2f) ? Math::Atan2(v.x, v.z) : want;
        float turn = want - have;
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        // A turn to the left (the yaw rising) is a negative steer; full lock past 35 degrees off.
        float steer = -turn / Math::DegreesToRadians(35.0f);
        return (steer > 1.0f) ? 1.0f : ((steer < -1.0f) ? -1.0f : steer);
    }

    // The rider's head looks at the next gate (an AimIkComponent on the board's entity), then the
    // finish, while it lies ahead; once it is behind (the finish crossed, a gate gone by), down the
    // way the rider is travelling instead. The point eases from one target to the next, so the head
    // turns rather than snaps. (Looking at the finish after crossing it, the head swung round to
    // look back at it.) About eye height above the snow.
    private void look(Float3 at, Float3 v, float d)
    {
        AimIkComponent@ aim = AimIkComponent::of(self);
        if (aim is null)
        {
            return;
        }
        float flat = Math::Sqrt(v.x * v.x + v.z * v.z);
        float dx = (flat > 0.5f) ? v.x / flat : Math::Sin(m_yaw);
        float dz = (flat > 0.5f) ? v.z / flat : Math::Cos(m_yaw);
        Float3 want = Float3(at.x + dx * gazeAhead, at.y + 0.3f, at.z + dz * gazeAhead);
        Entity@ next = self.scene.find("Gate" + m_nextGate);
        if (next is null || !next.isValid())
        {
            @next = self.scene.find("Finish");
        }
        if (next !is null && next.isValid())
        {
            Float3 p = next.worldPosition();
            float tx = p.x - at.x;
            float tz = p.z - at.z;
            float dist = Math::Sqrt(tx * tx + tz * tz);
            // Ahead: within about 60 degrees of the travel.
            if (dist > 1.0f && (tx * dx + tz * dz) > 0.5f * dist)
            {
                want = p + Float3(0.0f, 1.2f, 0.0f);
            }
        }
        if (!m_lookKnown)
        {
            m_lookAt = want;
            m_lookKnown = true;
        }
        float ease = lookEase * d;
        ease = (ease < 1.0f) ? ease : 1.0f;
        m_lookAt = m_lookAt + (want - m_lookAt) * ease;
        SceneAnimation::of(self.scene).setIkTarget(self, m_lookAt);
    }

    // How far down the course line the rider is (m): the closest point's distance along it.
    private float travelled(Float3 at)
    {
        if (course is null || !course.isValid())
        {
            return 0.0f;
        }
        SplineHit@ here = SceneSplines::of(self.scene).closestPoint(course, at);
        return (here !is null && here.valid) ? here.distance : 0.0f;
    }

    // The board's track: a mark every `trackSpacing` metres on the snow, under the board, along the
    // heading. None in the air, nor at a crawl.
    private void track(Float3 at, Float3 v, bool grounded, float d)
    {
        float speed = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (!grounded || speed < trackSpeed || trackMark is null || trackMark.IsNil())
        {
            m_sinceMark = trackSpacing; // the first mark on touching down lands at once
            return;
        }
        m_sinceMark += speed * d;
        if (m_sinceMark < trackSpacing)
        {
            return;
        }
        m_sinceMark = 0.0f;
        // The character's position is its capsule's centre; the board is 0.9 m below it.
        Entity@ mark = ScenePrefabs::of(self.scene).spawn(trackMark, Float3(at.x, at.y - 0.9f, at.z));
        if (mark !is null && mark.isValid())
        {
            mark.setRotationEuler(0.0f, Math::RadiansToDegrees(Math::Atan2(v.x, v.z)), 0.0f);
        }
    }

    // The board's snow: spray off its edge while it carves hard and fast (the Spray child, played
    // and stopped only on a change), and a burst of powder as it lands (the Powder child).
    private void snow(Float3 v, float steer, bool grounded)
    {
        SceneParticles@ particles = SceneParticles::of(self.scene);
        float speed = Math::Sqrt(v.x * v.x + v.z * v.z);
        bool spraying = grounded && Math::Abs(steer) > sprayCarve && speed > spraySpeed;
        if (spraying != m_spraying)
        {
            Entity@ spray = self.findChildByName("Spray");
            if (spray !is null && spray.isValid())
            {
                if (spraying)
                {
                    particles.play(spray);
                }
                else
                {
                    particles.stop(spray);
                }
            }
            m_spraying = spraying;
        }
        if (grounded && m_airborne)
        {
            Entity@ powder = self.findChildByName("Powder");
            if (powder !is null && powder.isValid())
            {
                particles.restart(powder);
            }
        }
    }

    // The wind in the rider's ears: silent standing, rising with the speed to its loudest at
    // `windSpeed`, and a little higher in pitch the faster it blows.
    private void wind(Float3 v)
    {
        Entity@ source = self.findChildByName("Wind");
        if (source is null || !source.isValid())
        {
            return;
        }
        float speed = Math::Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        float k = speed / windSpeed;
        k = (k > 1.0f) ? 1.0f : k;
        SceneAudio audio = SceneAudio::of(self.scene);
        audio.setVolume(source, windVolume * k * k, 0.15f);
        audio.setPitch(source, 0.8f + 0.5f * k, 0.15f);
    }

    // The rider faces where it goes, easing round.
    // The velocity to carry on from. The character's own step takes away whatever went into a
    // surface; the snow under the board only turns it, though (its push is square to the motion
    // along it). So when what was lost since the last drive went into the ground:
    // - riding (or after a hop too short to be a trick, as over a kicker's curve), the board turns
    //   its own velocity along the snow and keeps its speed: the lost part had taken a rider from
    //   17 to 7 m/s up a kicker;
    // - landing from real air, it keeps what lies along the snow and loses what went into it.
    // Speed lost to anything else (a rock, a tree) stays lost.
    private Float3 alongTheSnow(CharacterComponent@ c)
    {
        Float3 v = c.velocity;
        if (!c.grounded())
        {
            return v;
        }
        Float3 n = c.groundNormal;
        Float3 lost = m_driven - v;
        float lostLength = Math::Sqrt(lost.x * lost.x + lost.y * lost.y + lost.z * lost.z);
        if (lostLength < 0.05f || Math::Abs(lost.x * n.x + lost.y * n.y + lost.z * n.z) < 0.8f * lostLength)
        {
            return v;
        }
        float into = m_driven.x * n.x + m_driven.y * n.y + m_driven.z * n.z;
        Float3 along = Float3(m_driven.x - n.x * into, m_driven.y - n.y * into, m_driven.z - n.z * into);
        float alongLength = Math::Sqrt(along.x * along.x + along.y * along.y + along.z * along.z);
        if (alongLength < 0.01f)
        {
            return v;
        }
        if (m_inAir && m_air >= minAir)
        {
            return along; // a landing
        }
        float speed = Math::Sqrt(m_driven.x * m_driven.x + m_driven.y * m_driven.y + m_driven.z * m_driven.z);
        float k = speed / alongLength;
        return Float3(along.x * k, along.y * k, along.z * k);
    }

    // Judges a landing: the board along the path (forward or switch) is clean, a burst of speed
    // along it; across it is a crash. Short hops (under minAir) are neither. The trick's parts are
    // announced for the game to score. Returns the velocity the rider rides on with.
    private Float3 land(Float3 v)
    {
        m_inAir = false;
        float air = m_air;
        float spun = m_spin;
        float grabbed = m_grabbed;
        m_air = 0.0f;
        m_spin = 0.0f;
        m_grabbed = 0.0f;
        float flat = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (air < minAir || flat < 1.0f)
        {
            return v;
        }
        float off = wrap(m_yaw - Math::Atan2(v.x, v.z));
        if (Math::Abs(off) > 1.5708f)
        {
            off = wrap(off - 3.14159f); // landing switch: judged against riding backward
        }
        bool clean = Math::Abs(off) <= Math::DegreesToRadians(landTolerance);
        int halfTurns = int(Math::Abs(spun) / 3.14159f + 0.5f);
        self.scene.events.emit("TrickAir", int(air * 100.0f + 0.5f));
        self.scene.events.emit("TrickSpin", halfTurns * 180);
        self.scene.events.emit("TrickGrab", int(grabbed * 100.0f + 0.5f));
        self.scene.events.emit("TrickLanded", clean ? 1 : 0);
        if (clean)
        {
            Audio::playOneShot(kLandSound, AudioBus::Effects, 0.9f);
            float k = (flat + landBurst) / flat;
            return Float3(v.x * k, v.y, v.z * k);
        }
        return crash(v);
    }

    // Down: most of the speed gone, the control for crashTime.
    private Float3 crash(Float3 v)
    {
        Audio::playOneShot(kCrashSound, AudioBus::Effects, 1.0f);
        m_crash = crashTime;
        self.scene.events.emit("RiderCrashed", 1);
        return Float3(v.x * crashKeep, v.y, v.z * crashKeep);
    }

    // Whether the character's step took a lot of speed into something that is not the snow: a
    // tree, a rock. (Speed the snow takes goes along its normal, and alongTheSnow gives it back.)
    private bool hitSomething(CharacterComponent@ c)
    {
        if (m_crash > 0.0f)
        {
            return false;
        }
        Float3 v = c.velocity;
        float driven = Math::Sqrt(m_driven.x * m_driven.x + m_driven.z * m_driven.z);
        float now = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (driven - now < hitLoss)
        {
            return false;
        }
        Float3 lost = m_driven - v;
        float lostLength = Math::Sqrt(lost.x * lost.x + lost.y * lost.y + lost.z * lost.z);
        if (c.grounded())
        {
            Float3 n = c.groundNormal;
            if (Math::Abs(lost.x * n.x + lost.y * n.y + lost.z * n.z) >= 0.8f * lostLength)
            {
                return false; // into the snow, not into something on it
            }
        }
        // Thrown upward by what it met: a slope under it, not an obstacle. A hop on a kicker's
        // curve came back down onto the steeper ramp still counted as in the air, and the ramp
        // turning it upward (20 m/s ahead to 14 ahead and 11 up) read as a crash into something.
        // A tree or a rock takes the speed back along the travel and pushes nothing up.
        if (v.y - m_driven.y > 0.5f * lostLength)
        {
            return false;
        }
        return true;
    }

    // A trunk passing within reach at speed, not touched: a near miss, once for each trunk.
    private void nearMiss(Float3 at, Float3 v)
    {
        if (m_crash > 0.0f || v.x * v.x + v.z * v.z < nearMissSpeed * nearMissSpeed)
        {
            return;
        }
        RayCastHit@ trunk = ScenePhysics::of(self.scene).nearestOverlap(at.x, at.y, at.z, nearMissReach,
                                                                        1 << treesGroup);
        if (trunk is null || !trunk.hit)
        {
            return;
        }
        Float3 p = trunk.position;
        float dx = p.x - m_lastTrunk.x;
        float dz = p.z - m_lastTrunk.z;
        if (dx * dx + dz * dz < 0.25f)
        {
            return; // this one again
        }
        m_lastTrunk = p;
        self.scene.events.emit("NearMiss", 1);
    }

    private float wrap(float a)
    {
        while (a > 3.14159f) { a -= 6.28318f; }
        while (a < -3.14159f) { a += 6.28318f; }
        return a;
    }

    // On the snow the rider turns to its travel (easing); in the air it holds the facing it spun to.
    private void face(Float3 v, float d, bool grounded)
    {
        if (!grounded || v.x * v.x + v.z * v.z < 0.04f)
        {
            self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
            return;
        }
        float turn = Math::Atan2(v.x, v.z) - m_yaw;
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        float ease = 8.0f * d;
        m_yaw += turn * ((ease < 1.0f) ? ease : 1.0f);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }

    // The graph's parameters: the lean eases toward the carve (a right turn is the toe edge).
    // How far the rider leans into the turn the board is making, -1 (heel) .. +1 (toe): the angle
    // that balances the turn, atan(speed x turn rate / g), against fullLean. A right turn (the
    // heading falling) is on the toe edge.
    private float turnLean(Float3 v, float d)
    {
        float speed = Math::Sqrt(v.x * v.x + v.z * v.z);
        if (speed < 1.0f)
        {
            m_headingKnown = false;
            return 0.0f;
        }
        float heading = Math::Atan2(v.x, v.z);
        float rate = m_headingKnown ? wrap(heading - m_lastHeading) / d : 0.0f;
        m_lastHeading = heading;
        m_headingKnown = true;
        float angle = Math::RadiansToDegrees(Math::Atan2(speed * Math::Abs(rate), 9.81f));
        float lean = angle / fullLean;
        lean = (lean > 1.0f) ? 1.0f : lean;
        return (rate < 0.0f) ? lean : -lean;
    }

    private void animate(float steer, bool tuck, bool airborne, bool grab, float d)
    {
        Entity@ figureNow = figure();
        if (figureNow is null || !figureNow.isValid())
        {
            return;
        }
        float ease = 6.0f * d;
        m_lean += (steer - m_lean) * ((ease < 1.0f) ? ease : 1.0f);
        SceneAnimation anim = SceneAnimation::of(self.scene);
        anim.setFloat(figureNow, "Lean", m_lean);
        anim.setBool(figureNow, "Tuck", tuck);
        anim.setBool(figureNow, "Airborne", airborne);
        anim.setBool(figureNow, "Grab", grab);
        anim.setBool(figureNow, "Crashed", m_crash > 0.0f);
        m_airborne = airborne;
    }
}
