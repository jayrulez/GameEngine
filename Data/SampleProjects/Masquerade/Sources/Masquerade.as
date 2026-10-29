// Masquerade - the game's orchestrator (the reserved class Game): the title, a round at the ball
// under its HUD, the pause menu, and the results.
//
// The title offers play alone, host, join or quit; the first two choose a map (Maps), then play it alone or host it (the server on `kPort`, waiting for each
// joiner's ready, then the map); or joins a host: the connection first, from the title, then the
// host's map (named in its welcome) loads, and the joiner says it is ready, so the map's networked
// entities are in place for the host's first snapshot. The round itself is the scene's (Ball.as): this hears it. "Hud" (four times a second: your
// points, the seconds left, your target's costume, your place), "Note" (something happened to
// you, or a mask flared), "RoundOver" (everyone's points) and "RoundStart" (the host began another
// round). Pause (Escape or Start) shows the pause menu, holding the round still when playing alone
// (a networked round goes on). The title picks the map; the save keeps each map's best round
// ("best.Palace", "best.Canal", "best.Gardens").

const float kControlsShown = 7.0f; // how long the controls show as a round starts (s)
// The screens, on the engine GUI's screen stack (pushed by their documents' ids).
const string kTitleDoc = "{2c8d8d4f-d770-496d-a766-d14cfbfcd328}";
const string kMapsDoc = "{bef19424-1fc4-4f06-b262-b49abeacf1a6}";
const string kJoinDoc = "{36b90866-3b41-4a1c-8b87-c0305859bb5d}";
const string kHudDoc = "{e183e431-f835-4289-a1eb-88d33b67db46}";
const string kPauseDoc = "{76517a0e-2810-4018-ad32-036418a2bc1c}";
const string kResultsDoc = "{d6ee5f73-8622-4f72-8f7b-1d7e43f609b8}";
Guid kPalace = Guid("20eb69af-217d-4b37-93d2-d032e63094fb");
Guid kCanal = Guid("67c261b3-8da6-4a11-bab2-c96595b651cf");
Guid kGardens = Guid("6c8abc4e-60ff-4614-864b-68fd5dc21e19");
// sounds.py's: what the round says to the player, and the buttons.
Guid kTagSound = Guid("ea04649b-1257-4914-9348-2c9369d71bb4");
Guid kMissSound = Guid("beb39d19-017a-498f-a6d0-6ebb036b5f13");
Guid kFlareSound = Guid("2eb1ef65-d86b-4806-8a02-94b50e9702ea");
Guid kChimeSound = Guid("75d06e2f-5b1f-468e-96fd-e4177e8a3a8f");
Guid kFanfare = Guid("5a0d5172-db3d-44be-8922-f9249c64fa39");
Guid kClick = Guid("98658096-7e9e-4891-9f68-7b1f7125b3a2");

// Ball.as's notes.
const int kNoteGotTarget = 1;
const int kNoteMissed = 2;
const int kNoteTagged = 3;
const int kNoteFlare = 4;
const int kNoteFull = 6;
const int kPort = 7777;
const int kAlone = 0;
const float kJoinSeconds = 8.0f; // how long a joiner waits for the host's welcome
const int kHosting = 1;
const int kJoining = 2;

// A GUI event's handler (a button's click).
funcdef bool EventHandler(GuiEvent e);

// The GUI's screen stack, and a node of the screens by its id.
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
void setText(string id, string text) { Label::of(find(id)).text = text; }

class Game
{
    private bool m_inRound = false;
    private bool m_paused = false;
    private float m_note = 0.0f;         // how long the HUD's word shows yet
    private bool m_inputHeld = false;    // the "Menu" set pushed (holdInput)
    private float m_controlsShow = 0.0f; // how long the controls panel shows yet
    private int m_map = 0;               // 0 the Palace, 1 the Canal, 2 the Gardens
    private int m_mode = kAlone;
    private string m_address = "127.0.0.1";
    private float m_readyIn = -1.0f;     // a joiner: until it tells the host it is ready, once the host's map has loaded
    private float m_joinWait = -1.0f;    // a joiner on the title: how long until it gives up on the host

    void launch()
    {
        // A joiner hears the host's map in its welcome (Ball.as's onJoined), here rather than in a
        // scene's behaviour, as it arrives while the title's scene is the one loaded.
        NetTextHandler@ map = NetTextHandler(this.onHostMap);
        Net::on("map", map);
        showTitle();
    }

    // ---- the title ----
    private void showTitle()
    {
        m_inRound = false;
        m_paused = false;
        run::setTimeScale(0.0f); // whatever is behind the title holds still
        holdInput();
        screens().clear();
        screens().push(kTitleDoc);
        wire("solo-btn", EventHandler(this.onSolo));
        wire("host-btn", EventHandler(this.onHost));
        wire("join-btn", EventHandler(this.onJoin));
        wire("quit-btn", EventHandler(this.onQuit));
    }

    // ---- the map, to play alone or to host: a card each, the chosen one lit, then Start ----
    private bool m_hostChosen = false; // the map page's Start hosts (else it plays alone)

    private void showMaps(bool host)
    {
        m_hostChosen = host;
        screens().push(kMapsDoc);
        setText("maps-heading", host ? "Host a ball" : "Play alone");
        Button::of(find("maps-start")).text = host ? "Host" : "Play";
        wire("map-0", EventHandler(this.onMap0));
        wire("map-1", EventHandler(this.onMap1));
        wire("map-2", EventHandler(this.onMap2));
        find("map-1").enabled = !kCanal.IsNil();
        find("map-2").enabled = !kGardens.IsNil();
        for (int map = 0; map < 3; ++map)
        {
            setText("map-best-" + map, bestLine(map));
        }
        wire("maps-start", EventHandler(this.onStart));
        wire("maps-back", EventHandler(this.onBack));
        setText("net-status", "");
        choose(m_map, false);
    }

    private bool onMap0(GuiEvent e) { choose(0, true); return true; }
    private bool onMap1(GuiEvent e) { choose(1, true); return true; }
    private bool onMap2(GuiEvent e) { choose(2, true); return true; }

    // The chosen card lit; a click on it chooses, Start then plays or hosts it.
    private void choose(int map, bool click)
    {
        if (click)
        {
            sound(kClick);
        }
        m_map = map;
        for (int i = 0; i < 3; ++i)
        {
            UIWidget@ card = UIWidget::of(find("map-" + i));
            if (i == map)
            {
                card.addClass("chosen");
            }
            else
            {
                card.removeClass("chosen");
            }
        }
    }

    private bool onBack(GuiEvent e)
    {
        sound(kClick);
        screens().pop();
        return true;
    }

    private string mapTitle(int map)
    {
        return map == 0 ? "the Palace" : (map == 1 ? "the Canal" : "the Gardens");
    }

    private bool onSolo(GuiEvent e)
    {
        sound(kClick);
        showMaps(false);
        return true;
    }

    private bool onHost(GuiEvent e)
    {
        sound(kClick);
        showMaps(true);
        return true;
    }

    // The map page's Start: play the chosen map alone, or host it.
    private bool onStart(GuiEvent e)
    {
        sound(kClick);
        if (!m_hostChosen)
        {
            m_mode = kAlone;
            startRound();
            return true;
        }
        if (!Net::startServer(kPort))
        {
            setText("net-status", "Could not host on port " + kPort);
            return true;
        }
        m_mode = kHosting;
        Net::waitForReady(true); // a joiner hears no state until it has loaded this map
        startRound();
        return true;
    }

    // ---- joining: the address, then Join; the map is the host's ----
    private bool onJoin(GuiEvent e)
    {
        sound(kClick);
        screens().push(kJoinDoc);
        TextField::of(find("address")).text = m_address;
        wire("join-go", EventHandler(this.onJoinGo));
        wire("join-back", EventHandler(this.onBack));
        setText("net-status", "");
        return true;
    }

    private bool onJoinGo(GuiEvent e)
    {
        sound(kClick);
        if (m_joinWait >= 0.0f)
        {
            return true; // already waiting for the host
        }
        string typed = TextField::of(find("address")).text;
        m_address = typed.length() > 0 ? typed : "127.0.0.1";
        // Connect from the title; the host's welcome names its map, which then loads (onHostMap).
        if (!Net::connect(m_address, kPort))
        {
            setText("net-status", "Could not reach " + m_address);
            return true;
        }
        m_mode = kJoining;
        m_joinWait = kJoinSeconds;
        setText("net-status", "Joining " + m_address + "...");
        return true;
    }

    // A joiner: the host's map. It loads, and once it is ready the host's state begins.
    void onHostMap(int sender, const string &in name)
    {
        if (!Net::isClient() || m_mode != kJoining || m_joinWait < 0.0f)
        {
            return;
        }
        m_joinWait = -1.0f;
        m_map = name == "Canal" ? 1 : (name == "Gardens" ? 2 : 0);
        setText("net-status", "Joining " + mapTitle(m_map) + "...");
        startRound();
        m_readyIn = 0.2f; // once the map has loaded, a moment more for its first frame
    }

    private string mapName(int map) { return map == 0 ? "Palace" : (map == 1 ? "Canal" : "Gardens"); }

    private string bestLine(int map)
    {
        int best = Save::getInt("best." + mapName(map), -100000);
        return best == -100000 ? "No round played yet" : "Best round: " + best + " points";
    }
    private bool onQuit(GuiEvent e)
    {
        sound(kClick);
        run::requestExit(0);
        return true;
    }

    // ---- a round ----
    private void startRound()
    {
        m_paused = false;
        m_inRound = true;
        m_note = 0.0f;
        screens().clear();
        screens().push(kHudDoc);
        setText("hud-target", "");
        setText("hud-score", "Points 0");
        find("hud-controls").tween("opacity", "1", 0.3f, "ease"); // from the sheet's 0
        m_controlsShow = kControlsShown;
        letGo(); // time again, and the player's input: a round may start from any menu
        run::loadScene(m_map == 0 ? kPalace : (m_map == 1 ? kCanal : kGardens));
    }

    void update(float dt)
    {
        // A joiner on the title, waiting for the host's welcome.
        if (m_joinWait >= 0.0f)
        {
            m_joinWait -= dt;
            if (m_joinWait < 0.0f)
            {
                Net::disconnect();
                m_mode = kAlone;
                setText("net-status", "Could not reach " + m_address);
            }
        }
        if (!m_inRound)
        {
            return;
        }
        if (m_readyIn >= 0.0f && run::sceneReady())
        {
            m_readyIn -= dt;
            if (m_readyIn < 0.0f)
            {
                Net::ready();
            }
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
        if (m_controlsShow > 0.0f)
        {
            m_controlsShow -= dt;
            if (m_controlsShow <= 0.0f)
            {
                find("hud-controls").tween("opacity", "0", 0.6f, "ease");
            }
        }
        if (m_note > 0.0f)
        {
            m_note -= dt;
            if (m_note <= 0.0f)
            {
                find("hud-note").tween("opacity", "0", 0.5f, "ease");
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
        int costume = int(hud.z);
        setText("hud-target", costumeName(costume));
        // In its colour: the sheet's class for the costume.
        UIWidget@ mark = UIWidget::of(find("hud-target"));
        for (int c = 0; c < 6; ++c)
        {
            mark.removeClass("costume-" + c);
        }
        mark.addClass("costume-" + costume);
        setText("hud-score", "Points " + int(hud.x));
        setText("hud-time", clock(hud.y));
        setText("hud-rank", place(int(hud.w)) + " of 4");
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
        screens().push(kResultsDoc);
        setText("res-title", place == 1 ? "The ball is yours" : "The music stops");
        setText("res-you", "You came " + this.place(place) + " with " + you + " points");
        setText("res-0", "You        " + you);
        setText("res-1", "Hunter 1   " + int(scores.y));
        setText("res-2", "Hunter 2   " + int(scores.z));
        setText("res-3", "Hunter 3   " + int(scores.w));
        setText("res-best", record ? "Your best round yet" : "Your best: " + best + " points");
        wire("again-btn", EventHandler(this.onAgain));
        find("again-btn").visible = m_mode != kJoining; // the host starts the next round
        wire("title-btn", EventHandler(this.onTitle));
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
        find("hud-controls").tween("opacity", "0", 0.0f, "ease"); // the menu shows them itself
        screens().push(kPauseDoc);
        wire("resume-btn", EventHandler(this.onResume));
        wire("restart-btn", EventHandler(this.onAgain));
        find("restart-btn").visible = m_mode != kJoining; // the host's to start over
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
        sound(kClick);
        letGo();
    }

    // Another round: alone, the map afresh; hosting, the same scene goes on (the players' hunters
    // with it) and the Ball starts over ("Restart").
    private bool onAgain(GuiEvent e)
    {
        sound(kClick);
        if (m_mode == kHosting)
        {
            enterRound();
            run::currentScene().events.emit("Restart", 0); // Ball.as starts over
            return true;
        }
        startRound();
        return true;
    }

    // Under any menu the player's actions read released: the input map's "Menu" set (Pause alone)
    // is pushed as the only one in effect, and a press already held when it opens or closes stays
    // latched until let go, so the press that works a menu's button is never a Tag. Once, however
    // many menus ask.
    private void holdInput()
    {
        if (!m_inputHeld)
        {
            Input::pushSet("Menu");
            m_inputHeld = true;
        }
    }

    private void releaseInput()
    {
        if (m_inputHeld)
        {
            Input::popSet();
            m_inputHeld = false;
        }
    }

    // Alone, the scene stops behind a menu too. In a networked round it goes on (the host runs the
    // world for everyone, and a joiner's scene must keep hearing the host: a new round, say).
    private void holdStill()
    {
        holdInput();
        if (m_mode == kAlone)
        {
            run::setTimeScale(0.0f);
        }
    }

    private void letGo()
    {
        run::setTimeScale(1.0f);
        releaseInput();
    }

    // The HUD over the scene that is already running.
    private void enterRound()
    {
        screens().clear();
        screens().push(kHudDoc);
        m_inRound = true;
        m_paused = false;
        m_note = 0.0f;
        letGo();
    }

    private bool onTitle(GuiEvent e)
    {
        sound(kClick);
        if (m_mode != kAlone)
        {
            Net::disconnect();
            m_mode = kAlone;
        }
        m_readyIn = -1.0f;
        m_joinWait = -1.0f;
        showTitle();
        return true;
    }

    private void sound(Guid clip)
    {
        Audio::playOneShot(clip, AudioBus::Effects, 0.8f);
    }

    private void note(string text)
    {
        setText("hud-note", text);
        find("hud-note").tween("opacity", "1", 0.15f, "ease"); // from the sheet's 0, or a fade going
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
