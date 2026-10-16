// The GUI lab's showcase (G6 of Documentation/Specs/vg-and-gui-lab.md): the menu's behaviour, in
// AngelScript, through the lab's reflection alone. The markup (menu.xml) holds the widgets and the
// sheet (menu.css) their looks; this script only switches panels, binds two labels to functions of
// the GUI's data, and runs the loading bar. The host calls update(dt) while it returns true.

funcdef string TextExpression();

Node@ mainPanel;
Node@ settingsPanel;
Node@ loadingPanel;
Node@ confirmPanel;
bool loadingNow = false;
double loadTime = 0;

// Shows one panel, replaying its entrance (a panel hidden earlier lost its "enter" class then, so
// adding it again starts the animation over).
void hide(Node@ panel)
{
    panel.visible = false;
    UIWidget::of(panel).removeClass("enter");
}

void show(Node@ panel)
{
    hide(mainPanel);
    hide(settingsPanel);
    hide(loadingPanel);
    hide(confirmPanel);
    panel.visible = true;
    UIWidget::of(panel).addClass("enter");
}

string volumeText() { return "" + int(gui::getNumber("volume") * 100 + 0.5) + "%"; }
string musicText() { return gui::getBool("music") ? "On" : "Off"; }

void onPlay()
{
    gui::set("status", "Loading...");
    gui::set("progress", 0);
    loadTime = 0;
    loadingNow = true;
    show(loadingPanel);
}
void onSettings() { show(settingsPanel); }
void onBack() { loadingNow = false; show(mainPanel); }
void onQuit() { show(confirmPanel); }
void onLeave()
{
    // The whole menu fades out; the host closes once `quit` is set and the fade has run.
    UIWidget::of(gui::root().find("showcase")).addClass("leaving");
    gui::set("quit", true);
}

void main()
{
    Node@ root = gui::root();
    @mainPanel = root.find("main");
    @settingsPanel = root.find("settings");
    @loadingPanel = root.find("loading");
    @confirmPanel = root.find("confirm");

    root.find("play").on("click", Action(onPlay));
    root.find("settings-btn").on("click", Action(onSettings));
    root.find("quit").on("click", Action(onQuit));
    root.find("settings-back").on("click", Action(onBack));
    root.find("loading-back").on("click", Action(onBack));
    root.find("confirm-yes").on("click", Action(onLeave));
    root.find("confirm-no").on("click", Action(onBack));

    // Labels that follow the data through functions of it: no handler updates them.
    root.find("volume-text").bind("text", TextExpression(volumeText));
    root.find("music-text").bind("text", TextExpression(musicText));

    show(mainPanel);
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
