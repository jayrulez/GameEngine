// Snowline - the run's orchestrator (the reserved class Game): the clock, the gates and their
// penalties, the HUD, and the finish card.
//
// The gates announce themselves ("GateRegistered") and report each crossing ("GatePassed",
// "GateMissed": round the outside costs kMissPenalty seconds); the finish line ends the run
// ("RunFinished"). The card shows the time, the penalties and the result; Jump starts the next run
// ("RunRestart" puts the rider back at the top and the gates back to waiting).

Guid kHudDoc = Guid("9ff3ed63-f33c-487a-a88f-2761d48af746");    // UI/Hud
Guid kFinishDoc = Guid("409e4cea-20a2-4705-9a54-059fa662850f"); // UI/Finish

const float kMissPenalty = 2.0f;

class Game
{
    private bool m_running = true;
    private float m_time = 0.0f;    // seconds on the clock this run
    private float m_penalty = 0.0f; // seconds added for missed gates
    private int m_gates = 0;        // how many the course has
    private int m_passed = 0;
    private int m_missed = 0;
    private float m_flash = 0.0f;   // how long the "+2 s" shows yet

    void launch()
    {
        ui::push(kHudDoc);
        showGates();
    }

    void update(float dt)
    {
        if (m_running)
        {
            m_time += dt;
            ui::findLabel("hud-time").setText(clock(m_time + m_penalty));
            if (m_flash > 0.0f)
            {
                m_flash -= run::realDeltaTime();
                if (m_flash <= 0.0f)
                {
                    ui::find("hud-penalty").setVisible(false);
                }
            }
        }
        else if (Input::wasReleased("Jump"))
        {
            ui::pop(); // the finish card
            restart();
        }
    }

    void onGateRegistered(int index)
    {
        m_gates += 1;
        showGates();
    }

    void onGatePassed(int index)
    {
        m_passed += 1;
        showGates();
    }

    void onGateMissed(int index)
    {
        m_missed += 1;
        m_penalty += kMissPenalty;
        m_flash = 1.2f;
        View@ penalty = ui::find("hud-penalty");
        penalty.setVisible(true);
        showGates();
    }

    void onRunFinished(int unused)
    {
        m_running = false;
        Screen@ s = ui::push(kFinishDoc);
        s.findLabel("finish-time").setText(clock(m_time + m_penalty));
        s.findLabel("finish-detail").setText(m_missed == 0
            ? "Every gate, clean"
            : "Ride " + clock(m_time) + "   +" + int(m_penalty) + " s for " + m_missed
                  + (m_missed == 1 ? " missed gate" : " missed gates"));
    }

    private void restart()
    {
        m_running = true;
        m_time = 0.0f;
        m_penalty = 0.0f;
        m_passed = 0;
        m_missed = 0;
        m_flash = 0.0f;
        ui::find("hud-penalty").setVisible(false);
        showGates();
        run::events().emit("RunRestart", 0); // in a run the scene bus is the run bus
    }

    private void showGates()
    {
        ui::findLabel("hud-gates").setText("" + m_passed + " / " + m_gates);
    }

    // m:ss.cc
    private string clock(float seconds)
    {
        int whole = int(seconds);
        int hundredths = int((seconds - float(whole)) * 100.0f);
        int s = whole % 60;
        return "" + (whole / 60) + ":" + (s < 10 ? "0" : "") + s + "." + (hundredths < 10 ? "0" : "")
               + hundredths;
    }
}
