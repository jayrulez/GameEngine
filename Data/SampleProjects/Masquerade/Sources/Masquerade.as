// Masquerade: the game's orchestrator (the reserved class Game). It runs the title, a round at the
// ball under its HUD, the pause menu and the results.
//
// The title offers Play alone, Host, Join, Controls and Quit. Play alone and Host lead to the map
// page (Maps), then play the map alone or host it: the server listens on `kPort` and waits for
// each joiner to report ready. Join connects to a host from the title; the host's welcome names
// its map, which then loads, and the joiner reports ready so the map's networked entities are in
// place for the host's first snapshot.
//
// The round itself is run by the scene (Ball.as); this script listens to it: "InReach" (someone
// to tag is in front of you, or no longer), "HunterNear" (your hunter is near, or no longer),
// "Hud" (four times a second: your points, the seconds left, your target's costume, your place),
// "Note" (something happened to you, or a mask flared), "RoundOver" (everyone's points) and
// "RoundStart" (the host began another round). Pause (Escape or Start) shows the pause menu, which
// freezes the round when playing alone; a networked round goes on. The save keeps each map's best
// round ("best.Palace", "best.Canal", "best.Gardens").

const float kControlsShown = 7.0f; // how long the controls show as a round starts (s)
// The screens, shown on the engine GUI's screen stack, by the asset ids of their documents.
const string kTitleDoc = "{2c8d8d4f-d770-496d-a766-d14cfbfcd328}";
const string kMapsDoc = "{bef19424-1fc4-4f06-b262-b49abeacf1a6}";
const string kJoinDoc = "{36b90866-3b41-4a1c-8b87-c0305859bb5d}";
const string kControlsDoc = "{9e97d8b6-d7f7-42c0-8554-07fb7c631b59}";
// Each costume's portrait (Tools/portraits.py), in costume order (rose, teal and gold gowns, then
// black, plum and ivory coats), in the "{id}" form the HUD's Image takes for a texture.
array<string> kPortraits = {"{cc8a11f4-8f17-44d7-8de3-4eb3cb85ecba}", "{41b5c641-67bf-42bf-ba9c-f9c806f3ed14}",
                            "{7676ab80-0ce8-4fd6-9930-6ac5efaee6c3}", "{9ec8ca2c-9a53-4c3b-9bdf-bb4bf1ace540}",
                            "{96fe7fa5-05f5-4509-b87c-e2ad11df97a8}", "{72a109fb-d9b4-468e-a9c7-b4978d6ad72b}"};
const float kLateSeconds = 30.0f; // the clock turns warm for the last half minute of the round
const string kHudDoc = "{e183e431-f835-4289-a1eb-88d33b67db46}";
const string kPauseDoc = "{76517a0e-2810-4018-ad32-036418a2bc1c}";
const string kResultsDoc = "{d6ee5f73-8622-4f72-8f7b-1d7e43f609b8}";
Guid kPalace = Guid("20eb69af-217d-4b37-93d2-d032e63094fb");
Guid kCanal = Guid("67c261b3-8da6-4a11-bab2-c96595b651cf");
Guid kGardens = Guid("6c8abc4e-60ff-4614-864b-68fd5dc21e19");
// Sounds made by sounds.py: the round's cues to the player, and the buttons.
Guid kTagSound = Guid("ea04649b-1257-4914-9348-2c9369d71bb4");
Guid kMissSound = Guid("beb39d19-017a-498f-a6d0-6ebb036b5f13");
Guid kFlareSound = Guid("2eb1ef65-d86b-4806-8a02-94b50e9702ea");
Guid kChimeSound = Guid("75d06e2f-5b1f-468e-96fd-e4177e8a3a8f");
Guid kFanfare = Guid("5a0d5172-db3d-44be-8922-f9249c64fa39");
Guid kClick = Guid("98658096-7e9e-4891-9f68-7b1f7125b3a2");

// The notes Ball.as sends (the same values as there).
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
// Searches the top screen first. A screen just cleared or popped stays in the tree while its exit
// animation plays, with the same ids, and would otherwise be found first.
Node@ find(string id)
{
    Screen@ top = screens().top();
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}
void wire(string id, EventHandler@ handler) { find(id).on("click", handler); } // handlers are named on*, like the engine's event handlers
void setText(string id, string text) { Label::of(find(id)).text = text; }

class Game
{
    private bool m_inRound = false;
    private bool m_paused = false;
    private float m_note = 0.0f;         // how much longer the HUD's message shows
    private float m_delta = 0.0f;        // how much longer the change in points shows
    private int m_shownScore = 0;        // the points the HUD shows
    private int m_shownCostume = -1;     // the mark whose portrait the HUD shows
    private bool m_late = false;         // the clock is in the round's last half minute
    private float m_roundSeconds = 0.0f; // the round's length, taken from the time in its first report
    private bool m_inputHeld = false;    // the "Menu" input set is pushed (holdInput)
    private float m_controlsShow = 0.0f; // how much longer the controls panel shows
    private bool m_onTitle = false;      // the title (or a page over it) is up, so the camera sweeps before the dais
    private int m_map = 0;               // 0 the Palace, 1 the Canal, 2 the Gardens
    private int m_mode = kAlone;
    private string m_address = "127.0.0.1";
    private float m_readyIn = -1.0f;     // joiner: time until it tells the host it is ready, once the host's map has loaded
    private float m_joinWait = -1.0f;    // joiner on the title: time left before it gives up on the host

    void launch()
    {
        // A joiner learns the host's map from its welcome (sent by Ball.as in onJoined). The handler
        // lives here rather than in a scene script because the welcome arrives while the title's
        // scene is the one loaded.
        NetTextHandler@ map = NetTextHandler(this.onHostMap);
        Net::on("map", map);
        showTitle();
    }

    // ---- the title ----
    private void showTitle()
    {
        m_inRound = false;
        m_onTitle = true;
        m_paused = false;
        m_controlsOpen = false; // the stack is cleared below
        run::setTimeScale(0.0f); // whatever is behind the title holds still
        holdInput();
        screens().clear();
        screens().push(kTitleDoc);
        wire("solo-btn", EventHandler(this.onSolo));
        wire("host-btn", EventHandler(this.onHost));
        if (!Net::canHost())
        {
            UIWidget::of(find("host-btn")).addClass("gone"); // a browser cannot host, only join
        }
        wire("join-btn", EventHandler(this.onJoin));
        wire("controls-btn", EventHandler(this.onControls));
        wire("quit-btn", EventHandler(this.onQuit));
    }

    // ---- the controls page, over the title or the pause menu ----
    private bool m_controlsOpen = false;

    private bool onControls(GuiEvent e)
    {
        sound(kClick);
        screens().push(kControlsDoc);
        wire("controls-back", EventHandler(this.onControlsBack));
        m_controlsOpen = true;
        return true;
    }

    private bool onControlsBack(GuiEvent e)
    {
        closeControls();
        return true;
    }

    // Closes the controls page on its Back button or the game's Pause (Escape, Start). The screen
    // stack's own back handling is not used: Escape is also the game's Pause, so it would close the
    // pause menu underneath too.
    private void closeControls()
    {
        if (!m_controlsOpen)
        {
            return;
        }
        sound(kClick);
        m_controlsOpen = false;
        screens().pop();
    }

    // ---- the map page, to play alone or host: a card per map, the chosen one lit, then Start ----
    private bool m_hostChosen = false; // Start on the map page hosts (otherwise it plays alone)

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

    // Lights the chosen card. Clicking a card chooses it; Start then plays or hosts it.
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

    // Start on the map page: play the chosen map alone, or host it.
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

    // ---- joining: enter the address, then Join; the host chooses the map ----
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

    // Joiner only: the host's map arrives. It loads, and once it is ready the host's state starts
    // to arrive.
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
        m_readyIn = 0.2f; // after the map loads, wait a moment more for its first frame
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
        leaveTitle();
        m_paused = false;
        m_inRound = true;
        m_note = 0.0f;
        screens().clear();
        screens().push(kHudDoc);
        setText("hud-target", "");
        setText("hud-score", "0");
        m_shownScore = 0;
        m_shownCostume = -1;
        m_roundSeconds = 0.0f;
        m_late = false;
        find("hud-controls").tween("opacity", "1", 0.3f, "ease"); // from the style sheet's 0
        m_controlsShow = kControlsShown;
        letGo(); // restore time and the player's input, since a round may start from any menu
        run::loadScene(m_map == 0 ? kPalace : (m_map == 1 ? kCanal : kGardens));
    }

    // Puts the camera back behind the player (CameraRig) as the title gives way to a round.
    private void leaveTitle()
    {
        if (m_onTitle)
        {
            m_onTitle = false;
            run::currentScene().events.emit("TitleView", -1.0f);
        }
    }

    void update(float dt)
    {
        if (m_controlsOpen && Input::wasPressed("Pause"))
        {
            closeControls();
            return;
        }
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
            if (m_onTitle)
            {
                run::currentScene().events.emit("TitleView", dt); // CameraRig sweeps before the dais
            }
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
        if (m_delta > 0.0f)
        {
            m_delta -= dt;
            if (m_delta <= 0.0f)
            {
                find("hud-delta").tween("opacity", "0", 0.6f, "ease");
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
        // Colour the target by the style sheet's class for its costume.
        UIWidget@ mark = UIWidget::of(find("hud-target"));
        for (int c = 0; c < 6; ++c)
        {
            mark.removeClass("costume-" + c);
        }
        mark.addClass("costume-" + costume);
        // Change the portrait only when the target changes: setting the picture reloads it, and it
        // shows blank meanwhile.
        if (costume != m_shownCostume && costume >= 0 && costume < int(kPortraits.length()))
        {
            m_shownCostume = costume;
            Image::of(find("hud-portrait")).src = kPortraits[costume];
        }
        // The points, with each change floating beside them for a moment: gold up, red down.
        int score = int(hud.x);
        if (score != m_shownScore)
        {
            int change = score - m_shownScore;
            m_shownScore = score;
            setText("hud-score", "" + score);
            setText("hud-delta", (change > 0 ? "+" : "") + change);
            UIWidget@ delta = UIWidget::of(find("hud-delta"));
            delta.removeClass(change > 0 ? "loss" : "gain");
            delta.addClass(change > 0 ? "gain" : "loss");
            find("hud-delta").tween("opacity", "1", 0.15f, "ease");
            m_delta = 1.4f;
        }
        // The time, over a bar of the round remaining (the round's length is the first time
        // reported).
        m_roundSeconds = Math::Max(m_roundSeconds, hud.y);
        setText("hud-time", clock(hud.y));
        ProgressBar::of(find("hud-timebar")).value = m_roundSeconds > 0.0f ? hud.y / m_roundSeconds : 0.0f;
        // Set the class only when it changes; setting it on every report would restart its pulse.
        bool late = hud.y <= kLateSeconds && hud.y > 0.0f;
        if (late != m_late)
        {
            m_late = late;
            UIWidget@ clockCard = UIWidget::of(find("hud-clock"));
            if (late)
            {
                clockCard.addClass("late");
            }
            else
            {
                clockCard.removeClass("late");
            }
        }
        setText("hud-rank", place(int(hud.w)) + " of 4");
    }

    // Your own hunter is near (from Ball.as): a quiet line shows under your target while it is.
    void onHunterNear(int yes)
    {
        if (m_inRound)
        {
            find("hud-near").tween("opacity", yes != 0 ? "1" : "0", yes != 0 ? 0.3f : 0.8f, "ease");
        }
    }

    // Someone is in front of you to tag (from Ball.as), so the Tag prompt shows.
    void onInReach(int yes)
    {
        if (m_inRound)
        {
            find("hud-tag").tween("opacity", yes != 0 ? "1" : "0", 0.15f, "ease");
        }
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
        // Every hunter by points, most first (yours first among equals), with your row lit.
        array<string> names = {"You", "Hunter 1", "Hunter 2", "Hunter 3"};
        array<int> points = {you, int(scores.y), int(scores.z), int(scores.w)};
        array<int> order = {0, 1, 2, 3};
        for (int i = 1; i < 4; ++i)
        {
            for (int j = i; j > 0 && points[order[j]] > points[order[j - 1]]; --j)
            {
                int t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        }
        for (int row = 0; row < 4; ++row)
        {
            int who = order[row];
            setText("rank-place-" + row, this.place(row + 1));
            setText("rank-name-" + row, names[who]);
            setText("rank-points-" + row, "" + points[who]);
            UIWidget@ line = UIWidget::of(find("rank-" + row));
            if (who == 0)
            {
                line.addClass("you");
            }
            else
            {
                line.removeClass("you");
            }
        }
        setText("res-best", record ? "Your best round yet" : "Your best: " + best + " points");
        wire("again-btn", EventHandler(this.onAgain));
        find("again-btn").visible = m_mode != kJoining; // only the host starts the next round
        wire("title-btn", EventHandler(this.onTitle));
    }

    // The host started another round in the same scene, so a joiner leaves its results screen.
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
        find("restart-btn").visible = m_mode != kJoining; // only the host can start over
        wire("title-btn", EventHandler(this.onTitle));
        wire("pause-controls-btn", EventHandler(this.onControls));
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

    // Another round. Alone, the map loads afresh. Hosting, the same scene goes on, keeping the
    // players' hunters, and the Ball starts over ("Restart").
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

    // While any menu is up, the player's actions read as released. The input map's "Menu" set,
    // which holds only Pause, is pushed as the only set in effect, and a press already held when it
    // is pushed or popped stays latched until released, so the press that clicks a menu button is
    // never read as a Tag. The set is pushed once, however many menus ask.
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

    // When playing alone, the scene also stops behind a menu. In a networked round it goes on: the
    // host runs the world for everyone, and a joiner's scene must keep receiving the host's
    // updates, such as a new round starting.
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

    // Shows the HUD over the scene that is already running.
    private void enterRound()
    {
        leaveTitle();
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
        find("hud-note").tween("opacity", "1", 0.15f, "ease"); // from the style sheet's 0, or from a fade in progress
        m_note = 3.0f;
    }

    // In the costume order of blender/guest.py, which the crowd and the looks follow.
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
