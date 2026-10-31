// The GUI showcase: a game menu's behaviour, written in AngelScript using only the GUI's
// reflection, on top of the gamekit. The markup holds the widgets (menu.xml for the frame,
// screens/ for one document per screen), and the style sheet (menu.css) holds their looks,
// entrances and exits. This script pushes and pops the screens on the gamekit's stack, wires each
// screen's buttons every time it is pushed, and runs the loading bar. Once loading is done it runs
// a stand-in game under the HUD: the score counting up, health draining and healing, the run's
// clock, and a pause screen on Escape or a pad's East or Start. Back (Escape, East) is handled by
// the stack, which pops the top screen; the title screen stays. The host calls update(dt) for as
// long as it returns true.

funcdef string TextExpression();
funcdef bool CommandHandler();
funcdef bool EventHandler(GuiEvent e);

const string kScreens = "Samples/UI/showcase/screens/";

ScreenStack@ stack; // the game's screens
Node@ stage;        // the title over the menu's pages
Screen@ hud;        // the game's HUD while a game runs (null otherwise)
bool loadingNow = false;
double loadTime = 0;
double untilGame = -1; // seconds from "Ready" to the HUD (-1 when not waiting)
double gameTime = 0;
double nextPoint = 0;
int score = 0;
bool popTurn = false;

Screen@ push(const string &in name) { return stack.push(kScreens + name + ".xml"); }
void popScreen() { stack.pop(); }

// === The menu ===

void showTitle()
{
    stage.displayed = true;
    Screen@ title = push("title");
    title.find("play").on("click", Action(onPlay));
    title.find("settings-btn").on("click", Action(onSettings));
    title.find("controls-btn").on("click", Action(onControls));
    title.find("quit").on("click", Action(onQuit));
}

string volumeText() { return "" + int(gui::getNumber("volume") * 100 + 0.5) + "%"; }
string musicText() { return gui::tr(gui::getBool("music") ? "settings.on" : "settings.off"); }

void onSettings()
{
    Screen@ page = push("settings");
    page.find("settings-back").on("click", Action(popScreen));
    // Labels that follow the data through functions of it: no handler updates them.
    page.find("volume-text").bind("text", TextExpression(volumeText));
    page.find("music-text").bind("text", TextExpression(musicText));
}

// === Controls ===
// Each action's key, bound to its command in the game's key context. Choosing an action listens
// for the next key or pad button (gui::captureKey, which sees it before the keymap and navigation
// do). If another action already had that key, the two swap; Escape cancels.
array<string> actions = {"jump", "dash", "interact", "map"};
array<string> keys = {"space", "leftshift", "e", "m"};
int listening = -1; // the action waiting for its key (-1 for none)
Screen@ controls;

void onControls()
{
    @controls = push("controls");
    controls.find("bind-jump").on("click", Action(onBindJump));
    controls.find("bind-dash").on("click", Action(onBindDash));
    controls.find("bind-interact").on("click", Action(onBindInteract));
    controls.find("bind-map").on("click", Action(onBindMap));
    controls.find("controls-back").on("click", Action(popScreen));
    gui::set("controls-hint", gui::tr("controls.choose"));
    showKeys();
}
void showKeys()
{
    for (uint i = 0; i < actions.length(); i++)
    {
        gui::set("key-" + actions[i], gui::keyText(keys[i]));
        gui::bindKey(keys[i], "game." + actions[i], "game");
    }
}
void listen(int action)
{
    if (listening >= 0)
        return;
    listening = action;
    UIWidget::of(controls.find("bind-" + actions[action])).addClass("listening");
    gui::set("key-" + actions[action], gui::tr("controls.press"));
    gui::set("controls-hint", gui::tr("controls.keep") + gui::keyText(keys[action]));
    gui::captureKey(KeyHandler(onCapturedKey));
}
void onCapturedKey(const string &in pressed)
{
    const int action = listening;
    listening = -1;
    UIWidget::of(controls.find("bind-" + actions[action])).removeClass("listening");
    gui::set("controls-hint", gui::tr("controls.choose"));
    if (pressed != "")
    {
        for (uint i = 0; i < actions.length(); i++)
            if (int(i) != action && keys[i] == pressed)
            {
                keys[i] = keys[action]; // the key was taken, so the two swap
                gui::set("controls-hint", gui::tr("controls.swapped") + gui::tr("controls." + actions[i]));
            }
        gui::bindKey(keys[action], "", "game"); // unbind the old key in that context
        keys[action] = pressed;
    }
    showKeys();
}
void onBindJump() { listen(0); }
void onBindDash() { listen(1); }
void onBindInteract() { listen(2); }
void onBindMap() { listen(3); }

// === Loading ===

void onPlay()
{
    Screen@ loading = push("loading");
    loading.find("loading-back").on("click", Action(popScreen));
    loading.on("screenleave", EventHandler(onLoadingLeave)); // Back, or the game starting
    stage.displayed = false; // the loading screen replaces the menu
    gui::set("status", gui::tr("loading.loading"));
    gui::set("progress", 0);
    loadTime = 0;
    loadingNow = true;
}
bool onLoadingLeave(GuiEvent e)
{
    loadingNow = false;
    untilGame = -1;
    stage.displayed = hud is null; // back on the menu, unless the game took over
    return false;
}

// === Quit ===
// The question opens over the menu, which stays in place behind the dimmed glass. Stay and Back pop
// it, returning focus to Quit.
void onQuit()
{
    Screen@ question = push("quit");
    question.find("confirm-yes").on("click", Action(onLeave));
    question.find("confirm-no").on("click", Action(popScreen));
}
void onLeave()
{
    // The whole menu fades out; once `quit` is set and the fade has run, the host takes the player
    // back to where they came from (the sandbox).
    UIWidget::of(gui::root().find("showcase")).addClass("closing");
    gui::set("quit", true);
}

// === The game ===

void startGame()
{
    untilGame = -1;
    score = 0;
    gameTime = 0;
    nextPoint = 1.2;
    gui::set("score", 0);
    gui::set("health", 1.0);
    gui::set("clock", "0:00");
    @hud = null;
    stack.clear(); // the menu and the loading screen leave as the HUD comes in
    @hud = push("hud");
    stage.displayed = false;
    hud.onCommand("back", CommandHandler(onPauseCommand)); // in the game, Back pauses
}
// Pause (Back in the game, and Start): opens the pause screen over the HUD, or closes it.
bool onPauseCommand()
{
    if (hud is null)
        return true; // not in a game, so the command belongs to someone else
    if (stack.isPaused())
        stack.pop();
    else
    {
        Screen@ pause = push("pause");
        pause.find("resume").on("click", Action(popScreen));
        pause.find("to-menu").on("click", Action(onToMenu));
    }
    return false;
}
void onToMenu() { enter(); }

// Advances the stand-in game: a point now and then (the number counts up and pops), and health
// draining, warning when low, until the next point heals it.
void play(double dt)
{
    gameTime += dt;
    int seconds = int(gameTime);
    gui::set("clock", "" + (seconds / 60) + ":" + ((seconds % 60) < 10 ? "0" : "") + (seconds % 60));
    double health = gui::getNumber("health") - dt * 0.06;
    nextPoint -= dt;
    if (nextPoint <= 0)
    {
        nextPoint = 1.4;
        score += 10;
        gui::tween("score", score, 0.35, "ease-out").whole(true); // the data itself counts up in whole numbers; no widget drives it
        UIWidget@ number = UIWidget::of(hud.find("hud-score"));
        number.removeClass(popTurn ? "pop-a" : "pop-b");
        number.addClass(popTurn ? "pop-b" : "pop-a");
        popTurn = !popTurn;
        if (health < 0.3)
            health = 1.0; // a light gathered heals
    }
    if (health < 0)
        health = 0;
    gui::set("health", health);
    UIWidget@ bar = UIWidget::of(hud.find("hud-health"));
    if (health < 0.3)
        bar.addClass("hp-low");
    else
        bar.removeClass("hp-low");
}

void main()
{
    Node@ root = gui::root();
    @stack = ScreenStack::of(root.find("screens"));
    @stage = root.find("stage");
    // Start pauses in the game. The handler sits on the menu itself because the menu is a modal
    // layer of the sandbox's root, and a command from inside it stops at its edge.
    root.find("showcase").onCommand("pause", CommandHandler(onPauseCommand));
    gui::bindKey("pad-start", "pause", "");
    enter();
}

// Resets the menu as it opens (the host calls this each time it shows the menu): only the title
// screen, with nothing left over from the last visit.
void enter()
{
    loadingNow = false;
    untilGame = -1;
    @hud = null;
    UIWidget::of(gui::root().find("showcase")).removeClass("closing");
    stack.clear();
    showTitle();
}

// The loading bar fills over two and a half seconds, then the game runs while it is not paused.
// Returns true while there is more to do.
bool update(double dt)
{
    if (untilGame >= 0)
    {
        untilGame -= dt;
        if (untilGame < 0)
            startGame();
    }
    if (hud !is null && !stack.isPaused())
        play(dt);
    if (loadingNow)
    {
        loadTime += dt;
        double progress = loadTime / 2.5;
        if (progress >= 1)
        {
            progress = 1;
            loadingNow = false;
            gui::set("status", gui::tr("loading.ready"));
            untilGame = 0.6; // a moment on "Ready", then the game
        }
        gui::set("progress", progress);
    }
    return loadingNow || untilGame >= 0 || hud !is null;
}
