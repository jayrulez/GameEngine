// The GUI lab's showcase (G6 of Documentation/Specs/vg-and-gui-lab.md): the menu's behaviour, in
// AngelScript, through the lab's reflection alone. The markup (menu.xml) holds the widgets and the
// sheet (menu.css) their looks; this script only switches pages, screens and the modal, binds two
// labels to functions of the GUI's data, and runs the loading bar and the modal's fade-out. The host
// calls update(dt) while it returns true.

funcdef string TextExpression();
funcdef bool CommandHandler();

Node@ stage;         // the menu screen: the title and the slot
Node@ mainPanel;     // in the slot
Node@ settingsPanel; // in the slot, in the main menu's place
Node@ loadingScreen; // a screen of its own
Node@ modal;         // over everything, the quit question
bool loadingNow = false;
double loadTime = 0;
double modalClosing = -1; // seconds left of the modal's fade-out (-1: not closing)
const double kModalCloseTime = 0.25;

// Shows `node` and replays its entrance (`animationClass`; a node hidden earlier lost the class
// then, so adding it again starts the animation over).
void reveal(Node@ node, const string &in animationClass)
{
    node.displayed = true;
    UIWidget::of(node).addClass(animationClass);
}
void conceal(Node@ node, const string &in animationClass)
{
    node.displayed = false;
    UIWidget::of(node).removeClass(animationClass);
}

// === Pages ===
// A change of page is two steps: the page on show sinks out (the "leave" class), then the next rises
// in (its own entrance), so pages sharing the slot never stack. Input mid-change is ignored until the
// next page is up, and focus moves there with it.
Node@ leaving;            // the page or screen sinking out
string leavingEntrance;   // its entrance class, removed with it
Node@ arriving;           // what comes next: a page of the menu, or the loading screen
double leaveLeft = -1;    // seconds left of the sink (-1: no change under way)
const double kLeaveTime = 0.2; // the sheet's .leave

bool changing() { return leaveLeft >= 0; }

// What is on show now: the loading screen, or the page in the slot.
Node@ current()
{
    if (loadingScreen.displayed)
        return loadingScreen;
    return mainPanel.displayed ? mainPanel : settingsPanel;
}

// Starts the change to `next` (a page, or the loading screen).
void changeTo(Node@ next)
{
    if (changing())
        return;
    Node@ from = current();
    if (from is next)
        return;
    @leaving = from;
    leavingEntrance = (from is loadingScreen) ? "screen-in" : "enter";
    UIWidget::of(from).removeClass(leavingEntrance); // its entrance is over; the sink takes over
    UIWidget::of(from).addClass("leave");
    @arriving = next;
    leaveLeft = kLeaveTime;
}

// The sink ended: the old one goes, the next one comes in.
void arrive()
{
    conceal(leaving, leavingEntrance);
    UIWidget::of(leaving).removeClass("leave");
    leaveLeft = -1;
    if (arriving is loadingScreen)
    {
        gui::set("status", "Loading...");
        gui::set("progress", 0);
        loadTime = 0;
        loadingNow = true;
        stage.displayed = false; // the loading screen replaces the menu
        reveal(loadingScreen, "screen-in");
        loadingScreen.focus();
        return;
    }
    stage.displayed = true;
    reveal(arriving, "enter");
    arriving.focus(); // its remembered or default widget: the keyboard and a gamepad carry on there
}

string volumeText() { return "" + int(gui::getNumber("volume") * 100 + 0.5) + "%"; }
string musicText() { return gui::getBool("music") ? "On" : "Off"; }

void onPlay() { changeTo(loadingScreen); }
void onSettings() { changeTo(settingsPanel); }
void onBack() { loadingNow = false; changeTo(mainPanel); }
// The quit question opens over the menu, which stays where it is behind the dimmed glass.
void onQuit()
{
    // (Re)opening, a fade-out in progress included: its closing classes go.
    modalClosing = -1;
    UIWidget::of(modal).removeClass("modal-out");
    reveal(modal, "modal-in");
    Node@ confirm = gui::root().find("confirm");
    UIWidget::of(confirm).removeClass("leave");
    conceal(confirm, "enter");
    reveal(confirm, "enter");
    modal.focus(); // Stay, the safe answer, unless one was chosen before
}
// Stay (and Back): the modal fades out, then hides (update counts it down); focus is back on Quit
// at once.
void onStay()
{
    if (modalClosing >= 0)
        return; // already closing
    UIWidget::of(modal).removeClass("modal-in");
    UIWidget::of(modal).addClass("modal-out");
    UIWidget::of(gui::root().find("confirm")).addClass("leave");
    modalClosing = kModalCloseTime;
    mainPanel.focus();
}

// Back (Escape, a gamepad's East): closes what is open, one step at a time.
bool onBackCommand()
{
    if (modal.displayed && modalClosing < 0)
        onStay();
    else if (modal.displayed)
        return false; // closing already
    else if (!changing() && (loadingScreen.displayed || settingsPanel.displayed))
        onBack();
    return false;
}
void onLeave()
{
    // The whole menu fades out; once `quit` is set and the fade has run, the host takes the player
    // back to where they came from (the sandbox).
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

    enter();
}

// The menu as it opens (the host calls this each time it shows the menu): the main page rising in,
// the rest hidden, nothing left over from the last visit (a fade, a page change, the loading bar).
void enter()
{
    leaveLeft = -1;
    modalClosing = -1;
    loadingNow = false;
    UIWidget::of(gui::root().find("showcase")).removeClass("leaving");
    UIWidget::of(modal).removeClass("modal-out");
    UIWidget::of(gui::root().find("confirm")).removeClass("leave");
    conceal(modal, "modal-in");
    conceal(settingsPanel, "enter");
    UIWidget::of(settingsPanel).removeClass("leave");
    conceal(loadingScreen, "screen-in");
    UIWidget::of(loadingScreen).removeClass("leave");
    UIWidget::of(mainPanel).removeClass("leave");
    stage.displayed = true;
    conceal(mainPanel, "enter");
    reveal(mainPanel, "enter");
    mainPanel.focus();
}

// The loading bar fills over two and a half seconds; true while there is more to do.
bool update(double dt)
{
    if (leaveLeft >= 0)
    {
        leaveLeft -= dt;
        if (leaveLeft < 0)
            arrive();
    }
    if (modalClosing >= 0)
    {
        modalClosing -= dt;
        if (modalClosing < 0)
        {
            conceal(modal, "modal-out");
            UIWidget::of(gui::root().find("confirm")).removeClass("leave");
        }
    }
    if (!loadingNow)
        return modalClosing >= 0 || leaveLeft >= 0;
    loadTime += dt;
    double progress = loadTime / 2.5;
    if (progress >= 1)
    {
        progress = 1;
        loadingNow = false;
        gui::set("status", "Ready");
    }
    gui::set("progress", progress);
    return loadingNow || modalClosing >= 0 || leaveLeft >= 0;
}
