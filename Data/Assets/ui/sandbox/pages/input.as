// Input: the clickable card counts the clicks that reach it (the button's stop at the button);
// the pad shows where it was pressed and how many presses of a run that was; the glass over a
// button lets clicks through (pointer-events: none); the canvas says what was dropped on it.
funcdef bool EventHandler(GuiEvent e);
bool onCard(GuiEvent e) { gui::set("cardClicks", gui::getNumber("cardClicks") + 1); return false; }
bool onCardButton(GuiEvent e) { gui::set("buttonClicks", gui::getNumber("buttonClicks") + 1); return false; }
bool onPad(GuiEvent e)
{
    gui::set("padX", int(e.localX));
    gui::set("padY", int(e.localY));
    gui::set("padClicks", e.clickCount);
    return true;
}
bool onGlass(GuiEvent e) { gui::set("glassClicks", gui::getNumber("glassClicks") + 1); return false; }
bool onPaint(GuiEvent e) { gui::set("paint", e.text + " (" + e.effect + ")"); return true; }
void main()
{
    gui::set("cardClicks", 0);
    gui::set("buttonClicks", 0);
    gui::set("padX", 0);
    gui::set("padY", 0);
    gui::set("padClicks", 0);
    gui::set("glassClicks", 0);
    gui::set("paint", "nothing yet");
    Node@ root = gui::root();
    root.find("input-click-card").on("click", EventHandler(onCard));
    root.find("input-card-button").on("click", EventHandler(onCardButton));
    root.find("input-pad").on("mousedown", EventHandler(onPad));
    root.find("input-under-glass").on("click", EventHandler(onGlass));
    root.find("input-canvas").on("drop", EventHandler(onPaint));
}
