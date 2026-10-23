// Masquerade - the game's orchestrator (the reserved class Game): the title, a round at the ball
// under its HUD, the pause menu, and the results.
//
// The title picks a map, then plays it alone, hosts it (the server on `kPort`, then the map) or
// joins a host (the map first, then the connection, so the map's networked entities are in place
// for the host's first snapshot). The round itself is the scene's (Ball.as): this hears it. "Hud" (four times a second: your
// points, the seconds left, your target's costume, your place), "Note" (something happened to
// you, or a mask flared), "RoundOver" (everyone's points) and "RoundStart" (the host began another
// round). Pause (Escape or Start) shows the pause menu, holding the round still when playing alone
// (a networked round goes on). The title picks the map; the save keeps each map's best round
// ("best.Palace", "best.Canal", "best.Gardens").

const float kControlsShown = 7.0f; // how long the controls show as a round starts (s)
Guid kTitleDoc = Guid("{{UI:Title}}");
Guid kHudDoc = Guid("{{UI:Hud}}");
Guid kPauseDoc = Guid("{{UI:Pause}}");
Guid kResultsDoc = Guid("{{UI:Results}}");
Guid kPalace = Guid("{{Scene:Palace}}");
Guid kCanal = Guid("{{Scene:Canal}}");
Guid kGardens = Guid("{{Scene:Gardens}}");
// sounds.py's: what the round says to the player, and the buttons.
Guid kTagSound = Guid("{{Audio:Tag}}");
Guid kMissSound = Guid("{{Audio:Miss}}");
Guid kFlareSound = Guid("{{Audio:Flare}}");
Guid kChimeSound = Guid("{{Audio:Chime}}");
Guid kFanfare = Guid("{{Audio:Fanfare}}");
Guid kClick = Guid("{{Audio:Click}}");

// Ball.as's notes.
const int kNoteGotTarget = 1;
const int kNoteMissed = 2;
const int kNoteTagged = 3;
const int kNoteFlare = 4;
const int kNoteWrongMap = 5;
const int kNoteFull = 6;
const int kPort = 7777;
const int kAlone = 0;
const int kHosting = 1;
const int kJoining = 2;

class Game
{
    private bool m_inRound = false;
    private bool m_paused = false;
    private float m_note = 0.0f;         // how long the HUD's word shows yet
    private float m_controlsShow = 0.0f; // how long the controls panel shows yet
    private int m_map = 0;               // 0 the Palace, 1 the Canal, 2 the Gardens
    private int m_mode = kAlone;
    private string m_address = "127.0.0.1";
    private float m_connectIn = -1.0f;   // a joiner: until it connects, once its map has loaded

    void launch()
    {
        showTitle();
    }

    // ---- the title ----
    private void showTitle()
    {
        m_inRound = false;
        m_paused = false;
        run::setTimeScale(0.0f); // whatever is behind the title holds still
        ui::clear();
        UIScreen@ s = ui::push(kTitleDoc);
        s.findButton("play-btn").onClick(Action(this.onPalace));
        s.findButton("canal-btn").onClick(Action(this.onCanal));
        s.findButton("solo-btn").onClick(Action(this.onSolo));
        s.findButton("host-btn").onClick(Action(this.onHost));
        s.findButton("join-btn").onClick(Action(this.onJoin));
        s.findLabel("map-choice").setText("The map: " + mapTitle(m_map));
        s.findLabel("net-status").setText("");
        s.findButton("canal-btn").setEnabled(!kCanal.IsNil());
        s.findButton("gardens-btn").onClick(Action(this.onGardens));
        s.findButton("gardens-btn").setEnabled(!kGardens.IsNil());
        s.findButton("quit-btn").onClick(Action(this.onQuit));
        s.findLabel("palace-best").setText(bestLine(0));
        s.findLabel("canal-best").setText(bestLine(1));
        s.findLabel("gardens-best").setText(bestLine(2));
    }

    private void onPalace() { choose(0); }
    private void onCanal() { choose(1); }
    private void onGardens() { choose(2); }

    private void choose(int map)
    {
        sound(kClick);
        m_map = map;
        ui::findLabel("map-choice").setText("The map: " + mapTitle(map));
    }

    private string mapTitle(int map)
    {
        return map == 0 ? "the Palace" : (map == 1 ? "the Canal" : "the Gardens");
    }

    private void onSolo()
    {
        sound(kClick);
        m_mode = kAlone;
        startRound();
    }

    private void onHost()
    {
        sound(kClick);
        if (!Net::startServer(kPort))
        {
            ui::findLabel("net-status").setText("Could not host on port " + kPort);
            return;
        }
        m_mode = kHosting;
        startRound();
    }

    private void onJoin()
    {
        sound(kClick);
        UITextBox@ box = ui::findTextBox("address");
        m_address = (box !is null && box.text.length() > 0) ? box.text : "127.0.0.1";
        m_mode = kJoining;
        startRound();
        m_connectIn = 0.2f; // once the map has loaded
    }

    private string mapName(int map) { return map == 0 ? "Palace" : (map == 1 ? "Canal" : "Gardens"); }

    private string bestLine(int map)
    {
        int best = Save::getInt("best." + mapName(map), -100000);
        return best == -100000 ? "No round played yet" : "Best round: " + best + " points";
    }
    private void onQuit()
    {
        sound(kClick);
        run::requestExit(0);
    }

    // ---- a round ----
    private void startRound()
    {
        m_paused = false;
        m_inRound = true;
        m_note = 0.0f;
        ui::clear();
        ui::push(kHudDoc);
        ui::findLabel("hud-target").setText("");
        ui::findLabel("hud-score").setText("Points 0");
        UIView@ controls = ui::find("hud-controls");
        controls.setVisible(true);
        controls.setOpacity(0.0f);
        controls.fadeTo(1.0f, 0.3f);
        m_controlsShow = kControlsShown;
        run::setTimeScale(1.0f);
        run::loadScene(m_map == 0 ? kPalace : (m_map == 1 ? kCanal : kGardens));
    }

    void update(float dt)
    {
        if (!m_inRound)
        {
            return;
        }
        if (m_connectIn >= 0.0f && run::sceneReady())
        {
            m_connectIn -= dt; // a moment more, for the scene's first frame
            if (m_connectIn < 0.0f && !Net::connect(m_address, kPort))
            {
                note("Could not reach " + m_address);
            }
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
        if (m_controlsShow > 0.0f)
        {
            m_controlsShow -= dt;
            if (m_controlsShow <= 0.0f)
            {
                ui::find("hud-controls").fadeTo(0.0f, 0.6f);
            }
        }
        if (m_note > 0.0f)
        {
            m_note -= dt;
            if (m_note <= 0.0f)
            {
                ui::find("hud-note").fadeTo(0.0f, 0.5f);
            }
        }
    }

    // ---- what the round reports ----
    void onHud(Float4 hud)
    {
        if (!m_inRound)
        {
            return;
        }
        UILabel@ mark = ui::findLabel("hud-target");
        mark.setText(costumeName(int(hud.z)));
        mark.setTextColor(costumeColour(int(hud.z)));
        ui::findLabel("hud-score").setText("Points " + int(hud.x));
        ui::findLabel("hud-time").setText(clock(hud.y));
        ui::findLabel("hud-rank").setText(place(int(hud.w)) + " of 4");
    }

    void onNote(int what)
    {
        if (!m_inRound)
        {
            return;
        }
        if (what == kNoteGotTarget)
        {
            sound(kTagSound);
            note("Your mark is tagged. They will be back, dressed anew");
        }
        else if (what == kNoteMissed)
        {
            sound(kMissSound);
            note("Wrong guest! Your mask flares");
        }
        else if (what == kNoteTagged)
        {
            sound(kMissSound);
            note("Tagged! A new costume, somewhere else");
        }
        else if (what == kNoteFlare)
        {
            sound(kFlareSound);
            note("A mask flares: a hunter, there");
        }
        else if (what == kNoteWrongMap)
        {
            sound(kChimeSound);
            note("The host is at another map: choose it and join again");
        }
        else if (what == kNoteFull)
        {
            sound(kChimeSound);
            note("Every seat is taken: you are watching");
        }
    }

    void onRoundOver(Float4 scores)
    {
        if (!m_inRound)
        {
            return;
        }
        m_inRound = false;
        holdStill();
        sound(kFanfare);
        int you = int(scores.x);
        string key = "best." + mapName(m_map);
        int best = Save::getInt(key, -100000);
        bool record = you > best;
        if (record)
        {
            Save::setInt(key, you);
            Save::flush();
        }
        int place = 1;
        if (scores.y > scores.x) ++place;
        if (scores.z > scores.x) ++place;
        if (scores.w > scores.x) ++place;
        UIScreen@ s = ui::push(kResultsDoc);
        s.findLabel("res-title").setText(place == 1 ? "The ball is yours" : "The music stops");
        s.findLabel("res-you").setText("You came " + this.place(place) + " with " + you + " points");
        s.findLabel("res-0").setText("You        " + you);
        s.findLabel("res-1").setText("Hunter 1   " + int(scores.y));
        s.findLabel("res-2").setText("Hunter 2   " + int(scores.z));
        s.findLabel("res-3").setText("Hunter 3   " + int(scores.w));
        s.findLabel("res-best").setText(record ? "Your best round yet" : "Your best: " + best + " points");
        s.findButton("again-btn").onClick(Action(this.onAgain));
        s.findButton("again-btn").setVisible(m_mode != kJoining); // the host starts the next round
        s.findButton("title-btn").onClick(Action(this.onTitle));
    }

    // The host started another round in the same scene: a joiner leaves its results for it.
    void onRoundStart(int unused)
    {
        if (m_inRound || m_mode != kJoining)
        {
            return;
        }
        enterRound();
    }

    // ---- the pause menu ----
    private void pause()
    {
        m_paused = true;
        holdStill();
        m_controlsShow = 0.0f;
        ui::find("hud-controls").setVisible(false); // the menu shows them itself
        UIScreen@ s = ui::push(kPauseDoc);
        s.findButton("resume-btn").onClick(Action(this.onResume));
        s.findButton("restart-btn").onClick(Action(this.onAgain));
        s.findButton("restart-btn").setVisible(m_mode != kJoining); // the host's to start over
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
        sound(kClick);
        letGo();
    }

    // Another round: alone, the map afresh; hosting, the same scene goes on (the players' hunters
    // with it) and the Ball starts over ("Restart").
    private void onAgain()
    {
        sound(kClick);
        if (m_mode == kHosting)
        {
            enterRound();
            run::currentScene().events.emit("Restart", 0); // Ball.as starts over
            return;
        }
        startRound();
    }

    // Alone, the scene stops behind a menu. In a networked round it goes on (the host runs the
    // world for everyone, and a joiner's scene must keep hearing the host: a new round, say), so
    // the player hears "Menu" instead and stands still under it.
    private void holdStill()
    {
        if (m_mode == kAlone)
        {
            run::setTimeScale(0.0f);
        }
        else
        {
            run::currentScene().events.emit("Menu", 1);
        }
    }

    private void letGo()
    {
        run::setTimeScale(1.0f);
        if (m_mode != kAlone)
        {
            run::currentScene().events.emit("Menu", 0);
        }
    }

    // The HUD over the scene that is already running.
    private void enterRound()
    {
        ui::clear();
        ui::push(kHudDoc);
        m_inRound = true;
        m_paused = false;
        m_note = 0.0f;
        letGo();
    }

    private void onTitle()
    {
        sound(kClick);
        if (m_mode != kAlone)
        {
            Net::disconnect();
            m_mode = kAlone;
        }
        m_connectIn = -1.0f;
        showTitle();
    }

    private void sound(Guid clip)
    {
        Audio::playOneShot(clip, AudioBus::Effects, 0.8f);
    }

    private void note(string text)
    {
        UILabel@ label = ui::findLabel("hud-note");
        label.setText(text);
        label.setOpacity(1.0f);
        label.setVisible(true);
        m_note = 3.0f;
    }

    // In blender/guest.py's costume order, which the crowd and the looks follow.
    private string costumeName(int costume)
    {
        if (costume == 0) return "a rose gown";
        if (costume == 1) return "a teal gown";
        if (costume == 2) return "a gold gown";
        if (costume == 3) return "a black coat";
        if (costume == 4) return "a plum coat";
        return "an ivory coat";
    }

    // Each costume's colour, lightened enough to read on the HUD's night blue (the black coat as
    // a silver grey).
    private Color costumeColour(int costume)
    {
        if (costume == 0) return Color(0.95f, 0.45f, 0.55f, 1.0f);
        if (costume == 1) return Color(0.35f, 0.85f, 0.85f, 1.0f);
        if (costume == 2) return Color(0.98f, 0.80f, 0.35f, 1.0f);
        if (costume == 3) return Color(0.78f, 0.80f, 0.86f, 1.0f);
        if (costume == 4) return Color(0.85f, 0.55f, 0.90f, 1.0f);
        return Color(1.0f, 0.96f, 0.86f, 1.0f);
    }

    private string place(int p)
    {
        return p == 1 ? "1st" : (p == 2 ? "2nd" : (p == 3 ? "3rd" : "4th"));
    }

    private string clock(float seconds)
    {
        int s = int(seconds);
        int m = s / 60;
        s = s % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }
}
