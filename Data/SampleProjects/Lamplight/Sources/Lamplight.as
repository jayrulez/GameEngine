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

const int kLevelCount = 5;
const float kControlsShown = 6.0f; // how long the controls show as a level starts (s)
// The screens, on the engine GUI's screen stack: their documents by asset id (importscripts.py fills
// these in).
const string kTitleDoc = "{7c0d5336-6480-4612-b0f7-f5838873efeb}";
const string kHudDoc = "{4b6c9052-1ee3-4cd0-a04b-1fdb53c7ef9a}";
const string kPauseDoc = "{f5faae8a-fa1f-4c68-b2af-1cc8c1707afc}";
const string kResultsDoc = "{efbe6b9d-df66-4654-8044-d965602a043c}";

// A GUI event's handler (a button's click).
funcdef bool EventHandler(GuiEvent e);

ScreenStack@ screens() { return ScreenStack::of(gui::root().find("screens")); }
// The top screen first: one just cleared or popped away is still in the tree while its exit plays,
// with the same ids, and would be found first.
Node@ find(string id)
{
    Screen@ top = screens().top();
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}
void wire(string id, EventHandler@ handler) { find(id).on("click", handler); } // `on*` names are the engine's handlers
// Skips a node the screens up now do not have (a level's pieces report while the title is up).
void setText(string id, string text)
{
    Node@ n = find(id);
    if (n !is null)
    {
        Label::of(n).text = text;
    }
}

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
    private float m_controlsShow = 0.0f; // how long the controls panel shows yet

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
        if (i == 3) return Guid("d44b322e-9d97-4d77-ae11-5f6197386458");
        return Guid("78b56fe9-5031-40c9-be8f-72ce637aca3a");
    }
    private string levelName(int i)
    {
        return i == 0 ? "Gardens" : (i == 1 ? "StableYard" : (i == 2 ? "GroundFloor" : (i == 3 ? "Cellars" : "UpperFloor")));
    }
    private string levelTitle(int i)
    {
        return i == 0 ? "The Gardens"
                      : (i == 1 ? "The Stable Yard" : (i == 2 ? "The Ground Floor" : (i == 3 ? "The Cellars" : "The Upper Floor")));
    }
    private string targetName(int i)
    {
        return i == 0 ? "the gardener's key"
                      : (i == 1 ? "the stable ledger"
                                : (i == 2 ? "the butler's keys" : (i == 3 ? "the steward's letter" : "the family jewels")));
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
        screens().clear();
        screens().push(kTitleDoc);
        wire("level-0", EventHandler(this.onLevel0));
        wire("level-1", EventHandler(this.onLevel1));
        wire("level-2", EventHandler(this.onLevel2));
        wire("level-3", EventHandler(this.onLevel3));
        wire("level-4", EventHandler(this.onLevel4));
        wire("quit-btn", EventHandler(this.onQuit));
        for (int i = 0; i < kLevelCount; ++i)
        {
            find("level-" + i).enabled = unlocked(i);
            setText("level-" + i + "-best", bestLine(i));
        }
    }

    private bool onLevel0(GuiEvent e) { startLevel(0); return true; }
    private bool onLevel1(GuiEvent e) { startLevel(1); return true; }
    private bool onLevel2(GuiEvent e) { startLevel(2); return true; }
    private bool onLevel3(GuiEvent e) { startLevel(3); return true; }
    private bool onLevel4(GuiEvent e) { startLevel(4); return true; }
    private bool onQuit(GuiEvent e) { run::requestExit(0); return true; }

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
        screens().clear();
        screens().push(kHudDoc);
        find("hud-prompt-panel").visible = false; // until a lamp or a door is in reach
        showHud();
        // The controls, keyboard and pad, for the first seconds of the level (the pause menu has
        // them too): faded in from the sheet's 0.
        find("hud-controls").tween("opacity", "1", 0.3f, "ease");
        m_controlsShow = kControlsShown;
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
                resume();
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
        if (m_controlsShow > 0.0f)
        {
            m_controlsShow -= dt;
            if (m_controlsShow <= 0.0f)
            {
                find("hud-controls").tween("opacity", "0", 0.6f, "ease");
            }
        }
        setText("hud-time", clock(m_time));
        if (m_note > 0.0f)
        {
            m_note -= dt;
            if (m_note <= 0.0f)
            {
                find("hud-note").tween("opacity", "0", 0.5f, "ease");
            }
        }
    }

    private void showHud()
    {
        setText("hud-target", m_haveTarget ? "Got " + targetName(m_level) + ": get out" : "Find " + targetName(m_level));
        setText("hud-loot", "Loot " + m_loot);
    }

    private void note(string text)
    {
        Node@ label = find("hud-note");
        if (label is null)
        {
            return;
        }
        setText("hud-note", text);
        label.tween("opacity", "1", 0.0f, "linear");
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

    void onAlarm(Float3 at)
    {
        note("The alarm! Every guard is coming");
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
        m_controlsShow = 0.0f;
        find("hud-controls").tween("opacity", "0", 0.0f, "linear"); // the menu shows them itself
        screens().push(kPauseDoc);
        wire("resume-btn", EventHandler(this.onResume));
        wire("restart-btn", EventHandler(this.onRestart));
        wire("title-btn", EventHandler(this.onTitle));
    }

    private bool onResume(GuiEvent e)
    {
        resume();
        return true;
    }

    private void resume()
    {
        if (!m_paused)
        {
            return;
        }
        m_paused = false;
        screens().pop();
        run::setTimeScale(1.0f);
    }

    private bool onRestart(GuiEvent e) { startLevel(m_level); return true; }
    private bool onTitle(GuiEvent e) { showTitle(); return true; }

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

        screens().push(kResultsDoc);
        bool last = m_level + 1 >= kLevelCount;
        string title = ghost ? "Away like a ghost" : (m_caught == 0 ? "Away clean" : "Away");
        setText("res-title", last ? "The jewels are yours. " + title : title);
        setText("res-time", "Time   " + clock(m_time));
        setText("res-loot", "Loot   " + m_loot);
        setText("res-seen", "Seen " + m_seen + (m_seen == 1 ? " time" : " times") + ", caught " + m_caught);
        setText("res-medal", last ? "The Lamplight heist is done" + (ghost ? ". Ghost: never seen" : "")
                              : (ghost ? "Ghost: never seen" : ""));
        setText("res-best", "Best on " + levelTitle(m_level) + ": " + clock(Save::getFloat("best." + level, m_time))
                             + ", loot " + Save::getInt("loot." + level, m_loot));
        bool next = m_level + 1 < kLevelCount && unlocked(m_level + 1);
        find("next-btn").enabled = next;
        wire("next-btn", EventHandler(this.onNext));
        wire("again-btn", EventHandler(this.onRestart));
        wire("title-btn", EventHandler(this.onTitle));
    }

    private bool onNext(GuiEvent e) { startLevel(m_level + 1); return true; }

    private string clock(float seconds)
    {
        int s = int(seconds);
        int m = s / 60;
        s = s % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }
}
