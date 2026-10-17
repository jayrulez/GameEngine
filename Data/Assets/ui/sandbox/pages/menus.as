// Menus and windows: the menus written in markup say what was picked. An item's `activate`
// bubbles, through its menu to what opened it, so one listener on the section hears them all;
// the event's id names the item.

funcdef bool EventHandler(GuiEvent e);

bool picked(GuiEvent e)
{
    Label::of(gui::root().find("menus-markup-said")).text = "Picked from markup: " + e.id;
    return true;
}

void main()
{
    gui::root().find("menus-markup").on("activate", EventHandler(picked));
}
