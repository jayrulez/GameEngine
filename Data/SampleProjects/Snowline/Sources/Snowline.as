// Snowline - the run's orchestrator (the reserved class Game): the clock, the gates and their
// penalties, the gems, the HUD, and the results.
//
// The gates announce themselves ("GateRegistered") and report each crossing ("GatePassed",
// "GateMissed": round the outside costs kMissPenalty seconds); the finish line holds the course's
// medal times ("MedalGold", "MedalSilver", "MedalBronze", in hundredths) and ends the run
// ("RunFinished"). The clock starts when the rider does ("RunStarted", from Board.as), with the
// medal ghosts, not while the scene is still coming in. The gems announce themselves too ("GemRegistered") and report being taken
// ("GemCollected"), each with how many it counts for. The board announces each trick as it lands
// ("TrickAir", "TrickSpin", "TrickGrab", then "TrickLanded"): a clean one scores its air, its half
// turns and its grab, times the combo, which a clean landing raises and a crash or a missed gate
// ends.
//
// The results tally the run a row at a time (the ride, the gates, the gems, the tricks, the time
// bonus), then
// land the final time with the medal it earned, the score and the course's best. The best time and
// the best medal are kept in the save ("best.<scene>", "medal.<scene>"), and a run that beats the
// best is announced ("NewBest", in hundredths) so PlayerGhost.as keeps it as the course's ghost. Jump
// skips to the end, then starts the next run ("RunRestart" puts the rider back at the top, the
// gates back to waiting and the gems back in place).

Guid kHudDoc = Guid("9ff3ed63-f33c-487a-a88f-2761d48af746");    // UI/Hud
Guid kFinishDoc = Guid("409e4cea-20a2-4705-9a54-059fa662850f"); // UI/Finish

// Audio/GemChime (sounds.py): the tally's tick, pitched up row by row, and lower as a medal lands.
Guid kChime = Guid("c47f1131-6df4-49ea-b2c7-6e3b3316a3ce");

const float kMissPenalty = 2.0f;
const int kGemPoints = 100;       // a gem's worth
const int kBonusPerSecond = 100;  // each second under the bronze time
const int kTallySteps = 7;        // the five rows, then the final time and medal, then the score
const int kAirPoints = 200;       // a second in the air
const int kSpinPoints = 250;      // a half turn
const int kGrabPoints = 400;      // a second holding a grab
const int kComboMost = 5;         // the highest combo multiplier
const float kTrickShown = 1.6f;   // how long a landed trick shows (s)
const float kTallyFirst = 0.5f;   // the first row's moment after the finish (s)
const float kTallyGap = 0.45f;    // between the rows (s)

class Game
{
    private bool m_running = true;
    private bool m_started = false; // the rider's first frame has come (the clock runs from it)
    private float m_time = 0.0f;    // seconds on the clock this run
    private float m_penalty = 0.0f; // seconds added for missed gates
    private int m_gates = 0;        // how many the course has
    private int m_passed = 0;
    private int m_missed = 0;
    private float m_flash = 0.0f;   // how long the "+2 s" shows yet
    private int m_gems = 0;         // how many the course has
    private int m_taken = 0;

    // Tricks: the one landing now (its parts as the board reports them), the combo, the points.
    private float m_trickAir = 0.0f;
    private int m_trickSpin = 0;
    private float m_trickGrab = 0.0f;
    private int m_combo = 1;
    private int m_tricks = 0;        // trick points this run
    private float m_trickShow = 0.0f; // how long the landed trick shows yet

    // The course's medal times (s, penalties included), from the finish line.
    private float m_gold = 0.0f;
    private float m_silver = 0.0f;
    private float m_bronze = 0.0f;

    // The results: the tally's steps shown so far, the time since the finish, and the course's best
    // before this run (-1 for none).
    private int m_tallyStep = 0;
    private float m_tallyTime = 0.0f;
    private float m_previousBest = -1.0f;

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
            if (!m_started)
            {
                return;
            }
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
            if (m_trickShow > 0.0f)
            {
                m_trickShow -= run::realDeltaTime();
                if (m_trickShow <= 0.0f)
                {
                    ui::find("hud-trick").setVisible(false);
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
        endCombo();
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

    void onTrickAir(int centiseconds) { m_trickAir = float(centiseconds) / 100.0f; }
    void onTrickSpin(int degrees) { m_trickSpin = degrees; }
    void onTrickGrab(int centiseconds) { m_trickGrab = float(centiseconds) / 100.0f; }

    // A trick landed: clean, it scores and the combo grows; crashed, the combo is gone.
    void onTrickLanded(int clean)
    {
        if (!m_running)
        {
            return;
        }
        if (clean == 0)
        {
            endCombo();
            showTrick("Crash", "");
            return;
        }
        int points = int(m_trickAir * float(kAirPoints)) + (m_trickSpin / 180) * kSpinPoints
                     + int(m_trickGrab * float(kGrabPoints));
        points *= m_combo;
        m_tricks += points;
        showTrick(trickName(), "+" + points);
        if (m_combo < kComboMost)
        {
            m_combo += 1;
        }
        View@ combo = ui::find("hud-combo");
        ui::findLabel("hud-combo").setText("Combo x" + m_combo);
        combo.setVisible(true);
    }

    // "360 Grab", "180", "Air": the spin if any, then the grab if held a while, else just air.
    private string trickName()
    {
        string name = m_trickSpin > 0 ? "" + m_trickSpin : "";
        if (m_trickGrab >= 0.2f)
        {
            name += (name == "" ? "" : " ") + "Grab";
        }
        return name == "" ? "Air" : name;
    }

    private void showTrick(string name, string points)
    {
        Label@ trick = ui::findLabel("hud-trick");
        trick.setText(points == "" ? name : name + "   " + points);
        trick.setVisible(true);
        trick.setOpacity(0.0f);
        trick.fadeTo(1.0f, 0.15f);
        m_trickShow = kTrickShown;
    }

    private void endCombo()
    {
        m_combo = 1;
        ui::find("hud-combo").setVisible(false);
    }

    void onMedalGold(int hundredths) { m_gold = float(hundredths) / 100.0f; }
    void onMedalSilver(int hundredths) { m_silver = float(hundredths) / 100.0f; }
    void onMedalBronze(int hundredths) { m_bronze = float(hundredths) / 100.0f; }

    void onRunStarted(int unused)
    {
        m_started = true;
    }

    void onRunFinished(int unused)
    {
        m_running = false;
        m_tallyStep = 0;
        m_tallyTime = 0.0f;
        keepBest(m_time + m_penalty);
        ui::push(kFinishDoc);
    }

    // The course's best time and medal, kept in the save; a new best is announced at once, so the
    // ghost keeps this run before the next one starts recording.
    private void keepBest(float total)
    {
        string course = run::currentScene().name();
        m_previousBest = Save::getFloat("best." + course, -1.0f);
        if (m_previousBest < 0.0f || total < m_previousBest)
        {
            Save::setFloat("best." + course, total);
            run::events().emit("NewBest", int(total * 100.0f + 0.5f));
        }
        int rank = medalRank(medalName(total));
        if (rank > Save::getInt("medal." + course, 0))
        {
            Save::setInt("medal." + course, rank);
        }
        Save::flush();
    }

    // 3 gold, 2 silver, 1 bronze, 0 none.
    private int medalRank(string medal)
    {
        if (medal == "gold")
        {
            return 3;
        }
        if (medal == "silver")
        {
            return 2;
        }
        return medal == "bronze" ? 1 : 0;
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

    // Steps 0-4 are the rows, 5 the final time with its medal, 6 the score and the best.
    private void tallyStep(int step, bool quiet)
    {
        float total = m_time + m_penalty;
        if (step <= 4)
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
            else if (step == 3)
            {
                key = "tricks";
                value = "+" + m_tricks;
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
        if (step == 5)
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
        ui::findLabel("final-score").setText("" + (m_taken * kGemPoints + m_tricks + timeBonus(total)));
        string best;
        if (m_previousBest < 0.0f)
        {
            best = "Your ghost will ride this run";
        }
        else if (total < m_previousBest)
        {
            best = "New best!  (was " + clock(m_previousBest) + ")";
        }
        else
        {
            best = "Best " + clock(m_previousBest);
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
        m_tricks = 0;
        m_trickShow = 0.0f;
        endCombo();
        ui::find("hud-trick").setVisible(false);
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
