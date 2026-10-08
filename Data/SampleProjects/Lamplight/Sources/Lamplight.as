// Lamplight - the game's orchestrator (the reserved class Game): the title and its levels, and in
// a level the clock, the HUD's target and loot, the pause menu, and the results.
//
// The title lists the levels with the best taken on each (from the save); a level opens once the
// one before it is done. Picking one loads its scene afresh under the HUD. In a level the scene's
// pieces report what happens: loot taken ("Loot", its value), the target taken ("Taken"), a guard
// giving chase ("Spotted"), the thief caught ("Caught": he is back at his checkpoint, the level
// goes on), and the thief at the exit ("Escaped"): with the target, the level is done; without
// it, the HUD says what he is there for. Pause (Escape or Start) holds the level still under the
// pause menu.
//
// The results: the time, the loot, how often seen and caught, and the ghost medal for a level done
// without a guard ever giving chase. The save keeps, per level, that it is done, the best time,
// the most loot and whether it was ghosted ("done.<level>", "best.<level>", "loot.<level>",
// "ghost.<level>").

const int kLevelCount = 4;
Guid kTitleDoc = Guid("8d1f7418-513a-4730-b043-96d9a9150585");
Guid kHudDoc = Guid("b7c6bea8-3a7d-454b-9aeb-a2a178e5693e");
Guid kPauseDoc = Guid("f40800e3-4aed-4158-94d8-fb49fa0e68d0");
Guid kResultsDoc = Guid("3f2573fe-e52a-4306-a626-92d5c7840341");

class Game
{
    private bool m_inLevel = false;  // a level is being played (not the title, not the results)
    private bool m_paused = false;
    private int m_level = 0;
    private float m_time = 0.0f;
    private int m_loot = 0;
    private bool m_haveTarget = false;
    private int m_seen = 0;
    private int m_caught = 0;
    private float m_note = 0.0f; // how long the HUD's word shows yet

    void launch()
    {
        showTitle();
    }

    // ---- the levels: their scenes, names and what each is for ----
    private Guid levelScene(int i)
    {
        if (i == 0) return Guid("75fcbc4e-0e3d-43c6-b143-a37ae6f887cf");
        if (i == 1) return Guid("dfd95986-d233-4894-bee9-476f3c9ea9cf");
        if (i == 2) return Guid("04933472-8d3a-4a3c-943d-ad9f45fe86e7");
        return Guid("d44b322e-9d97-4d77-ae11-5f6197386458");
    }
    private string levelName(int i)
    {
        return i == 0 ? "Gardens" : (i == 1 ? "StableYard" : (i == 2 ? "GroundFloor" : "Cellars"));
    }
    private string levelTitle(int i)
    {
        return i == 0 ? "The Gardens" : (i == 1 ? "The Stable Yard" : (i == 2 ? "The Ground Floor" : "The Cellars"));
    }
    private string targetName(int i)
    {
        return i == 0 ? "the gardener's key"
                      : (i == 1 ? "the stable ledger" : (i == 2 ? "the butler's keys" : "the steward's letter"));
    }

    // A level opens once the one before it is done; the first is always open, one not built yet never.
    private bool unlocked(int i)
    {
        if (levelScene(i).IsNil())
        {
            return false;
        }
        return i == 0 || Save::getInt("done." + levelName(i - 1), 0) == 1;
    }

    private string bestLine(int i)
    {
        if (levelScene(i).IsNil())
        {
            return "Not open yet";
        }
        if (!unlocked(i))
        {
            return "Locked: get away from " + levelTitle(i - 1);
        }
        float best = Save::getFloat("best." + levelName(i), -1.0f);
        if (best < 0.0f)
        {
            return "Not yet done";
        }
        string line = "best " + clock(best) + "   loot " + Save::getInt("loot." + levelName(i), 0);
        return Save::getInt("ghost." + levelName(i), 0) == 1 ? line + "   ghost" : line;
    }

    // ---- the title ----
    private void showTitle()
    {
        m_inLevel = false;
        m_paused = false;
        run::setTimeScale(0.0f); // whatever is behind the title holds still
        ui::clear();
        Screen@ s = ui::push(kTitleDoc);
        s.findButton("level-0").onClick(Action(this.onLevel0));
        s.findButton("level-1").onClick(Action(this.onLevel1));
        s.findButton("level-2").onClick(Action(this.onLevel2));
        s.findButton("level-3").onClick(Action(this.onLevel3));
        s.findButton("quit-btn").onClick(Action(this.onQuit));
        for (int i = 0; i < kLevelCount; ++i)
        {
            s.findButton("level-" + i).setEnabled(unlocked(i));
            s.findLabel("level-" + i + "-best").setText(bestLine(i));
        }
    }

    private void onLevel0() { startLevel(0); }
    private void onLevel1() { startLevel(1); }
    private void onLevel2() { startLevel(2); }
    private void onLevel3() { startLevel(3); }
    private void onQuit() { run::requestExit(0); }

    // ---- a level ----
    private void startLevel(int i)
    {
        m_level = i;
        m_time = 0.0f;
        m_loot = 0;
        m_haveTarget = false;
        m_seen = 0;
        m_caught = 0;
        m_note = 0.0f;
        m_paused = false;
        m_inLevel = true;
        ui::clear();
        ui::push(kHudDoc);
        showHud();
        run::setTimeScale(1.0f);
        run::loadScene(levelScene(i));
    }

    void update(float dt)
    {
        if (!m_inLevel)
        {
            return;
        }
        if (Input::wasPressed("Pause"))
        {
            if (m_paused)
            {
                onResume();
            }
            else
            {
                pause();
            }
            return;
        }
        if (m_paused)
        {
            return;
        }
        m_time += dt;
        ui::findLabel("hud-time").setText(clock(m_time));
        if (m_note > 0.0f)
        {
            m_note -= dt;
            if (m_note <= 0.0f)
            {
                ui::find("hud-note").fadeTo(0.0f, 0.5f);
            }
        }
    }

    private void showHud()
    {
        ui::findLabel("hud-target").setText(m_haveTarget ? "Got " + targetName(m_level) + ": get out"
                                                         : "Find " + targetName(m_level));
        ui::findLabel("hud-loot").setText("Loot " + m_loot);
    }

    private void note(string text)
    {
        Label@ label = ui::findLabel("hud-note");
        label.setText(text);
        label.setOpacity(1.0f);
        label.setVisible(true);
        m_note = 2.5f;
    }

    // ---- what the level's pieces report ----
    void onLoot(int value)
    {
        m_loot += value;
        showHud();
    }

    void onTaken(int value)
    {
        m_loot += value;
        m_haveTarget = true;
        showHud();
        note("Got " + targetName(m_level));
    }

    void onSpotted(int unused)
    {
        m_seen++;
    }

    void onCaught(Float3 at)
    {
        m_caught++;
        note("Caught: back to the checkpoint");
    }

    void onEscaped(Float3 at)
    {
        if (!m_inLevel)
        {
            return;
        }
        if (!m_haveTarget)
        {
            note("Not without " + targetName(m_level));
            return;
        }
        finish();
    }

    // ---- the pause menu ----
    private void pause()
    {
        m_paused = true;
        run::setTimeScale(0.0f);
        Screen@ s = ui::push(kPauseDoc);
        s.findButton("resume-btn").onClick(Action(this.onResume));
        s.findButton("restart-btn").onClick(Action(this.onRestart));
        s.findButton("title-btn").onClick(Action(this.onTitle));
    }

    private void onResume()
    {
        if (!m_paused)
        {
            return;
        }
        m_paused = false;
        ui::pop();
        run::setTimeScale(1.0f);
    }

    private void onRestart() { startLevel(m_level); }
    private void onTitle() { showTitle(); }

    // ---- the results ----
    private void finish()
    {
        m_inLevel = false;
        run::setTimeScale(0.0f);
        string level = levelName(m_level);
        bool ghost = m_seen == 0;
        float previous = Save::getFloat("best." + level, -1.0f);
        Save::setInt("done." + level, 1);
        if (previous < 0.0f || m_time < previous)
        {
            Save::setFloat("best." + level, m_time);
        }
        if (m_loot > Save::getInt("loot." + level, 0))
        {
            Save::setInt("loot." + level, m_loot);
        }
        if (ghost)
        {
            Save::setInt("ghost." + level, 1);
        }
        Save::flush();

        Screen@ s = ui::push(kResultsDoc);
        s.findLabel("res-title").setText(ghost ? "Away like a ghost" : (m_caught == 0 ? "Away clean" : "Away"));
        s.findLabel("res-time").setText("Time   " + clock(m_time));
        s.findLabel("res-loot").setText("Loot   " + m_loot);
        s.findLabel("res-seen").setText("Seen " + m_seen + (m_seen == 1 ? " time" : " times") + ", caught " + m_caught);
        s.findLabel("res-medal").setText(ghost ? "Ghost: never seen" : "");
        s.findLabel("res-best").setText("Best on " + levelTitle(m_level) + ": " + clock(Save::getFloat("best." + level, m_time))
                                        + ", loot " + Save::getInt("loot." + level, m_loot));
        bool next = m_level + 1 < kLevelCount && unlocked(m_level + 1);
        s.findButton("next-btn").setEnabled(next);
        s.findButton("next-btn").onClick(Action(this.onNext));
        s.findButton("again-btn").onClick(Action(this.onRestart));
        s.findButton("title-btn").onClick(Action(this.onTitle));
    }

    private void onNext() { startLevel(m_level + 1); }

    private string clock(float seconds)
    {
        int s = int(seconds);
        int m = s / 60;
        s = s % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }
}
