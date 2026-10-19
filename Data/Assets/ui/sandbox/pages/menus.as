// Menus and windows: the menus written in markup say what was picked. An item's `activate`
// bubbles, through its menu to what opened it, so one listener on the section hears them all;
// the event's id names the item. And what the script builds itself (E16): a menu of sixty items,
// longer than the window, which scrolls, and a window of its own.

funcdef bool EventHandler(GuiEvent e);

bool picked(GuiEvent e)
{
    Label::of(gui::root().find("menus-markup-said")).text = "Picked from markup: " + e.id;
    return true;
}

Node@ longMenu;
Node@ scriptWindow;

bool pickedLong(GuiEvent e)
{
    Label::of(gui::root().find("menus-long-said")).text = "Picked from the long menu: " + e.id;
    return true;
}

bool openLong(GuiEvent e)
{
    Menu::of(longMenu).openBelow(gui::root().find("menus-long"));
    return true;
}

// A window of the script's own, built when asked (a window closed by its button is gone).
bool openWindow(GuiEvent e)
{
    if (scriptWindow is null || !scriptWindow.isValid())
    {
        @scriptWindow = gui::root().add("Window"); // over the whole GUI, centred in it
        scriptWindow.width = 320;
        scriptWindow.height = 160;
        Window::of(scriptWindow).title = "From menus.as";
        Node@ text = scriptWindow.add("Label");
        Label::of(text).text = "The page's script built this window and its text.";
        UIWidget::of(text).addClass("text");
    }
    if (!Window::of(scriptWindow).isOpen())
        Window::of(scriptWindow).open();
    return true;
}

void main()
{
    Node@ root = gui::root();
    root.find("menus-markup").on("activate", EventHandler(picked));

    // A menu longer than the window: it scrolls.
    @longMenu = root.find("menus-long").add("Menu");
    for (int i = 1; i <= 60; ++i)
    {
        Node@ item = longMenu.add("MenuItem");
        MenuItem::of(item).text = "Item " + i;
        UIWidget::of(item).id = "item " + i;
    }
    longMenu.on("activate", EventHandler(pickedLong));
    root.find("menus-long").on("click", EventHandler(openLong));

    root.find("menus-script-window").on("click", EventHandler(openWindow));
}
