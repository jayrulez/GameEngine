// Board - the rider on the snow.
//
// A grounded character moves only by its input, so the board drives the whole velocity
// (CharacterComponent.drive): each frame, gravity less its part along the ground's normal is added
// to the velocity the rider has, kept along the ground; snow friction and air drag take some back.
// The stick turns that velocity (a carve), at a rate, keeping its speed but for what a hard carve
// scrubs off; a tuck cuts the drag and the turning; a jump pops the rider off the snow. In the air,
// gravity alone, and the grab button holds a grab. At the finish, back to the top.
//
// The rider's animation graph (graph.py) follows by parameters: Lean (the carve, heel -1 to toe
// +1), Tuck, Airborne and Grab. The rider stands left foot forward facing the board's right side,
// so a right turn is a toe-side carve.
//
// With `autopilot`, the board steers itself down the course line (playtests, the measurements).
class Board
{
    private Entity@ self;

    [null, "The course line (an entity with a spline)"] Entity@ course;
    [false, "Steer down the course line on its own (playtests)"] bool autopilot;
    [15.0, "How far down the course line the autopilot aims (m)"] float lookAhead;
    [80.0, "Fastest turn of the velocity, a full carve (degrees a second)"] float carveRate;
    [0.35, "Speed a full carve scrubs off (share of gravity's pull into the slope)"] float carveScrub;
    [12.0, "Speed at which a carve scrubs in full; slower, in proportion (m/s)"] float scrubSpeed;
    [0.04, "Snow friction (share of gravity's pull into the slope)"] float friction;
    [0.0025, "Air drag (per m/s of speed, per second)"] float drag;
    [0.45, "Air drag in a tuck (share of the standing drag)"] float tuckDrag;
    [0.5, "Turning in a tuck (share of the full carve)"] float tuckTurn;
    [5.5, "Upward speed of a jump (m/s)"] float jumpSpeed;
    [6.0, "Distance before the end of the course line that counts as the finish (m)"] float finishMargin;
    ["asset:Prefab", "The board's mark in the snow (Prefabs/TrackMark)"] Guid@ trackMark;
    [1.5, "Distance between the track's marks (m)"] float trackSpacing;
    [2.0, "Slowest speed that leaves a track (m/s)"] float trackSpeed;
    [0.45, "Carve (share of a full one) above which the board throws spray"] float sprayCarve;
    [6.0, "Slowest speed that throws spray (m/s)"] float spraySpeed;
    [25.0, "Speed at which the wind is at its loudest (m/s)"] float windSpeed;
    [0.7, "The wind's loudest volume"] float windVolume;

    private Float3 m_start;
    private float m_yaw = 0.0f;   // radians; 0 faces +Z
    private float m_lean = 0.0f;  // the eased carve, -1 heel .. +1 toe
    private bool m_airborne = false;
    private Entity@ m_figure;     // the skinned mesh the graph drives
    private float m_sinceMark = 0.0f; // metres ridden since the last mark of the track
    private bool m_spraying = false;

    Board(Entity@ entity) { @self = entity; }

    void onStart()
    {
        m_start = self.position();
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
        Float3 v = c.velocity;
        Float3 at = self.position();
        const float g = 9.81f;
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
        bool grab = !autopilot && Input::isDown("Grab");

        bool grounded = c.grounded();
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
        }
        else
        {
            v = Float3(v.x, v.y - g * d, v.z);
        }
        c.drive(v);
        face(v, d);
        track(at, v, grounded, d);
        snow(v, steer, grounded);
        wind(v);
        animate(steer, tuck, !grounded, grab && !grounded, d);

        if (course !is null && course.isValid() &&
            down > SceneSplines::of(self.scene).length(course) - finishMargin)
        {
            c.setPosition(m_start);
        }
    }

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
    private void face(Float3 v, float d)
    {
        if (v.x * v.x + v.z * v.z < 0.04f)
        {
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
        m_airborne = airborne;
    }
}
