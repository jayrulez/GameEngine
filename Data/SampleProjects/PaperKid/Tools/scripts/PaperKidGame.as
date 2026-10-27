// PaperKidGame - the run's orchestrator (the reserved class Game): the screens, the levels, the
// score and the lives.
//
// The Level script owns a block's rules and tells this class how it ended ("QuotaMet" with the
// whole seconds left, or "LevelFailed" with 0 for time, 1 for papers); every delivery
// ("Delivered", from a Subscriber) scores here. In a run the scene bus IS the run bus, so all of
// them reach this class.
//
// A run is the blocks in order. A cleared block banks its score (deliveries plus a time bonus); a
// failed one spends a life and replays from the banked score; no lives left, or the last block
// cleared, is the Game over screen. Pause (the Pause action) freezes the block under a menu;
// Settings, from the title or the pause menu, sets the audio buses' volumes, which the player saves
// when it exits, and goes back to whichever opened it (Documentation/Specs/paperkid.md).
//
// A cleared block is celebrated before its screen: the clock stops, a banner drops in, the world
// eases into slow motion under a fanfare while the music fades, and only then does the Block
// cleared screen come up and the block freeze. A failed block cuts straight to its screen.
//
// The sound: the title's music on the menus, a track per block, a fanfare when a block is cleared
// and a jingle when one is failed, and the fanfare and a voice over the final screen before the
// ending's music. Buttons click, and a delivery's points rise from under the score and fade. The
// pad cheers a cleared block and slumps on a failed one (the bike and porches rumble their own).

// The screens, on the engine GUI's screen stack: their documents by asset id.
const string kTitleDoc = "{{{Title}}}";
const string kHudDoc = "{{{Hud}}}";
const string kClearedDoc = "{{{Cleared}}}";
const string kFailedDoc = "{{{Failed}}}";
const string kGameOverDoc = "{{{GameOver}}}";
const string kPauseDoc = "{{{Pause}}}";
const string kSettingsDoc = "{{{Settings}}}";

Guid kTitleMusic = Guid("{{MusicTitle}}");
Guid kEndingMusic = Guid("{{MusicEnding}}");
Guid kClearedFanfare = Guid("{{ClearedFanfare}}");
Guid kFailedJingle = Guid("{{FailedJingle}}");
Guid kCongratulations = Guid("{{Congratulations}}");
Guid kGameOverVoice = Guid("{{GameOverVoice}}");
Guid kClickSound = Guid("{{Click}}");
Guid kSliderSound = Guid("{{Slider}}");

const float kMusicVolume = 0.5f;
const float kPopSeconds = 0.9f;
const float kPopX = 262.0f; // under the HUD's score
const float kPopY = 96.0f;
// The celebration of a cleared block: the world slows to kCelebrateSlowest of real time over
// kCelebrateEase seconds (easing out), and the Block cleared screen comes up at kCelebrateSeconds.
const float kCelebrateEase = 1.1f;
const float kCelebrateSlowest = 0.12f;
const float kCelebrateSeconds = 1.9f;
const float kBannerDrop = 0.35f; // seconds for the banner to drop into place
Guid kStart = Guid("{{Start}}");

// A GUI event's handler (a button's click, a slider's move).
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
void wire(string id, string event, EventHandler@ handler) { find(id).on(event, handler); } // `on*` names are the engine's handlers
void setText(string id, string text)
{
    Node@ n = find(id);
    if (n !is null)
    {
        Label::of(n).text = text;
    }
}

const int kStartLives = 3;
const int kTimeBonusPerSecond = 5;

enum Phase
{
    Title,
    Playing,
    Paused,
    Settings,
    Celebrating,
    Ended
}

class Game
{
    private Phase m_phase = Phase::Title;
    private array<Guid> m_levels;
    private int m_level = 0;
    private int m_lives = kStartLives;
    private int m_score = 0;
    private int m_banked = 0; // the score when the current block started
    private int m_delivered = 0;
    private Phase m_settingsFrom = Phase::Title; // where Settings goes back to
    private array<Guid> m_blockMusic;
    private float m_endingIn = -1.0f; // seconds until the ending's music, after the final screen's voice
    private float m_celebrated = 0.0f; // seconds into a cleared block's celebration
    private int m_secondsLeft = 0;     // the clock when the block was cleared (its time bonus)

    void launch()
    {
        m_levels.insertLast(Guid("{{Block1}}"));
        m_levels.insertLast(Guid("{{Block2}}"));
        m_levels.insertLast(Guid("{{Block3}}"));
        m_levels.insertLast(Guid("{{Block4}}"));
        m_levels.insertLast(Guid("{{Block5}}"));
        m_blockMusic.insertLast(Guid("{{MusicBlockA}}"));
        m_blockMusic.insertLast(Guid("{{MusicBlockB}}"));
        m_blockMusic.insertLast(Guid("{{MusicBlockC}}"));
        // The default scene is the title's backdrop: the street goes about its day behind the menu.
        run::setTimeScale(1.0f);
        showTitle();
    }

    // The Game tier ticks at time scale 0 too, so the Pause action is read here in every phase.
    void update(float dt)
    {
        if (m_phase == Phase::Celebrating)
        {
            tickCelebration(dt);
        }
        if (m_endingIn >= 0.0f)
        {
            m_endingIn -= dt;
            if (m_endingIn < 0.0f)
            {
                Audio::playMusic(kEndingMusic, 1.0f, kMusicVolume);
            }
        }
        if (!Input::wasPressed("Pause"))
        {
            return;
        }
        if (m_phase == Phase::Playing)
        {
            pause();
        }
        else if (m_phase == Phase::Paused)
        {
            resume();
        }
        else if (m_phase == Phase::Settings)
        {
            settingsBack();
        }
    }

    void exit() {}

    // ---- what the level reports ----

    void onDelivered(int points)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_delivered += 1;
        m_score += points;
        setText("hud-score", "" + m_score);
        // The points: up 36 px from under the score over the pop's time, fading out late.
        Node@ pop = find("hud-pop");
        if (pop !is null)
        {
            Label::of(pop).text = "+" + points;
            pop.tween("transform", "translate(" + kPopX + "px, " + (kPopY - 36.0f) + "px)", kPopSeconds, "linear")
                .from("translate(" + kPopX + "px, " + kPopY + "px)");
            pop.tween("opacity", "0", kPopSeconds, "ease-in").from("1");
        }
    }

    void onQuotaMet(int secondsLeft)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_phase = Phase::Celebrating;
        m_celebrated = 0.0f;
        m_secondsLeft = secondsLeft;
        Input::rumble(0.3f, 0.6f, 0.45f); // the cheer of a cleared block
        Audio::stopMusic(1.2f);
        Audio::playOneShot(kClearedFanfare, AudioBus::Music);
        // The banner drops in over kBannerDrop, easing out.
        Node@ banner = find("hud-banner");
        banner.tween("opacity", "1", kBannerDrop, "ease-out").from("0");
        banner.tween("transform", "translateY(0px)", kBannerDrop, "ease-out").from("translateY(-60px)");
    }

    // The block winds down under the banner, then its screen comes up and it freezes.
    private void tickCelebration(float dt)
    {
        m_celebrated += dt;
        float eased = m_celebrated / kCelebrateEase;
        if (eased > 1.0f)
        {
            eased = 1.0f;
        }
        eased = 1.0f - (1.0f - eased) * (1.0f - eased);
        run::setTimeScale(1.0f + (kCelebrateSlowest - 1.0f) * eased);

        if (m_celebrated >= kCelebrateSeconds)
        {
            find("hud-banner").tween("opacity", "0", 0.0f, "linear");
            showCleared();
        }
    }

    private void showCleared()
    {
        m_phase = Phase::Ended;
        run::setTimeScale(0.0f);
        int earned = m_score - m_banked;
        int bonus = m_secondsLeft * kTimeBonusPerSecond;
        m_score += bonus;
        m_banked = m_score;
        setText("hud-score", "" + m_score);
        screens().push(kClearedDoc);
        setText("cleared-summary", "" + m_delivered + " delivered, " + earned + " points + " + bonus + " time bonus");
        if (m_level + 1 >= int(m_levels.length()))
        {
            Button::of(find("continue-btn")).text = "Finish";
        }
        wire("continue-btn", "click", EventHandler(this.onContinue));
    }

    void onLevelFailed(int reason)
    {
        if (m_phase != Phase::Playing)
        {
            return;
        }
        m_phase = Phase::Ended;
        run::setTimeScale(0.0f);
        Input::rumble(0.6f, 0.2f, 0.4f); // the slump of a failed block
        Audio::stopMusic(0.3f);
        Audio::playOneShot(kFailedJingle, AudioBus::Music);
        m_lives -= 1;
        setText("hud-lives", "" + m_lives);
        if (m_lives <= 0)
        {
            showGameOver(false);
            return;
        }
        screens().push(kFailedDoc);
        setText("failed-reason", reason == 0 ? "Out of time" : "Out of papers");
        setText("failed-lives", m_lives == 1 ? "1 life left" : "" + m_lives + " lives left");
        wire("retry-btn", "click", EventHandler(this.onRetry));
        wire("menu-btn", "click", EventHandler(this.onToTitle));
    }

    // ---- buttons ----

    bool onNewGame(GuiEvent e) { newGame(); return true; }
    bool onRetry(GuiEvent e) { retry(); return true; }
    bool onContinue(GuiEvent e) { continueRun(); return true; }
    bool onResume(GuiEvent e) { resume(); return true; }
    bool onSettings(GuiEvent e) { settings(); return true; }
    bool onSettingsBack(GuiEvent e) { settingsBack(); return true; }
    bool onToTitle(GuiEvent e) { toTitle(); return true; }
    bool onQuit(GuiEvent e) { quit(); return true; }

    private void newGame()
    {
        click();
        m_level = 0;
        m_lives = kStartLives;
        m_score = 0;
        m_banked = 0;
        startLevel();
    }

    private void retry()
    {
        click();
        startLevel();
    }

    private void continueRun()
    {
        click();
        m_level += 1;
        if (m_level >= int(m_levels.length()))
        {
            m_level = int(m_levels.length()) - 1;
            screens().pop(); // the Block cleared screen gives way to the summary
            showGameOver(true);
            return;
        }
        startLevel();
    }

    private void resume()
    {
        click();
        screens().pop(); // the pause menu
        run::setTimeScale(1.0f);
        m_phase = Phase::Playing;
    }

    private void settings()
    {
        click();
        m_settingsFrom = m_phase;
        screens().push(kSettingsDoc);
        bindVolume("master", AudioBus::Master, EventHandler(this.onMasterChanged));
        bindVolume("music", AudioBus::Music, EventHandler(this.onMusicChanged));
        bindVolume("effects", AudioBus::Effects, EventHandler(this.onEffectsChanged));
        wire("settings-back-btn", "click", EventHandler(this.onSettingsBack));
        m_phase = Phase::Settings;
    }

    private void settingsBack()
    {
        click();
        screens().pop(); // the settings screen; the title or the pause menu is under it
        m_phase = m_settingsFrom;
    }

    bool onMasterChanged(GuiEvent e)
    {
        volumeChanged("master", AudioBus::Master);
        return true;
    }

    bool onMusicChanged(GuiEvent e)
    {
        volumeChanged("music", AudioBus::Music);
        return true;
    }

    bool onEffectsChanged(GuiEvent e)
    {
        volumeChanged("effects", AudioBus::Effects);
        return true;
    }

    private void toTitle()
    {
        click();
        run::loadScene(kStart);
        run::setTimeScale(1.0f); // the title's street moves (an ended block stopped the clock)
        showTitle();
    }

    private void quit()
    {
        click();
        run::requestExit(0);
    }

    // ---- screens ----

    // The current block from its start: the score goes back to what was banked before it.
    private void startLevel()
    {
        m_score = m_banked;
        m_delivered = 0;
        screens().clear();
        screens().push(kHudDoc);
        setText("hud-score", "" + m_score);
        setText("hud-lives", "" + m_lives);
        m_endingIn = -1.0f;
        Audio::playMusic(m_blockMusic[m_level % int(m_blockMusic.length())], 0.8f, kMusicVolume);
        run::setTimeScale(1.0f);
        run::loadScene(m_levels[m_level]);
        m_phase = Phase::Playing;
    }

    private void pause()
    {
        run::setTimeScale(0.0f);
        screens().push(kPauseDoc);
        wire("resume-btn", "click", EventHandler(this.onResume));
        wire("restart-btn", "click", EventHandler(this.onRetry));
        wire("settings-btn", "click", EventHandler(this.onSettings));
        wire("menu-btn", "click", EventHandler(this.onToTitle));
        m_phase = Phase::Paused;
    }

    private void click()
    {
        Audio::playOneShot(kClickSound, AudioBus::Effects, 0.7f);
    }

    // A volume row: its slider at the bus's level, its readout, and the handler.
    private void bindVolume(string row, AudioBus bus, EventHandler@ handler)
    {
        Slider@ slider = Slider::of(find(row + "-slider"));
        slider.value = Audio::busVolume(bus);
        showVolume(row, float(slider.value));
        wire(row + "-slider", "input", handler); // each step as the slider moves
    }

    private void volumeChanged(string row, AudioBus bus)
    {
        float level = float(Slider::of(find(row + "-slider")).value);
        Audio::setBusVolume(bus, level);
        showVolume(row, level);
        Audio::playOneShot(kSliderSound, AudioBus::Effects, 0.6f); // hear the level being set
    }

    private void showVolume(string row, float level)
    {
        setText(row + "-value", "" + int(level * 100.0f + 0.5f) + "%");
    }

    private void showGameOver(bool won)
    {
        m_phase = Phase::Ended;
        Audio::stopMusic(0.3f);
        if (won)
        {
            Audio::playOneShot(kClearedFanfare, AudioBus::Music);
            Audio::playOneShot(kCongratulations, AudioBus::Effects);
        }
        else
        {
            Audio::playOneShot(kGameOverVoice, AudioBus::Effects);
        }
        m_endingIn = won ? 4.2f : 2.0f; // the ending's music once the fanfare or the voice is done
        screens().push(kGameOverDoc);
        setText("over-title", won ? "Route complete!" : "Game over");
        setText("over-score", "Score " + m_score);
        setText("over-reached", won ? "Every block delivered" : "Reached block " + (m_level + 1) + " of " + m_levels.length());
        wire("again-btn", "click", EventHandler(this.onNewGame));
        wire("menu-btn", "click", EventHandler(this.onToTitle));
    }

    private void showTitle()
    {
        m_phase = Phase::Title;
        m_endingIn = -1.0f;
        Audio::playMusic(kTitleMusic, 0.8f, kMusicVolume);
        screens().clear();
        screens().push(kTitleDoc);
        wire("play-btn", "click", EventHandler(this.onNewGame));
        wire("settings-btn", "click", EventHandler(this.onSettings));
        wire("quit-btn", "click", EventHandler(this.onQuit));
    }
}
