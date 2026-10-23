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

Guid kTitleDoc = Guid("{{Title}}");
Guid kHudDoc = Guid("{{Hud}}");
Guid kClearedDoc = Guid("{{Cleared}}");
Guid kFailedDoc = Guid("{{Failed}}");
Guid kGameOverDoc = Guid("{{GameOver}}");
Guid kPauseDoc = Guid("{{Pause}}");
Guid kSettingsDoc = Guid("{{Settings}}");

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
    private float m_pop = 0.0f;       // seconds left of the points rising from the score
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
        tickPop(dt);
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
            onResume();
        }
        else if (m_phase == Phase::Settings)
        {
            onSettingsBack();
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
        ui::findLabel("hud-score").setText("" + m_score);
        UILabel@ pop = ui::findLabel("hud-pop");
        pop.setText("+" + points);
        pop.setOpacity(1.0f);
        pop.setTranslation(kPopX, kPopY);
        pop.setVisible(true);
        m_pop = kPopSeconds;
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
        UILabel@ banner = ui::findLabel("hud-banner");
        banner.setOpacity(0.0f);
        banner.setVisible(true);
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

        UILabel@ banner = ui::findLabel("hud-banner");
        float drop = m_celebrated / kBannerDrop;
        if (drop > 1.0f)
        {
            drop = 1.0f;
        }
        banner.setOpacity(drop);
        banner.setTranslation(0.0f, -60.0f * (1.0f - drop) * (1.0f - drop));

        if (m_celebrated >= kCelebrateSeconds)
        {
            banner.setVisible(false);
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
        ui::findLabel("hud-score").setText("" + m_score);
        UIScreen@ s = ui::push(kClearedDoc);
        s.findLabel("cleared-summary").setText("" + m_delivered + " delivered, " + earned + " points + "
                                               + bonus + " time bonus");
        UIButton@ next = s.findButton("continue-btn");
        if (m_level + 1 >= int(m_levels.length()))
        {
            next.setText("Finish");
        }
        next.onClick(Action(this.onContinue));
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
        ui::findLabel("hud-lives").setText("" + m_lives);
        if (m_lives <= 0)
        {
            showGameOver(false);
            return;
        }
        UIScreen@ s = ui::push(kFailedDoc);
        s.findLabel("failed-reason").setText(reason == 0 ? "Out of time" : "Out of papers");
        s.findLabel("failed-lives").setText(m_lives == 1 ? "1 life left" : "" + m_lives + " lives left");
        s.findButton("retry-btn").onClick(Action(this.onRetry));
        s.findButton("menu-btn").onClick(Action(this.onToTitle));
    }

    // ---- buttons ----

    void onNewGame()
    {
        click();
        m_level = 0;
        m_lives = kStartLives;
        m_score = 0;
        m_banked = 0;
        startLevel();
    }

    void onRetry()
    {
        click();
        startLevel();
    }

    void onContinue()
    {
        click();
        m_level += 1;
        if (m_level >= int(m_levels.length()))
        {
            m_level = int(m_levels.length()) - 1;
            ui::pop(); // the Block cleared screen gives way to the summary
            showGameOver(true);
            return;
        }
        startLevel();
    }

    void onResume()
    {
        click();
        ui::pop(); // the pause menu
        run::setTimeScale(1.0f);
        m_phase = Phase::Playing;
    }

    void onSettings()
    {
        click();
        m_settingsFrom = m_phase;
        UIScreen@ s = ui::push(kSettingsDoc);
        bindVolume(s, "master", AudioBus::Master, Action(this.onMasterChanged));
        bindVolume(s, "music", AudioBus::Music, Action(this.onMusicChanged));
        bindVolume(s, "effects", AudioBus::Effects, Action(this.onEffectsChanged));
        s.findButton("settings-back-btn").onClick(Action(this.onSettingsBack));
        m_phase = Phase::Settings;
    }

    void onSettingsBack()
    {
        click();
        ui::pop(); // the settings screen; the title or the pause menu is under it
        m_phase = m_settingsFrom;
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
    }

    void onToTitle()
    {
        click();
        run::loadScene(kStart);
        run::setTimeScale(1.0f); // the title's street moves (an ended block stopped the clock)
        showTitle();
    }

    void onQuit()
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
        ui::clear();
        ui::push(kHudDoc);
        ui::findLabel("hud-score").setText("" + m_score);
        ui::findLabel("hud-lives").setText("" + m_lives);
        m_pop = 0.0f;
        m_endingIn = -1.0f;
        Audio::playMusic(m_blockMusic[m_level % int(m_blockMusic.length())], 0.8f, kMusicVolume);
        run::setTimeScale(1.0f);
        run::loadScene(m_levels[m_level]);
        m_phase = Phase::Playing;
    }

    private void pause()
    {
        run::setTimeScale(0.0f);
        UIScreen@ s = ui::push(kPauseDoc);
        s.findButton("resume-btn").onClick(Action(this.onResume));
        s.findButton("restart-btn").onClick(Action(this.onRetry));
        s.findButton("settings-btn").onClick(Action(this.onSettings));
        s.findButton("menu-btn").onClick(Action(this.onToTitle));
        m_phase = Phase::Paused;
    }

    private void click()
    {
        Audio::playOneShot(kClickSound, AudioBus::Effects, 0.7f);
    }

    // The delivery's points: up 36 px from under the score over the pop's time, fading out late.
    private void tickPop(float dt)
    {
        if (m_pop <= 0.0f)
        {
            return;
        }
        m_pop -= dt;
        UILabel@ pop = ui::findLabel("hud-pop");
        if (pop is null || !pop.isValid())
        {
            m_pop = 0.0f;
            return;
        }
        if (m_pop <= 0.0f)
        {
            pop.setVisible(false);
            return;
        }
        float t = 1.0f - m_pop / kPopSeconds;
        pop.setTranslation(kPopX, kPopY - 36.0f * t);
        pop.setOpacity(1.0f - t * t);
    }

    // A volume row: its slider at the bus's level, its readout, and the handler.
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
        Audio::playOneShot(kSliderSound, AudioBus::Effects, 0.6f); // hear the level being set
    }

    private void showVolume(string row, float level)
    {
        ui::findLabel(row + "-value").setText("" + int(level * 100.0f + 0.5f) + "%");
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
        UIScreen@ s = ui::push(kGameOverDoc);
        s.findLabel("over-title").setText(won ? "Route complete!" : "Game over");
        s.findLabel("over-score").setText("Score " + m_score);
        s.findLabel("over-reached").setText(won ? "Every block delivered"
                                                : "Reached block " + (m_level + 1) + " of " + m_levels.length());
        s.findButton("again-btn").onClick(Action(this.onNewGame));
        s.findButton("menu-btn").onClick(Action(this.onToTitle));
    }

    private void showTitle()
    {
        m_phase = Phase::Title;
        m_endingIn = -1.0f;
        Audio::playMusic(kTitleMusic, 0.8f, kMusicVolume);
        ui::clear();
        UIScreen@ s = ui::push(kTitleDoc);
        s.findButton("play-btn").onClick(Action(this.onNewGame));
        s.findButton("settings-btn").onClick(Action(this.onSettings));
        s.findButton("quit-btn").onClick(Action(this.onQuit));
    }
}
