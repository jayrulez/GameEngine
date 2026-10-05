// Snowline - the run's orchestrator (the reserved class Game): the clock, the gates and their
// penalties, the gems, the HUD, and the results.
//
// The gates announce themselves ("GateRegistered") and report each crossing ("GatePassed",
// "GateMissed": round the outside costs kMissPenalty seconds); the finish line holds the course's
// medal times ("MedalGold", "MedalSilver", "MedalBronze", in hundredths) and ends the run
// ("RunFinished"). The gems announce themselves too ("GemRegistered") and report being taken
// ("GemCollected"), each with how many it counts for.
//
// The results tally the run a row at a time (the ride, the gates, the gems, the time bonus), then
// land the final time with the medal it earned, the score and the best time this session. Jump
// skips to the end, then starts the next run ("RunRestart" puts the rider back at the top, the
// gates back to waiting and the gems back in place).

Guid kHudDoc = Guid("9ff3ed63-f33c-487a-a88f-2761d48af746");    // UI/Hud
Guid kFinishDoc = Guid("409e4cea-20a2-4705-9a54-059fa662850f"); // UI/Finish

// Audio/GemChime (sounds.py): the tally's tick, pitched up row by row, and lower as a medal lands.
Guid kChime = Guid("c47f1131-6df4-49ea-b2c7-6e3b3316a3ce");

const float kMissPenalty = 2.0f;
const int kGemPoints = 100;       // a gem's worth
const int kBonusPerSecond = 100;  // each second under the bronze time
const int kTallySteps = 6;        // the four rows, then the final time and medal, then the score
const float kTallyFirst = 0.5f;   // the first row's moment after the finish (s)
const float kTallyGap = 0.45f;    // between the rows (s)

class Game
{
    private bool m_running = true;
    private float m_time = 0.0f;    // seconds on the clock this run
    private float m_penalty = 0.0f; // seconds added for missed gates
    private int m_gates = 0;        // how many the course has
    private int m_passed = 0;
    private int m_missed = 0;
    private float m_flash = 0.0f;   // how long the "+2 s" shows yet
    private int m_gems = 0;         // how many the course has
    private int m_taken = 0;

    // The course's medal times (s, penalties included), from the finish line.
    private float m_gold = 0.0f;
    private float m_silver = 0.0f;
    private float m_bronze = 0.0f;

    // The results: the tally's steps shown so far, the time since the finish, this session's best.
    private int m_tallyStep = 0;
    private float m_tallyTime = 0.0f;
    private float m_best = -1.0f;

    void launch()
    {
        ui::push(kHudDoc);
        showGates();
        showGems();
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
            return;
        }
        updateResults(dt);
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

    void onGemRegistered(int value)
    {
        m_gems += value;
        showGems();
    }

    void onGemCollected(int value)
    {
        m_taken += value;
        showGems();
    }

    void onMedalGold(int hundredths) { m_gold = float(hundredths) / 100.0f; }
    void onMedalSilver(int hundredths) { m_silver = float(hundredths) / 100.0f; }
    void onMedalBronze(int hundredths) { m_bronze = float(hundredths) / 100.0f; }

    void onRunFinished(int unused)
    {
        m_running = false;
        m_tallyStep = 0;
        m_tallyTime = 0.0f;
        ui::push(kFinishDoc);
    }

    // ---- the results: one row at a time, then the final time and its medal, then the score ----
    private void updateResults(float dt)
    {
        m_tallyTime += dt;
        if (m_tallyStep < kTallySteps)
        {
            if (Input::wasPressed("Jump"))
            {
                // Skip to the end: everything at once, quietly.
                while (m_tallyStep < kTallySteps)
                {
                    tallyStep(m_tallyStep, true);
                    m_tallyStep += 1;
                }
                return;
            }
            while (m_tallyStep < kTallySteps && m_tallyTime >= kTallyFirst + kTallyGap * float(m_tallyStep))
            {
                tallyStep(m_tallyStep, false);
                m_tallyStep += 1;
            }
            return;
        }
        if (Input::wasPressed("Jump"))
        {
            ui::pop(); // the results
            restart();
        }
    }

    // Steps 0-3 are the rows, 4 the final time with its medal, 5 the score and the best.
    private void tallyStep(int step, bool quiet)
    {
        float total = m_time + m_penalty;
        if (step <= 3)
        {
            string key;
            string value;
            if (step == 0)
            {
                key = "ride";
                value = clock(m_time);
            }
            else if (step == 1)
            {
                key = "gates";
                value = "" + m_passed + " / " + m_gates
                        + (m_missed == 0 ? "" : "    +" + int(m_penalty) + " s");
            }
            else if (step == 2)
            {
                key = "gems";
                value = "" + m_taken + " / " + m_gems + "    +" + (m_taken * kGemPoints);
            }
            else
            {
                key = "bonus";
                value = "+" + timeBonus(total);
            }
            View@ row = ui::find("row-" + key);
            row.setVisible(true);
            ui::findLabel("value-" + key).setText(value);
            if (!quiet)
            {
                row.setOpacity(0.0f);
                row.fadeTo(1.0f, 0.2f);
                row.setTranslation(-24.0f, 0.0f);
                row.moveTo(0.0f, 0.0f, 0.25f, Ease::Out);
                Audio::playOneShot(kChime, AudioBus::Effects, 0.3f, 0.8f + 0.08f * float(step));
            }
            return;
        }
        if (step == 4)
        {
            ui::findLabel("final-time").setText(clock(total));
            string medal = medalName(total);
            ui::findLabel("final-medal").setText(medalLine(total));
            if (medal == "")
            {
                ui::find("medal-none").setVisible(true);
            }
            else
            {
                View@ disc = ui::find("medal-" + medal);
                disc.setVisible(true);
                if (!quiet)
                {
                    disc.setScale(1.6f);
                    disc.scaleTo(1.0f, 0.3f, Ease::Out);
                    Audio::playOneShot(kChime, AudioBus::Effects, 0.6f, 0.6f);
                }
            }
            return;
        }
        ui::find("row-score").setVisible(true);
        ui::findLabel("final-score").setText("" + (m_taken * kGemPoints + timeBonus(total)));
        bool newBest = m_best < 0.0f || total < m_best;
        string best = newBest ? (m_best < 0.0f ? "" : "New best!  (was " + clock(m_best) + ")")
                              : "Best " + clock(m_best);
        if (newBest)
        {
            m_best = total;
        }
        ui::findLabel("final-best").setText(best);
        ui::findLabel("final-prompt").setText("Jump to ride again");
    }

    // "gold", "silver", "bronze", or "" for none.
    private string medalName(float total)
    {
        if (m_gold > 0.0f && total <= m_gold)
        {
            return "gold";
        }
        if (m_silver > 0.0f && total <= m_silver)
        {
            return "silver";
        }
        if (m_bronze > 0.0f && total <= m_bronze)
        {
            return "bronze";
        }
        return "";
    }

    // The medal earned and the next one's time to beat.
    private string medalLine(float total)
    {
        if (m_bronze <= 0.0f)
        {
            return "";
        }
        string medal = medalName(total);
        if (medal == "gold")
        {
            return "Gold";
        }
        if (medal == "silver")
        {
            return "Silver.  Gold at " + clock(m_gold);
        }
        if (medal == "bronze")
        {
            return "Bronze.  Silver at " + clock(m_silver);
        }
        return "No medal.  Bronze at " + clock(m_bronze);
    }

    // Points for each second under the bronze time.
    private int timeBonus(float total)
    {
        float under = m_bronze - total;
        return under > 0.0f ? int(under * float(kBonusPerSecond)) : 0;
    }

    private void restart()
    {
        m_running = true;
        m_time = 0.0f;
        m_penalty = 0.0f;
        m_passed = 0;
        m_missed = 0;
        m_flash = 0.0f;
        m_taken = 0;
        ui::find("hud-penalty").setVisible(false);
        showGates();
        showGems();
        run::events().emit("RunRestart", 0); // in a run the scene bus is the run bus
    }

    private void showGates()
    {
        ui::findLabel("hud-gates").setText("" + m_passed + " / " + m_gates);
    }

    private void showGems()
    {
        ui::findLabel("hud-gems").setText("" + m_taken + " / " + m_gems);
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
