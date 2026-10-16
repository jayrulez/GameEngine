// Controls: the buttons count into the GUI's data (a label shows it through {{clicks}}); the bar and
// the number follow the slider, and the echo the text field, through bindings, with no handlers.

funcdef string TextExpression();
funcdef double NumberExpression();

Slider@ slider;
TextField@ field;

void add1() { gui::set("clicks", gui::getNumber("clicks") + 1); }
void add5() { gui::set("clicks", gui::getNumber("clicks") + 5); }
void reset() { gui::set("clicks", 0); }

double sliderValue() { return slider.value; }
string sliderText() { return "" + int(slider.value * 100 + 0.5) + "%"; }
string echoText() { return field.text == "" ? "(the echo follows the field)" : "echo: " + field.text; }

void main()
{
    Node@ root = gui::root();
    root.find("controls-add").on("click", Action(add1));
    root.find("controls-add5").on("click", Action(add5));
    root.find("controls-reset").on("click", Action(reset));
    @slider = Slider::of(root.find("controls-slider"));
    @field = TextField::of(root.find("controls-field"));
    root.find("controls-progress").bind("progress", NumberExpression(sliderValue));
    root.find("controls-slider-value").bind("text", TextExpression(sliderText));
    root.find("controls-echo").bind("text", TextExpression(echoText));
}
