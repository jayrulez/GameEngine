// Level - a block's rules and numbers: the countdown, the papers, the deliveries toward the quota.
// The block's scene sets the numbers on this script's properties, so each block carries its own.
//
// In a run the scene bus IS the run bus: a behavior's "Delivered" (Subscriber) and "PaperThrown"
// (Bike) reach this script and the Game alike. This script turns them into the level's outcome -
// "QuotaMet", or "LevelFailed" with the reason (0 time, 1 papers) - and tells the bike how many
// papers are left ("PapersLeft"). It writes the HUD's time, papers and deliveries. "QuotaMet" carries
// the whole seconds left, the Game's time bonus.
//
// The block's traffic is set here too: on the first frame (every behavior has started by then) it
// tells the cars and pedestrians their speeds, and the pedestrians where the ring road runs. A crash ("BikeCrashed", from the Bike) costs time.
// The clock's tells: "Hurry up!" once at 15 s left, when the block's music also speeds up (the
// arcade hurry-up: its playback rate eased up, tempo and pitch together; the next block's track
// starts at its own speed), a tick every second of the last 10 while the HUD's time blinks, and
// "Time over!" when it runs out. All on scene time, so a pause stops them.
Guid kHurrySound = Guid("{{HurryUp}}");
Guid kTickSound = Guid("{{Tick}}");
Guid kTimeOverSound = Guid("{{TimeOver}}");

const float kHurryAt = 15.0f;
const float kHurryMusicRate = 1.2f;    // 20% faster
const float kHurryMusicEase = 0.6f;    // seconds to get there
const float kTicksFrom = 10.0f;

// A HUD element: the top screen's first, then any (a HUD just replaced is still in the tree while it
// leaves, with the same ids, and would be found first).
Node@ hud(string id)
{
    ScreenStack@ stack = ScreenStack::of(gui::root().find("screens"));
    Screen@ top = stack !is null ? stack.top() : null;
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}

class Level
{
    private Scene@ scene;

    [120.0, "Time limit (s)"] float timeLimit;
    [4, "Deliveries to clear the block"] int quota;
    [8, "Papers for the block"] int papers;
    [3.0, "After the last paper, how long it may still land (s)"] float paperGrace;
    [6.0, "Car speed (m/s)"] float trafficSpeed;
    [1.6, "Pedestrian walking speed (m/s)"] float pedestrianSpeed;
    [5.0, "Seconds a crash costs"] float crashPenalty;
    [24.0, "The ring road's distance from the middle (m)"] float ring;

    private float m_timeLeft = 0.0f;
    private int m_papersLeft = 0;
    private int m_delivered = 0;
    private bool m_ended = false;
    private float m_graceLeft = -1.0f; // counting down once the last paper is thrown
    private bool m_trafficSet = false;
    private bool m_hurried = false;
    private int m_lastTick = -1; // the whole second last ticked
    private bool m_dimmed = false; // the clock dimmed by the blink

    Level(Scene@ s) { @scene = s; }

    void onStart()
    {
        m_timeLeft = timeLimit;
        m_papersLeft = papers;
        scene.events.emit("PapersLeft", m_papersLeft);
        updateHud();
    }

    void onUpdate(double dt)
    {
        if (!m_trafficSet)
        {
            m_trafficSet = true;
            scene.events.emit("TrafficSpeed", trafficSpeed);
            scene.events.emit("PedestrianSpeed", pedestrianSpeed);
            scene.events.emit("BlockRing", ring);
        }
        if (m_ended)
        {
            return;
        }
        m_timeLeft -= float(dt);
        if (m_timeLeft <= 0.0f)
        {
            m_timeLeft = 0.0f;
            Audio::playOneShot(kTimeOverSound, AudioBus::Effects);
            end(false, 0);
            return;
        }
        clockTells();
        if (m_graceLeft >= 0.0f)
        {
            m_graceLeft -= float(dt);
            if (m_graceLeft < 0.0f)
            {
                end(false, 1);
                return;
            }
        }
        updateHud();
    }

    void onBikeCrashed(int count)
    {
        if (m_ended)
        {
            return;
        }
        m_timeLeft -= crashPenalty;
        updateHud();
    }

    void onPaperThrown(int count)
    {
        if (m_ended || m_papersLeft <= 0)
        {
            return;
        }
        m_papersLeft -= 1;
        scene.events.emit("PapersLeft", m_papersLeft);
        if (m_papersLeft == 0)
        {
            m_graceLeft = paperGrace;
        }
        updateHud();
    }

    void onDelivered(int points)
    {
        if (m_ended)
        {
            return;
        }
        m_delivered += 1;
        updateHud();
        if (m_delivered >= quota)
        {
            end(true, 0);
        }
    }

    private void clockTells()
    {
        if (!m_hurried && m_timeLeft <= kHurryAt)
        {
            m_hurried = true;
            Audio::playOneShot(kHurrySound, AudioBus::Effects);
            Audio::setVoicePitch(Audio::musicVoice(), kHurryMusicRate, kHurryMusicEase);
        }
        if (m_timeLeft > kTicksFrom)
        {
            blink(false);
            return;
        }
        int second = int(m_timeLeft);
        if (second != m_lastTick)
        {
            m_lastTick = second;
            Audio::playOneShot(kTickSound, AudioBus::Effects, 0.7f);
        }
        // Dim for the second half of every second: a blink in step with the ticks.
        blink((m_timeLeft - float(second)) < 0.5f);
    }

    // The clock dimmed or not, set when it changes.
    private void blink(bool dim)
    {
        Node@ time = hud("hud-time");
        if (time is null || dim == m_dimmed)
        {
            return;
        }
        m_dimmed = dim;
        time.tween("opacity", dim ? "0.35" : "1", 0.0f, "linear");
    }

    private void end(bool cleared, int reason)
    {
        m_ended = true;
        blink(false); // not left dim by the blink
        updateHud();
        if (cleared)
        {
            run::events().emit("QuotaMet", int(m_timeLeft));
        }
        else
        {
            run::events().emit("LevelFailed", reason);
        }
    }

    private void updateHud()
    {
        if (hud("hud-time") is null)
        {
            return; // the HUD is not up
        }
        Label::of(hud("hud-time")).text = "" + int(m_timeLeft + 0.99f);
        Label::of(hud("hud-papers")).text = "" + m_papersLeft;
        Label::of(hud("hud-deliveries")).text = "" + m_delivered + " / " + quota;
    }
}
