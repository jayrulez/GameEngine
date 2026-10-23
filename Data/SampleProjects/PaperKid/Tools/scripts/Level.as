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
        UILabel@ time = ui::findLabel("hud-time");
        if (m_timeLeft > kTicksFrom)
        {
            time.setOpacity(1.0f);
            return;
        }
        int second = int(m_timeLeft);
        if (second != m_lastTick)
        {
            m_lastTick = second;
            Audio::playOneShot(kTickSound, AudioBus::Effects, 0.7f);
        }
        // Dim for the second half of every second: a blink in step with the ticks.
        time.setOpacity((m_timeLeft - float(second)) < 0.5f ? 0.35f : 1.0f);
    }

    private void end(bool cleared, int reason)
    {
        m_ended = true;
        ui::findLabel("hud-time").setOpacity(1.0f); // not left dim by the blink
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
        ui::findLabel("hud-time").setText("" + int(m_timeLeft + 0.99f));
        ui::findLabel("hud-papers").setText("" + m_papersLeft);
        ui::findLabel("hud-deliveries").setText("" + m_delivered + " / " + quota);
    }
}
