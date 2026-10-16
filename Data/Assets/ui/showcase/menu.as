// The GUI lab's showcase (G6 of Documentation/Specs/vg-and-gui-lab.md): the menu's behaviour, in
// AngelScript, through the lab's reflection alone. The markup (menu.xml) holds the widgets and the
// sheet (menu.css) their looks; this script only switches pages, screens and the modal, binds two
// labels to functions of the GUI's data, and runs the loading bar. The host calls update(dt) while it returns true.

funcdef string TextExpression();
funcdef bool CommandHandler();

Node@ stage;         // the menu screen: the title and the slot
Node@ mainPanel;     // in the slot
Node@ settingsPanel; // in the slot, in the main menu's place
Node@ loadingScreen; // a screen of its own
Node@ modal;         // over everything, the quit question
bool loadingNow = false;
double loadTime = 0;

// Shows `node` and replays its entrance (`animationClass`; a node hidden earlier lost the class
// then, so adding it again starts the animation over).
void reveal(Node@ node, const string &in animationClass)
{
    node.visible = true;
    UIWidget::of(node).addClass(animationClass);
}
void conceal(Node@ node, const string &in animationClass)
{
    node.visible = false;
    UIWidget::of(node).removeClass(animationClass);
}

// A page of the menu in the slot under the title (the main menu or the settings).
void showPage(Node@ page)
{
    conceal(loadingScreen, "screen-in");
    conceal(modal, "modal-in");
    stage.visible = true;
    conceal(mainPanel, "enter");
    conceal(settingsPanel, "enter");
    reveal(page, "enter");
    page.focus(); // its remembered or default widget: the keyboard and a gamepad carry on there
}

string volumeText() { return "" + int(gui::getNumber("volume") * 100 + 0.5) + "%"; }
string musicText() { return gui::getBool("music") ? "On" : "Off"; }

void onPlay()
{
    gui::set("status", "Loading...");
    gui::set("progress", 0);
    loadTime = 0;
    loadingNow = true;
    stage.visible = false; // the loading screen replaces the menu
    reveal(loadingScreen, "screen-in");
    loadingScreen.focus();
}
void onSettings() { showPage(settingsPanel); }
void onBack() { loadingNow = false; showPage(mainPanel); }
// The quit question opens over the menu, which stays where it is behind the dimmed glass.
void onQuit()
{
    reveal(modal, "modal-in");
    Node@ confirm = gui::root().find("confirm");
    conceal(confirm, "enter");
    reveal(confirm, "enter");
    modal.focus(); // Stay, the safe answer, unless one was chosen before
}
void onStay()
{
    conceal(modal, "modal-in");
    mainPanel.focus(); // back to Quit, where the question came from
}

// Back (Escape, a gamepad's East): closes what is open, one step at a time.
bool onBackCommand()
{
    if (modal.visible)
        onStay();
    else if (loadingScreen.visible || settingsPanel.visible)
        onBack();
    return false;
}
void onLeave()
{
    // The whole menu fades out; the host closes once `quit` is set and the fade has run.
    UIWidget::of(gui::root().find("showcase")).addClass("leaving");
    gui::set("quit", true);
}

void main()
{
    Node@ root = gui::root();
    @stage = root.find("stage");
    @mainPanel = root.find("main");
    @settingsPanel = root.find("settings");
    @loadingScreen = root.find("loading");
    @modal = root.find("modal");

    root.find("play").on("click", Action(onPlay));
    root.find("settings-btn").on("click", Action(onSettings));
    root.find("quit").on("click", Action(onQuit));
    root.find("settings-back").on("click", Action(onBack));
    root.find("loading-back").on("click", Action(onBack));
    root.find("confirm-yes").on("click", Action(onLeave));
    root.find("confirm-no").on("click", Action(onStay));

    // Labels that follow the data through functions of it: no handler updates them.
    root.find("volume-text").bind("text", TextExpression(volumeText));
    root.find("music-text").bind("text", TextExpression(musicText));

    // Played by keyboard and gamepad too: Back is Escape and the East button.
    root.onCommand("back", CommandHandler(onBackCommand));
    gui::bindKey("escape", "back", "");
    gui::bindKey("pad-east", "back", "");

    showPage(mainPanel);
}

// The loading bar fills over two and a half seconds; true while there is more to do.
bool update(double dt)
{
    if (!loadingNow)
        return false;
    loadTime += dt;
    double progress = loadTime / 2.5;
    if (progress >= 1)
    {
        progress = 1;
        loadingNow = false;
        gui::set("status", "Ready");
    }
    gui::set("progress", progress);
    return loadingNow;
}
