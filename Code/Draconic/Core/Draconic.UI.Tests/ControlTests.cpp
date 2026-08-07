// Ported from Sedulous.UI.Tests/src/ControlTests.bf - the Button/RepeatButton/CheckBox subset (the
// controls ported so far) + the Button/CheckBox OnActivate cases from DirectionalFocusTests. Other
// controls (Label/ToggleButton/Slider/...) land in later batches. Text rendering is deferred in the
// controls, but every tested behavior (state/events/toggle/measure fallback) is exercised here.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<Button> MakeButton(StringView t)
{
    return foundation::MakeRef<Button>(foundation::DefaultAllocator(), t);
}
static foundation::RefPtr<CheckBox> MakeCheckBox(StringView t)
{
    return foundation::MakeRef<CheckBox>(foundation::DefaultAllocator(), t);
}

// === Button ===

TEST_CASE("control: Button_PressedTransitions")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = MakeButton(u8"Test");
    root->AddView(btn.Get());
    LayoutPass(ctx, root.Get());

    MouseEventArgs downArgs;
    downArgs.Set(10, 10, MouseButton::Left);
    btn->OnMouseDown(downArgs);
    CHECK(btn->IsPressed());

    MouseEventArgs upArgs;
    upArgs.Set(10, 10, MouseButton::Left);
    btn->OnMouseUp(upArgs);
    CHECK(!btn->IsPressed());
}

TEST_CASE("control: Button_DisabledDoesNotClick")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = MakeButton(u8"Test");
    btn->IsEnabled = false;
    root->AddView(btn.Get());
    bool clicked = false;
    btn->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    btn->FireClick();
    CHECK(!clicked);
}

TEST_CASE("control: Button_KeyboardActivation")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = MakeButton(u8"Test");
    root->AddView(btn.Get());
    bool clicked = false;
    btn->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    KeyEventArgs args;
    args.Set(KeyCode::Return, KeyModifiers::None, false);
    btn->OnKeyDown(args);
    CHECK(clicked);
}

TEST_CASE("control: Button_IsFocusable")
{
    auto btn = MakeButton(u8"Test");
    CHECK(btn->IsFocusable);
    CHECK(btn->IsTabStop);
}

TEST_CASE("control: Button_ControlState_Pressed")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = MakeButton(u8"Test");
    root->AddView(btn.Get());
    MouseEventArgs args;
    args.Set(10, 10, MouseButton::Left);
    btn->OnMouseDown(args);
    CHECK(HasFlag(btn->GetControlState(), ControlState::Pressed));
}

TEST_CASE("control: Button_OnActivate_FiresClick")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = MakeButton(u8"Test");
    root->AddView(btn.Get());
    bool clicked = false;
    btn->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    btn->OnActivate();
    CHECK(clicked);
}

// === IconButton ===

TEST_CASE("control: IconButton_MeasuresToFixedSize")
{
    auto btn = foundation::MakeRef<IconButton>(foundation::DefaultAllocator(), nullptr, 24.0f);
    btn->Measure(BoxConstraints(0, 100, 0, 100));
    CHECK(btn->MeasuredSize.x == doctest::Approx(24.0f));
    CHECK(btn->MeasuredSize.y == doctest::Approx(24.0f));
}

TEST_CASE("control: IconButton_Clicks")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = foundation::MakeRef<IconButton>(foundation::DefaultAllocator(), nullptr, 20.0f);
    root->AddView(btn.Get());
    bool clicked = false;
    btn->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    btn->FireClick();
    CHECK(clicked);
}

TEST_CASE("control: IconButton_PressedTransitions")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = foundation::MakeRef<IconButton>(foundation::DefaultAllocator(), nullptr, 20.0f);
    root->AddView(btn.Get());
    MouseEventArgs down;
    down.Set(5, 5, MouseButton::Left);
    btn->OnMouseDown(down);
    CHECK(btn->IsPressed());
    MouseEventArgs up;
    up.Set(5, 5, MouseButton::Left);
    btn->OnMouseUp(up);
    CHECK(!btn->IsPressed());
}

TEST_CASE("control: IconButton_IsFocusable")
{
    auto btn = foundation::MakeRef<IconButton>(foundation::DefaultAllocator(), nullptr);
    CHECK(btn->IsFocusable);
}

// === RepeatButton ===

TEST_CASE("control: RepeatButton_ClicksOnce")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = foundation::MakeRef<RepeatButton>(foundation::DefaultAllocator(), StringView(u8"Hold"));
    root->AddView(btn.Get());
    int clickCount = 0;
    btn->OnClick.Add([&clickCount](ButtonBase*) { ++clickCount; });
    KeyEventArgs args;
    args.Set(KeyCode::Return, KeyModifiers::None, false);
    btn->OnKeyDown(args);
    CHECK(clickCount == 1);
}

TEST_CASE("control: RepeatButton_RepeatsOnHold")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = foundation::MakeRef<RepeatButton>(foundation::DefaultAllocator(), StringView(u8"Hold"));
    btn->RepeatDelay = 0.1f;
    btn->RepeatInterval = 0.05f;
    root->AddView(btn.Get());
    int clickCount = 0;
    btn->OnClick.Add([&clickCount](ButtonBase*) { ++clickCount; });

    MouseEventArgs downArgs;
    downArgs.Set(10, 10, MouseButton::Left);
    btn->OnMouseDown(downArgs);

    btn->UpdateRepeat(0.05f);
    CHECK(clickCount == 0);
    btn->UpdateRepeat(0.06f); // total 0.11 > 0.1
    CHECK(clickCount >= 1);
    const int countBefore = clickCount;
    btn->UpdateRepeat(0.1f);
    CHECK(clickCount > countBefore);
}

TEST_CASE("control: RepeatButton_StopsOnRelease")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = foundation::MakeRef<RepeatButton>(foundation::DefaultAllocator(), StringView(u8"Hold"));
    btn->RepeatDelay = 0.05f;
    btn->RepeatInterval = 0.02f;
    root->AddView(btn.Get());
    int clickCount = 0;
    btn->OnClick.Add([&clickCount](ButtonBase*) { ++clickCount; });

    MouseEventArgs downArgs;
    downArgs.Set(10, 10, MouseButton::Left);
    btn->OnMouseDown(downArgs);
    MouseEventArgs upArgs;
    upArgs.Set(10, 10, MouseButton::Left);
    btn->OnMouseUp(upArgs);

    const int countAfterRelease = clickCount;
    btn->UpdateRepeat(0.2f);
    CHECK(clickCount == countAfterRelease);
}

// === CheckBox ===

TEST_CASE("control: CheckBox_Toggle")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto cb = MakeCheckBox(u8"Option");
    root->AddView(cb.Get());
    CHECK(!cb->IsChecked.Value());
    bool fired = false, newVal = false;
    cb->OnCheckedChanged.Add(
        [&](CheckBox*, bool val)
        {
            fired = true;
            newVal = val;
        });
    cb->IsChecked.SetValue(true);
    CHECK(fired);
    CHECK(newVal == true);
    CHECK(cb->IsChecked.Value());
}

TEST_CASE("control: CheckBox_MouseToggle")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto cb = MakeCheckBox(u8"Option");
    root->AddView(cb.Get());
    MouseEventArgs args;
    args.Set(5, 5, MouseButton::Left);
    cb->OnMouseDown(args);
    CHECK(cb->IsChecked.Value());
    MouseEventArgs args2;
    args2.Set(5, 5, MouseButton::Left);
    cb->OnMouseDown(args2);
    CHECK(!cb->IsChecked.Value());
}

TEST_CASE("control: CheckBox_NoChangeNotifyOnSameValue")
{
    auto cb = foundation::MakeRef<CheckBox>(foundation::DefaultAllocator(), StringView(u8"Test"), true);
    int fireCount = 0;
    cb->OnCheckedChanged.Add([&fireCount](CheckBox*, bool) { ++fireCount; });
    cb->IsChecked.SetValue(true); // same value
    CHECK(fireCount == 0);
}

TEST_CASE("control: CheckBox_OnActivate_Toggles")
{
    auto cb = MakeCheckBox(u8"Test");
    CHECK(!cb->IsChecked.Value());
    cb->OnActivate();
    CHECK(cb->IsChecked.Value());
    cb->OnActivate();
    CHECK(!cb->IsChecked.Value());
}

// === Label ===

TEST_CASE("control: Label_SetText")
{
    auto label = foundation::MakeRef<Label>(foundation::DefaultAllocator(), StringView(u8"Hello"));
    CHECK(label->Text.Value() == StringView(u8"Hello"));
}

TEST_CASE("control: Label_SetTextChaining")
{
    auto label = foundation::MakeRef<Label>(foundation::DefaultAllocator());
    label->SetText(u8"World");
    CHECK(label->Text.Value() == StringView(u8"World"));
}

TEST_CASE("control: Label_MeasuresNonZero")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto label = foundation::MakeRef<Label>(foundation::DefaultAllocator(), StringView(u8"Hello"));
    root->AddView(label.Get());
    LayoutPass(ctx, root.Get());
    CHECK(label->MeasuredSize.y > 0);
}

// === Spacer ===

TEST_CASE("control: Spacer_MeasuresToDesiredSize")
{
    auto spacer = foundation::MakeRef<Spacer>(foundation::DefaultAllocator(), 20.0f, 10.0f);
    spacer->Measure(BoxConstraints::Expand());
    CHECK(spacer->MeasuredSize.x == 20);
    CHECK(spacer->MeasuredSize.y == 10);
}

// === ColorView ===

TEST_CASE("control: ColorView_StoresColor")
{
    auto cv =
        foundation::MakeRef<ColorView>(foundation::DefaultAllocator(), foundation::Color{1.0f, 0.0f, 0.0f, 1.0f});
    CHECK(cv->Color.Value().r == 1.0f);
    CHECK(cv->Color.Value().g == 0.0f);
}

// === Separator ===

TEST_CASE("control: Separator_HorizontalMeasure")
{
    auto sep = foundation::MakeRef<Separator>(foundation::DefaultAllocator(), Orientation::Horizontal);
    sep->Measure(BoxConstraints::Loose(400, 300));
    CHECK(sep->MeasuredSize.y == 1);
    CHECK(sep->MeasuredSize.x == 400);
}

TEST_CASE("control: Separator_VerticalMeasure")
{
    auto sep = foundation::MakeRef<Separator>(foundation::DefaultAllocator(), Orientation::Vertical);
    sep->Measure(BoxConstraints::Loose(400, 300));
    CHECK(sep->MeasuredSize.x == 1);
    CHECK(sep->MeasuredSize.y == 300);
}

// === ProgressBar ===

TEST_CASE("control: ProgressBar_ValueClamped")
{
    auto bar = foundation::MakeRef<ProgressBar>(foundation::DefaultAllocator());
    bar->Value.SetValue(0.5f);
    CHECK(bar->Value.Value() == 0.5f);
    bar->Value.SetValue(-1.0f);
    CHECK(bar->Value.Value() == 0.0f);
    bar->Value.SetValue(2.0f);
    CHECK(bar->Value.Value() == 1.0f);
}

// === Panel ===

TEST_CASE("control: Panel_ChildFillsContent")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto panel = foundation::MakeRef<Panel>(foundation::DefaultAllocator());
    panel->Padding = Thickness{10.0f};
    auto child = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
    panel->AddView(child.Get());
    root->AddView(panel.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(10));
    CHECK(child->Bounds.y == doctest::Approx(10));
}

// === ImageView ===

TEST_CASE("control: ImageView_NullImage_ZeroSize")
{
    auto iv = foundation::MakeRef<ImageView>(foundation::DefaultAllocator());
    iv->Measure(BoxConstraints::Expand());
    CHECK(iv->MeasuredSize.x == 0);
    CHECK(iv->MeasuredSize.y == 0);
}

// === ToggleButton ===

TEST_CASE("control: ToggleButton_Toggle")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto toggle = foundation::MakeRef<ToggleButton>(foundation::DefaultAllocator(), StringView(u8"Toggle"));
    root->AddView(toggle.Get());
    CHECK(!toggle->IsChecked.Value());
    bool fired = false;
    toggle->OnCheckedChanged.Add([&fired](ToggleButton*, bool) { fired = true; });
    KeyEventArgs args;
    args.Set(KeyCode::Space, KeyModifiers::None, false);
    toggle->OnKeyDown(args);
    CHECK(toggle->IsChecked.Value());
    CHECK(fired);
}

// === RadioButton + RadioGroup ===

TEST_CASE("control: RadioButton_CannotUncheckByClick")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto radio = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"Option"));
    radio->IsChecked.SetValue(true);
    root->AddView(radio.Get());
    MouseEventArgs args;
    args.Set(5, 5, MouseButton::Left);
    radio->OnMouseDown(args);
    CHECK(radio->IsChecked.Value()); // still checked
}

TEST_CASE("control: RadioGroup_MutualExclusion")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto group = foundation::MakeRef<RadioGroup>(foundation::DefaultAllocator());
    auto a = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"A"));
    auto b = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"B"));
    auto c = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"C"));
    group->AddRadioButton(a.Get());
    group->AddRadioButton(b.Get());
    group->AddRadioButton(c.Get());
    root->AddView(group.Get());

    group->CheckAt(0);
    CHECK(a->IsChecked.Value());
    CHECK(!b->IsChecked.Value());

    b->IsChecked.SetValue(true);
    CHECK(!a->IsChecked.Value());
    CHECK(b->IsChecked.Value());
    CHECK(!c->IsChecked.Value());
}

TEST_CASE("control: RadioGroup_SelectionChangedEvent")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto group = foundation::MakeRef<RadioGroup>(foundation::DefaultAllocator());
    auto a = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"A"));
    auto b = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"B"));
    group->AddRadioButton(a.Get());
    group->AddRadioButton(b.Get());
    root->AddView(group.Get());

    RadioButton* selected = nullptr;
    group->OnSelectionChanged.Add([&selected](RadioGroup*, RadioButton* r) { selected = r; });

    a->IsChecked.SetValue(true);
    CHECK(selected == a.Get());
    b->IsChecked.SetValue(true);
    CHECK(selected == b.Get());
}

// === ToggleSwitch ===

TEST_CASE("control: ToggleSwitch_Toggle")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto sw = foundation::MakeRef<ToggleSwitch>(foundation::DefaultAllocator(), StringView(u8"VSync"));
    root->AddView(sw.Get());
    CHECK(!sw->IsChecked.Value());
    bool toggled = false;
    sw->OnCheckedChanged.Add([&toggled](ToggleSwitch*, bool) { toggled = true; });
    MouseEventArgs args;
    args.Set(10, 10, MouseButton::Left);
    sw->OnMouseDown(args);
    CHECK(sw->IsChecked.Value());
    CHECK(toggled);
}

TEST_CASE("control: ToggleSwitch_OnActivate_Toggles")
{
    auto sw = foundation::MakeRef<ToggleSwitch>(foundation::DefaultAllocator(), StringView(u8"Test"));
    CHECK(!sw->IsChecked.Value());
    sw->OnActivate();
    CHECK(sw->IsChecked.Value());
}

TEST_CASE("control: RadioButton_OnActivate_Selects")
{
    auto rb = foundation::MakeRef<RadioButton>(foundation::DefaultAllocator(), StringView(u8"Test"));
    CHECK(!rb->IsChecked.Value());
    rb->OnActivate();
    CHECK(rb->IsChecked.Value());
}

// === Slider ===

TEST_CASE("control: Slider_ValueClamped")
{
    auto slider = foundation::MakeRef<Slider>(foundation::DefaultAllocator(), 0.0f, 100.0f, 50.0f);
    CHECK(slider->Value.Value() == 50);
    slider->Value.SetValue(-10.0f);
    CHECK(slider->Value.Value() == 0);
    slider->Value.SetValue(200.0f);
    CHECK(slider->Value.Value() == 100);
}

TEST_CASE("control: Slider_Step")
{
    auto slider = foundation::MakeRef<Slider>(foundation::DefaultAllocator(), 0.0f, 100.0f);
    slider->Step.SetValue(10.0f);
    slider->Value.SetValue(33.0f);
    CHECK(slider->Value.Value() == doctest::Approx(30));
}

TEST_CASE("control: Slider_ValueChangedEvent")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto slider = foundation::MakeRef<Slider>(foundation::DefaultAllocator(), 0.0f, 100.0f);
    root->AddView(slider.Get());
    f32 lastVal = -1;
    slider->OnValueChanged.Add([&lastVal](Slider*, f32 v) { lastVal = v; });
    slider->Value.SetValue(42.0f);
    CHECK(lastVal == 42);
}

TEST_CASE("control: Slider_KeyboardControl")
{
    auto slider = foundation::MakeRef<Slider>(foundation::DefaultAllocator(), 0.0f, 100.0f, 50.0f);
    slider->Step.SetValue(5.0f);
    KeyEventArgs r;
    r.Set(KeyCode::Right, KeyModifiers::None, false);
    slider->OnKeyDown(r);
    CHECK(slider->Value.Value() == 55);
    KeyEventArgs l;
    l.Set(KeyCode::Left, KeyModifiers::None, false);
    slider->OnKeyDown(l);
    CHECK(slider->Value.Value() == 50);
    KeyEventArgs h;
    h.Set(KeyCode::Home, KeyModifiers::None, false);
    slider->OnKeyDown(h);
    CHECK(slider->Value.Value() == 0);
    KeyEventArgs e;
    e.Set(KeyCode::End, KeyModifiers::None, false);
    slider->OnKeyDown(e);
    CHECK(slider->Value.Value() == 100);
}

// === Expander ===

TEST_CASE("control: Expander_DefaultExpanded")
{
    auto expander = foundation::MakeRef<Expander>(foundation::DefaultAllocator(), StringView(u8"Header"));
    CHECK(expander->IsExpanded());
}

TEST_CASE("control: Expander_Toggle")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto expander = foundation::MakeRef<Expander>(foundation::DefaultAllocator(), StringView(u8"Settings"));
    auto content = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 100.0f, 50.0f);
    expander->SetContent(content.Get());
    root->AddView(expander.Get());

    bool fired = false;
    expander->OnExpandedChanged.Add([&fired](Expander*, bool) { fired = true; });
    expander->SetIsExpanded(false);
    CHECK(!expander->IsExpanded());
    CHECK(fired);
    CHECK(content->Visibility == Visibility::Gone);
    expander->SetIsExpanded(true);
    CHECK(content->Visibility == Visibility::Visible);
}

TEST_CASE("control: Expander_CollapsedMeasure")
{
    auto expander = foundation::MakeRef<Expander>(foundation::DefaultAllocator(), StringView(u8"Header"));
    auto content = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 100.0f, 50.0f);
    expander->SetContent(content.Get());

    expander->Measure(BoxConstraints::Loose(400, 300));
    const f32 expandedH = expander->MeasuredSize.y;
    expander->SetIsExpanded(false);
    expander->Measure(BoxConstraints::Loose(400, 300));
    const f32 collapsedH = expander->MeasuredSize.y;

    CHECK(collapsedH < expandedH);
    CHECK(collapsedH == doctest::Approx(expander->HeaderHeight.Value()).epsilon(0.02));
}

// === WantsArrowKeys (from DirectionalFocusTests: arrow keys go to the focused control, not focus-nav) ===

TEST_CASE("control: WantsArrowKeys_ButtonFalse")
{
    auto btn = MakeButton(u8"Test");
    CHECK_FALSE(btn->WantsArrowKeys);
}

TEST_CASE("control: WantsArrowKeys_EditTextTrue")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto edit = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    root->AddView(edit.Get());
    CHECK(edit->WantsArrowKeys);
}

TEST_CASE("control: WantsArrowKeys_NumericFieldTrue")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto nf = foundation::MakeRef<NumericField>(foundation::DefaultAllocator());
    root->AddView(nf.Get());
    CHECK(nf->WantsArrowKeys);
}
