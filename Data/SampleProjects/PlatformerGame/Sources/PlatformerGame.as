// PlatformerGame - the run's orchestrator (the reserved class Game): the title screen, the
// levels in order with an intro and a clear tally each, the pause menu, the fades between them,
// the HUD, and the stakes.
//
// The stakes: a run has three lives. A fall or a hit costs one; a heart found in a level gives
// one back, and so does every 50th coin. The last life lost is the game over, and trying again
// starts the run over from the first level.
//
// The score: a coin is 100, a stomped enemy 200, a level's gem 500, and a clear adds a bonus for
// every second under the level's par time and 1000 for a level without a fall. Each clear earns
// up to three stars: all the coins, no falls, under par. A level's best score, stars and time,
// and the best run, are saved (the Save facade) and shown on the title.
//
// The flow is one state machine on the run's REAL clock (run::realDeltaTime), because the
// game sits at time scale 0 on every screen but play: the title, an intro, a pause, a clear.
//
// A level's coins announce themselves ("CoinRegistered") and report pickups
// ("CoinCollected"), enemies report stomps ("EnemyDefeated"), pickups report hearts and gems
// ("LifeCollected", "GemCollected"), the player reports falls ("PlayerDied"), and the goal flag
// ends the level ("GoalReached"). In a run the scene bus IS the run bus, so all of them reach
// this class.

Guid kHudDoc = Guid("628098f1-29ea-4b5e-bdea-e6a828da27f5");
Guid kTitleDoc = Guid("56b50198-2bcf-4b62-8f30-8d13a4f5cce0");
Guid kIntroDoc = Guid("490f0994-d42b-488d-b9f7-25199105819a");
Guid kPauseDoc = Guid("a5fbc391-7746-4948-80a3-4e04ce330d93");
Guid kClearDoc = Guid("3f75887a-3307-46a8-9a26-cb67ff50b552");
Guid kVictoryDoc = Guid("02f01d46-4ab6-4d5e-99b1-f805ee089785");
Guid kFadeDoc = Guid("34ea193f-3d13-409c-bc2b-9727c0b6df87");
Guid kSettingsDoc = Guid("b2d2fa8d-33c1-458a-a509-af220c6b2141");
Guid kGameOverDoc = Guid("201f7898-8288-44c8-a274-bdfbc0f2fc44");

Guid kLevel1 = Guid("6dd1ae0e-fbe8-4c9b-8c9e-d10b727f4d84");
Guid kLevel2 = Guid("96aab32b-3151-41a9-ac2e-5d16ba80ee06");
Guid kLevel3 = Guid("07b014f6-9147-43ff-aa83-bdeb6d846ceb");
Guid kLevel4 = Guid("6702ae48-a78e-4c75-aa28-538a15c2c03f");
Guid kLevel5 = Guid("15c15063-ec67-430b-9a55-1fb9617f6b6a");

// Music: CodeManu's Platformer Game Music Pack (CC-BY 3.0) and Juhani Junkala's Chiptune
// Adventures (CC0); see CREDITS.md.
Guid kMenuMusic = Guid("a20b5995-a5a4-4fe5-ae2a-a7116075100c");
Guid kLevel1Music = Guid("8d11b3f5-6a2b-404d-8b28-e29582d02a36");
Guid kLevel2Music = Guid("72646687-10e4-464f-af68-c9585bca1f82");
Guid kLevel3Music = Guid("ddb09ad4-a77d-489e-8feb-4166d4a88200");
Guid kButtonSound = Guid("3202be18-f708-429a-9e20-c8f22e6dade4");
Guid kClearJingle = Guid("c66fcc06-6184-4d2e-b03d-9c7ce7e82269");
Guid kVictoryJingle = Guid("ae009c62-c064-4d20-905e-c188865664ca");
// Audio/powerUp7 (Kenney, CC0): the tally's tick and a star landing, pitched up as they go.
Guid kChime = Guid("6d963695-67f8-4b7f-9cf4-d234f8034116");
const float kMusicVolume = 0.55f;

const int kLevelCount = 5;
const int kStartLives = 3;
const int kMaxLives = 9;
const int kCoinsPerLife = 50;
const int kCoinPoints = 100;
const int kStompPoints = 200;
const int kGemPoints = 500;
const int kSecondPoints = 20;
const int kFlawlessPoints = 1000;
const float kFadeSeconds = 0.45f;
/// After the last life goes, the poof plays out before the game over card.
const float kDyingSeconds = 1.1f;
/// How long the clear card waits once its tally is done before the next level comes on its own.
const float kClearHoldSeconds = 4.0f;

Guid levelScene(int index)
{
    switch (index)
    {
    case 1: return kLevel2;
    case 2: return kLevel3;
    case 3: return kLevel4;
    case 4: return kLevel5;
    }
    return kLevel1;
}

/// The later levels reuse the first ones' tracks.
Guid levelMusic(int index)
{
    switch (index)
    {
    case 1: return kLevel2Music;
    case 2: return kLevel3Music;
    case 3: return kLevel1Music;
    case 4: return kLevel2Music;
    }
    return kLevel1Music;
}

string levelName(int index)
{
    switch (index)
    {
    case 1: return "Crab Crossing";
    case 2: return "Sky Climb";
    case 3: return "Bee Meadow";
    case 4: return "Cloud Fortress";
    }
    return "Grassy Hills";
}

/// A clear in this many seconds or fewer earns the time star, and every second under it scores.
float levelPar(int index)
{
    switch (index)
    {
    case 1: return 50.0f;
    case 2: return 60.0f;
    case 3: return 70.0f;
    case 4: return 85.0f;
    }
    return 40.0f;
}

enum Phase
{
    Title,
    Settings,
    FadingOut,
    FadingIn,
    Intro,
    Playing,
    Paused,
    Dying,
    GameOver,
    Cleared,
    Victory
}

class Game
{
    private Phase m_phase = Phase::Title;
    /// Real seconds in the current phase.
    private float m_timer = 0.0f;
    /// Where a fade out lands: the title, or level m_nextLevel.
    private bool m_toTitle = false;
    private int m_nextLevel = 0;
    /// The phase a fade in reveals.
    private Phase m_afterFade = Phase::Intro;
    private UIView@ m_fade;
    /// The track playing: 0 none, 1 the menu's, 2 + n level n's. A track asked for again keeps
    /// playing rather than restarting.
    private int m_track = 0;

    // ---- the run ----
    private int m_lives = kStartLives;
    /// Points banked by the levels cleared so far.
    private int m_score = 0;
    /// Coins toward the next extra life.
    private int m_lifeCoins = 0;
    private int m_runStars = 0;
    private int m_allCoins = 0;
    private int m_allCoinTotal = 0;
    private int m_allFalls = 0;
    private float m_allTime = 0.0f;
    /// The score the HUD shows, rolling up toward the real one.
    private float m_shownScore = 0.0f;

    // ---- this level ----
    private int m_level = 0;
    private int m_coins = 0;
    private int m_coinTotal = 0;
    private int m_falls = 0;
    private int m_stomps = 0;
    private bool m_gem = false;
    private float m_time = 0.0f;

    // ---- the clear tally ----
    private int m_timeBonus = 0;
    private int m_flawless = 0;
    private int m_levelScore = 0;
    private int m_stars = 0;
    private bool m_newBest = false;
    /// The tally's steps done so far, and whether the player skipped to the end.
    private int m_tallyStep = 0;
    private bool m_tallyDone = false;

    void launch()
    {
        // The default scene is the title's backdrop, frozen behind the menu.
        run::setTimeScale(0.0f);
        showTitle();
    }

    void update(float dt)
    {
        float real = run::realDeltaTime();
        m_timer += real;
        switch (m_phase)
        {
        case Phase::FadingOut:
            if (m_timer >= kFadeSeconds)
            {
                arrive();
            }
            break;
        case Phase::FadingIn:
            if (m_timer >= kFadeSeconds)
            {
                ui::pop(); // the fade layer, on top since the fade began
                enter(m_afterFade);
                if (m_afterFade == Phase::Intro)
                {
                    // The level's card drops in from above and settles with a bounce.
                    UIView@ card = ui::find("intro-card");
                    card.moveTo(0.0f, 0.0f, 0.55f, Ease::OutBack);
                    card.fadeTo(1.0f, 0.25f);
                }
            }
            break;
        case Phase::Intro:
            // On the RELEASE: the press lands while the level is still frozen, so the player
            // never sees it and does not jump the moment play starts.
            if (Input::wasReleased("Jump"))
            {
                ui::pop(); // the intro banner
                run::setTimeScale(1.0f);
                enter(Phase::Playing);
            }
            break;
        case Phase::Playing:
            m_time += dt;
            ui::findLabel("hud-time").setText(clock(m_time));
            rollScore(real, m_score + levelPoints());
            if (Input::wasPressed("Pause"))
            {
                pause();
            }
            break;
        case Phase::Dying:
            rollScore(real, m_score + levelPoints());
            if (m_timer >= kDyingSeconds)
            {
                showGameOver();
            }
            break;
        case Phase::Settings:
            if (Input::wasPressed("Pause"))
            {
                onSettingsBack();
            }
            break;
        case Phase::Paused:
            if (Input::wasPressed("Pause"))
            {
                onResume();
            }
            break;
        case Phase::Cleared:
            rollScore(real, m_score); // the clear's points are banked: the HUD catches up under the card
            updateTally();
            break;
        default:
            break;
        }
    }

    void exit()
    {
        ui::clear();
    }

    // ---- run events ----
    void onCoinRegistered(int count)
    {
        m_coinTotal += count;
        refreshHud();
    }

    void onCoinCollected(int value)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_coins += value;
        popup("+" + (value * kCoinPoints));
        ui::find("hud-coin").pulse(1.35f, 0.25f);
        m_lifeCoins += value;
        if (m_lifeCoins >= kCoinsPerLife)
        {
            m_lifeCoins -= kCoinsPerLife;
            gainLife();
        }
        refreshHud();
    }

    void onEnemyDefeated(int count)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_stomps += count;
        popup("+" + (count * kStompPoints));
        refreshHud();
    }

    void onGemCollected(int unused)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_gem = true;
        popup("+" + kGemPoints);
        UIView@ gem = ui::find("hud-gem");
        gem.setVisible(true);
        gem.setScale(0.0f);
        gem.scaleTo(1.0f, 0.45f, Ease::OutBack);
        refreshHud();
    }

    void onLifeCollected(int count)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        gainLife();
    }

    void onPlayerDied(int deaths)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_falls += 1;
        m_lives -= 1;
        UIView@ heart = ui::find("hud-heart");
        heart.pulse(0.6f, 0.35f); // the heart shrinks and comes back: a life gone
        refreshHud();
        if (m_lives <= 0)
        {
            // The poof plays out, the pad gives a long low rumble, then the card.
            Input::rumble(0.9f, 0.2f, 0.6f);
            Audio::setVoiceVolume(Audio::musicVoice(), 0.15f, 0.8f);
            enter(Phase::Dying);
        }
    }

    void onGoalReached(int unused)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        // Time runs on under the card: the confetti flies and the player stands and watches.
        float par = levelPar(m_level);
        m_timeBonus = (m_time < par) ? int(par - m_time) * kSecondPoints : 0;
        m_flawless = (m_falls == 0) ? kFlawlessPoints : 0;
        m_levelScore = levelPoints() + m_timeBonus + m_flawless;
        m_stars = 0;
        if (m_coins >= m_coinTotal)
        {
            m_stars += 1;
        }
        if (m_falls == 0)
        {
            m_stars += 1;
        }
        if (m_time <= par)
        {
            m_stars += 1;
        }
        m_newBest = saveLevel(m_level, m_levelScore, starMask(), m_time);

        m_score += m_levelScore;
        m_runStars += m_stars;
        m_allCoins += m_coins;
        m_allCoinTotal += m_coinTotal;
        m_allFalls += m_falls;
        m_allTime += m_time;
        Audio::playOneShot(kClearJingle);
        UIScreen@ s = ui::push(kClearDoc);
        s.findLabel("star-3-caption").setText("Under " + clock(par));
        s.findLabel("tally-total").setText("0");
        s.findLabel("clear-best").setText("");
        s.findLabel("clear-next").setText("");
        m_tallyStep = 0;
        m_tallyDone = false;
        enter(Phase::Cleared);
    }

    // ---- buttons ----
    void onPlay()
    {
        click();
        startRun();
    }

    /// The volumes, on the audio buses the player saves when it exits, so a change made
    /// here is there next time without the game storing anything itself.
    void onSettings()
    {
        click();
        UIScreen@ s = ui::push(kSettingsDoc);
        bindVolume(s, "master", AudioBus::Master, Action(this.onMasterChanged));
        bindVolume(s, "music", AudioBus::Music, Action(this.onMusicChanged));
        bindVolume(s, "effects", AudioBus::Effects, Action(this.onEffectsChanged));
        s.findButton("settings-back-btn").onClick(Action(this.onSettingsBack));
        enter(Phase::Settings);
    }

    void onSettingsBack()
    {
        click();
        ui::pop(); // the settings screen; the title is under it
        enter(Phase::Title);
    }

    void onMasterChanged()
    {
        volumeChanged("master", AudioBus::Master);
    }

    void onMusicChanged()
    {
        volumeChanged("music", AudioBus::Music);
    }

    void onEffectsChanged()
    {
        volumeChanged("effects", AudioBus::Effects);
        click(); // hear the level being set; the music is heard anyway
    }

    void onQuit()
    {
        click();
        run::requestExit(0);
    }

    void onResume()
    {
        click();
        ui::pop(); // the pause menu
        run::setTimeScale(1.0f);
        enter(Phase::Playing);
    }

    /// From the pause menu: the level again from its start, with the score it started with and
    /// the lives left now (a restart is no way to win lives back).
    void onRestart()
    {
        click();
        fadeToLevel(m_level);
    }

    void onRetry()
    {
        click();
        startRun();
    }

    void onQuitToTitle()
    {
        click();
        fadeToTitle();
    }

    // ---- the flow ----
    private void enter(Phase phase)
    {
        m_phase = phase;
        m_timer = 0.0f;
    }

    private void startRun()
    {
        m_lives = kStartLives;
        m_score = 0;
        m_shownScore = 0.0f;
        m_lifeCoins = 0;
        m_runStars = 0;
        m_allCoins = 0;
        m_allCoinTotal = 0;
        m_allFalls = 0;
        m_allTime = 0.0f;
        fadeToLevel(0);
    }

    /// A bound volume row: its slider at the bus's level, its readout, and the handler.
    private void bindVolume(UIScreen@ s, string row, AudioBus bus, Action@ handler)
    {
        UISlider@ slider = s.findSlider(row + "-slider");
        slider.setValue(Audio::busVolume(bus));
        showVolume(row, slider.value);
        slider.onChanged(handler);
    }

    private void volumeChanged(string row, AudioBus bus)
    {
        float level = ui::findSlider(row + "-slider").value;
        Audio::setBusVolume(bus, level);
        showVolume(row, level);
    }

    private void showVolume(string row, float level)
    {
        ui::findLabel(row + "-value").setText("" + int(level * 100.0f + 0.5f) + "%");
    }

    private void showTitle()
    {
        music(1);
        ui::clear();
        UIScreen@ s = ui::push(kTitleDoc);
        s.findButton("play-btn").onClick(Action(this.onPlay));
        s.findButton("settings-btn").onClick(Action(this.onSettings));
        s.findButton("quit-btn").onClick(Action(this.onQuit));
        s.findLabel("title-stars").setText("" + savedStars() + " / " + (kLevelCount * 3));
        int best = Save::getInt("best.run", 0);
        s.findLabel("title-best").setText(best > 0 ? "Best " + points(best) : "");
        enter(Phase::Title);
    }

    private void pause()
    {
        run::setTimeScale(0.0f);
        UIScreen@ s = ui::push(kPauseDoc);
        s.findButton("resume-btn").onClick(Action(this.onResume));
        s.findButton("restart-btn").onClick(Action(this.onRestart));
        s.findButton("title-btn").onClick(Action(this.onQuitToTitle));
        enter(Phase::Paused);
    }

    private void showGameOver()
    {
        run::setTimeScale(0.0f);
        int total = m_score + levelPoints();
        bool best = saveRun(total);
        UIScreen@ s = ui::push(kGameOverDoc);
        s.findLabel("gameover-reached").setText("Reached level " + (m_level + 1) + ", " + levelName(m_level));
        s.findLabel("gameover-score").setText(points(total));
        s.findLabel("gameover-best").setText(best ? "New best run!" : "Best " + points(Save::getInt("best.run", 0)));
        s.findButton("gameover-retry-btn").onClick(Action(this.onRetry));
        s.findButton("gameover-title-btn").onClick(Action(this.onQuitToTitle));
        enter(Phase::GameOver);
    }

    private void showVictory()
    {
        ui::pop(); // the clear card
        Audio::playOneShot(kVictoryJingle);
        bool best = saveRun(m_score);
        UIScreen@ s = ui::push(kVictoryDoc);
        s.findLabel("victory-summary").setText("Coins " + m_allCoins + " / " + m_allCoinTotal + "    Falls "
            + m_allFalls + "    Time " + clock(m_allTime));
        s.findLabel("victory-score").setText(points(m_score));
        s.findLabel("victory-stars").setText("" + m_runStars + " / " + (kLevelCount * 3));
        s.findLabel("victory-best").setText(best ? "New best run!" : "Best " + points(Save::getInt("best.run", 0)));
        s.findButton("victory-title-btn").onClick(Action(this.onQuitToTitle));
        enter(Phase::Victory);
    }

    private void fadeToLevel(int index)
    {
        m_toTitle = false;
        m_nextLevel = index;
        fadeOut();
    }

    private void fadeToTitle()
    {
        m_toTitle = true;
        fadeOut();
    }

    /// Black over everything, then arrive() swaps what is under it.
    private void fadeOut()
    {
        if (m_phase == Phase::FadingOut || m_phase == Phase::FadingIn)
        {
            return;
        }
        UIScreen@ s = ui::push(kFadeDoc);
        @m_fade = s.find("fade");
        m_fade.fadeTo(1.0f, kFadeSeconds);
        enter(Phase::FadingOut);
    }

    /// Under the black: the new scene and its screens, then the black fades away.
    private void arrive()
    {
        ui::clear();
        run::setTimeScale(0.0f);
        if (m_toTitle)
        {
            run::loadScene(kLevel1);
            showTitle();
            m_afterFade = Phase::Title;
        }
        else
        {
            m_level = m_nextLevel;
            m_coins = 0;
            m_coinTotal = 0; // the new level's coins register again
            m_falls = 0;
            m_stomps = 0;
            m_gem = false;
            m_time = 0.0f;
            run::loadScene(levelScene(m_level));
            m_track = 0; // the level's music from its start, at full volume after a game over
            music(2 + m_level);
            ui::push(kHudDoc);
            m_shownScore = float(m_score);
            refreshHud();
            UIScreen@ intro = ui::push(kIntroDoc);
            intro.findLabel("intro-number").setText("Level " + (m_level + 1));
            intro.findLabel("intro-name").setText(levelName(m_level));
            UIView@ card = intro.find("intro-card"); // above, waiting for the fade to clear
            card.setTranslation(0.0f, -260.0f);
            card.setOpacity(0.0f);
            m_afterFade = Phase::Intro;
        }
        UIScreen@ s = ui::push(kFadeDoc);
        @m_fade = s.find("fade");
        m_fade.setOpacity(1.0f);
        m_fade.fadeTo(0.0f, kFadeSeconds);
        enter(Phase::FadingIn);
    }

    // ---- the clear tally: one row at a time, each counting up with a tick ----
    private void updateTally()
    {
        if (!m_tallyDone && Input::wasPressed("Jump"))
        {
            // Skip to the end: every row, the total and the stars at once.
            while (m_tallyStep < 9)
            {
                tallyStep(m_tallyStep, true);
                m_tallyStep += 1;
            }
            finishTally();
            return;
        }
        if (!m_tallyDone)
        {
            // Step n lands at 0.45 s, then every 0.4 s.
            while (m_tallyStep < 9 && m_timer >= 0.45f + 0.4f * float(m_tallyStep))
            {
                tallyStep(m_tallyStep, false);
                m_tallyStep += 1;
            }
            if (m_tallyStep >= 9)
            {
                finishTally();
            }
            return;
        }
        bool more = m_level + 1 < kLevelCount;
        int left = int(kClearHoldSeconds - m_timer + 0.999f);
        ui::findLabel("clear-next").setText((more ? "Next level in " : "Results in ") + left
            + "    (Space or A to go on)");
        if (m_timer >= kClearHoldSeconds || Input::wasPressed("Jump"))
        {
            if (more)
            {
                fadeToLevel(m_level + 1);
            }
            else
            {
                showVictory();
            }
        }
    }

    /// Steps 0-4 are the rows, 5 the total, 6-8 the stars.
    private void tallyStep(int step, bool quiet)
    {
        if (step <= 4)
        {
            string key;
            string value;
            if (step == 0)
            {
                key = "coins";
                value = "" + m_coins + " / " + m_coinTotal + "    +" + points(m_coins * kCoinPoints);
            }
            else if (step == 1)
            {
                key = "stomps";
                value = "" + m_stomps + "    +" + points(m_stomps * kStompPoints);
            }
            else if (step == 2)
            {
                key = "gem";
                value = m_gem ? "Found    +" + points(kGemPoints) : "Not found";
            }
            else if (step == 3)
            {
                key = "time";
                value = clock(m_time) + "    +" + points(m_timeBonus);
            }
            else
            {
                key = "flawless";
                value = (m_flawless > 0) ? "+" + points(m_flawless) : "-";
            }
            UIView@ row = ui::find("tally-" + key + "-row");
            row.setVisible(true);
            UILabel@ label = ui::findLabel("tally-" + key);
            label.setText(value);
            if (!quiet)
            {
                row.setOpacity(0.0f);
                row.fadeTo(1.0f, 0.2f);
                row.setTranslation(-24.0f, 0.0f);
                row.moveTo(0.0f, 0.0f, 0.25f, Ease::Out);
                Audio::playOneShot(kChime, AudioBus::Effects, 0.35f, 0.9f + 0.08f * float(step));
            }
            return;
        }
        if (step == 5)
        {
            UILabel@ total = ui::findLabel("tally-total");
            total.setText(points(m_levelScore));
            if (!quiet)
            {
                total.pulse(1.4f, 0.35f);
            }
            return;
        }
        int star = step - 5; // 1 to 3
        if ((starMask() & (1 << (star - 1))) == 0)
        {
            return;
        }
        UIView@ filled = ui::find("star-" + star);
        filled.setVisible(true);
        if (!quiet)
        {
            filled.setScale(0.0f);
            filled.scaleTo(1.0f, 0.4f, Ease::OutBack);
            filled.setRotation(-40.0f);
            filled.rotateTo(0.0f, 0.4f, Ease::Out);
            Audio::playOneShot(kChime, AudioBus::Effects, 0.7f, 1.0f + 0.15f * float(star));
            Input::rumble(0.0f, 0.3f, 0.06f);
        }
    }

    private void finishTally()
    {
        m_tallyDone = true;
        UILabel@ best = ui::findLabel("clear-best");
        if (m_newBest)
        {
            best.setText("New best!");
            best.setScale(0.0f);
            best.scaleTo(1.0f, 0.4f, Ease::OutBack);
        }
        else
        {
            best.setText("Best " + points(Save::getInt("best.score." + m_level, 0)));
        }
        enter(Phase::Cleared); // the hold before the next level starts now
    }

    // ---- the save: per level its best score, stars and time; the best run ----
    /// Bit 0 all coins, bit 1 no falls, bit 2 under par: the stars this clear earned.
    private int starMask()
    {
        int mask = 0;
        if (m_coins >= m_coinTotal)
        {
            mask |= 1;
        }
        if (m_falls == 0)
        {
            mask |= 2;
        }
        if (m_time <= levelPar(m_level))
        {
            mask |= 4;
        }
        return mask;
    }

    /// Keeps the level's best score, every star ever earned there and its best time; true when
    /// the score is a new best.
    private bool saveLevel(int level, int score, int stars, float seconds)
    {
        string suffix = "." + level;
        bool best = score > Save::getInt("best.score" + suffix, 0);
        if (best)
        {
            Save::setInt("best.score" + suffix, score);
        }
        Save::setInt("best.stars" + suffix, Save::getInt("best.stars" + suffix, 0) | stars);
        float bestTime = Save::getFloat("best.time" + suffix, 0.0f);
        if (bestTime <= 0.0f || seconds < bestTime)
        {
            Save::setFloat("best.time" + suffix, seconds);
        }
        Save::flush(); // a clear is a moment worth keeping, whatever happens next
        return best;
    }

    private bool saveRun(int score)
    {
        bool best = score > Save::getInt("best.run", 0);
        if (best)
        {
            Save::setInt("best.run", score);
            Save::flush();
        }
        return best;
    }

    /// Every star earned on every level, over all runs.
    private int savedStars()
    {
        int total = 0;
        for (int level = 0; level < kLevelCount; level++)
        {
            int mask = Save::getInt("best.stars." + level, 0);
            for (int bit = 0; bit < 3; bit++)
            {
                if ((mask & (1 << bit)) != 0)
                {
                    total += 1;
                }
            }
        }
        return total;
    }

    // ---- the HUD ----
    private void music(int track)
    {
        if (track == m_track)
        {
            return;
        }
        m_track = track;
        Audio::playMusic((track == 1) ? kMenuMusic : levelMusic(track - 2), 1.0f, kMusicVolume);
    }

    private void click()
    {
        Audio::playOneShot(kButtonSound);
    }

    private void gainLife()
    {
        if (m_lives < kMaxLives)
        {
            m_lives += 1;
        }
        UILabel@ up = ui::findLabel("hud-life-popup");
        up.setVisible(true);
        up.setOpacity(1.0f);
        up.setTranslation(0.0f, 0.0f);
        up.moveTo(0.0f, -26.0f, 0.9f, Ease::Out);
        up.fadeTo(0.0f, 0.9f, Ease::In);
        ui::find("hud-heart").pulse(1.5f, 0.4f);
        refreshHud();
    }

    /// The points scored in the level so far, before its clear bonuses.
    private int levelPoints()
    {
        return m_coins * kCoinPoints + m_stomps * kStompPoints + (m_gem ? kGemPoints : 0);
    }

    /// A gain floating up from the score.
    private void popup(string text)
    {
        UILabel@ pop = ui::findLabel("hud-popup");
        pop.setText(text);
        pop.setVisible(true);
        pop.setOpacity(1.0f);
        pop.setTranslation(0.0f, 0.0f);
        pop.moveTo(0.0f, -22.0f, 0.7f, Ease::Out);
        pop.fadeTo(0.0f, 0.7f, Ease::In);
    }

    /// The HUD's score rolls up toward `goal`, quickly for a big gain.
    private void rollScore(float real, int goal)
    {
        float target = float(goal);
        if (m_shownScore >= target)
        {
            return;
        }
        float step = (target - m_shownScore) * 8.0f * real + 40.0f * real;
        m_shownScore = (m_shownScore + step > target) ? target : m_shownScore + step;
        UILabel@ score = ui::findLabel("hud-score");
        score.setText(points(int(m_shownScore)));
        if (m_shownScore >= target)
        {
            score.pulse(1.2f, 0.2f);
        }
    }

    private void refreshHud()
    {
        ui::findLabel("hud-lives").setText("x " + m_lives);
        ui::findLabel("hud-coins").setText("" + m_coins + " / " + m_coinTotal);
        ui::findLabel("hud-time").setText(clock(m_time));
        ui::findLabel("hud-level").setText("" + (m_level + 1) + "  " + levelName(m_level));
        ui::findLabel("hud-score").setText(points(int(m_shownScore)));
        ui::find("hud-gem").setVisible(m_gem);
    }

    private string clock(float seconds)
    {
        int whole = int(seconds);
        int s = whole % 60;
        return "" + (whole / 60) + ":" + (s < 10 ? "0" : "") + s;
    }

    /// A score with its thousands set apart: 12,400.
    private string points(int value)
    {
        string digits = "" + value;
        string grouped = "";
        int count = 0;
        for (int i = int(digits.length()) - 1; i >= 0; i--)
        {
            grouped = digits.substr(i, 1) + grouped;
            count += 1;
            if (count % 3 == 0 && i > 0)
            {
                grouped = "," + grouped;
            }
        }
        return grouped;
    }
}
