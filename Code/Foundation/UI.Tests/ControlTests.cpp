// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ControlTests.bf - the Button/RepeatButton/CheckBox subset
// + the Button/CheckBox OnActivate cases from DirectionalFocusTests. Text rendering is not
// exercised in the controls, but every tested behavior (state/events/toggle/measure fallback)
// is exercised here.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.image; // ImageDataRef (the provider's answer in the Source tests)
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

static core::RefPtr<RootView> MakeRoot()
{
    return core::MakeRef<RootView>(core::DefaultAllocator());
}
static core::RefPtr<Button> MakeButton(StringView t)
{
    return core::MakeRef<Button>(core::DefaultAllocator(), t);
}
static core::RefPtr<CheckBox> MakeCheckBox(StringView t)
{
    return core::MakeRef<CheckBox>(core::DefaultAllocator(), t);
}

// === Button ===

TEST_CASE("control: Button_PressedTransitions")
{
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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

TEST_CASE("control: IconButton_MeasuresIconPlusChrome")
{
    // The size is the ICON (content); measure adds the default {3,3} padding. A flat-size
    // measure would inset the draw by padding that measure never reserved, shrinking the icon.
    auto btn = core::MakeRef<IconButton>(core::DefaultAllocator(), nullptr, 24.0f);
    btn->Measure(BoxConstraints(0, 100, 0, 100));
    CHECK(btn->MeasuredSize.x == doctest::Approx(30.0f)); // 24 + 3 + 3
    CHECK(btn->MeasuredSize.y == doctest::Approx(30.0f));
}

TEST_CASE("control: IconButton_Clicks")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = core::MakeRef<IconButton>(core::DefaultAllocator(), nullptr, 20.0f);
    root->AddView(btn.Get());
    bool clicked = false;
    btn->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    btn->FireClick();
    CHECK(clicked);
}

TEST_CASE("control: IconButton_PressedTransitions")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = core::MakeRef<IconButton>(core::DefaultAllocator(), nullptr, 20.0f);
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
    auto btn = core::MakeRef<IconButton>(core::DefaultAllocator(), nullptr);
    CHECK(btn->IsFocusable);
}

// === RepeatButton ===

TEST_CASE("control: RepeatButton_ClicksOnce")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = core::MakeRef<RepeatButton>(core::DefaultAllocator(), StringView(u8"Hold"));
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = core::MakeRef<RepeatButton>(core::DefaultAllocator(), StringView(u8"Hold"));
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto btn = core::MakeRef<RepeatButton>(core::DefaultAllocator(), StringView(u8"Hold"));
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"), true);
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
    auto label = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"Hello"));
    CHECK(label->Text.Value() == StringView(u8"Hello"));
}

TEST_CASE("control: Label_SetTextChaining")
{
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    label->SetText(u8"World");
    CHECK(label->Text.Value() == StringView(u8"World"));
}

TEST_CASE("control: Label_MeasuresNonZero")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto label = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"Hello"));
    root->AddView(label.Get());
    LayoutPass(ctx, root.Get());
    CHECK(label->MeasuredSize.y > 0);
}

// === Spacer ===

TEST_CASE("control: Spacer_MeasuresToDesiredSize")
{
    auto spacer = core::MakeRef<Spacer>(core::DefaultAllocator(), 20.0f, 10.0f);
    spacer->Measure(BoxConstraints::Expand());
    CHECK(spacer->MeasuredSize.x == 20);
    CHECK(spacer->MeasuredSize.y == 10);
}

// === ColorView ===

TEST_CASE("control: ColorView_StoresColor")
{
    auto cv =
        core::MakeRef<ColorView>(core::DefaultAllocator(), core::Color{1.0f, 0.0f, 0.0f, 1.0f});
    CHECK(cv->Color.Value().r == 1.0f);
    CHECK(cv->Color.Value().g == 0.0f);
}

// === Separator ===

TEST_CASE("control: Separator_HorizontalMeasure")
{
    auto sep = core::MakeRef<Separator>(core::DefaultAllocator(), Orientation::Horizontal);
    sep->Measure(BoxConstraints::Loose(400, 300));
    CHECK(sep->MeasuredSize.y == 1);
    CHECK(sep->MeasuredSize.x == 400);
}

TEST_CASE("control: Separator_VerticalMeasure")
{
    auto sep = core::MakeRef<Separator>(core::DefaultAllocator(), Orientation::Vertical);
    sep->Measure(BoxConstraints::Loose(400, 300));
    CHECK(sep->MeasuredSize.x == 1);
    CHECK(sep->MeasuredSize.y == 300);
}

// === ProgressBar ===

TEST_CASE("control: ProgressBar_ValueClamped")
{
    auto bar = core::MakeRef<ProgressBar>(core::DefaultAllocator());
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto panel = core::MakeRef<Panel>(core::DefaultAllocator());
    panel->Padding = Thickness{10.0f};
    auto child = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
    panel->AddView(child.Get());
    root->AddView(panel.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(10));
    CHECK(child->Bounds.y == doctest::Approx(10));
}

// === ImageView ===

TEST_CASE("control: ImageView_NullImage_ZeroSize")
{
    auto iv = core::MakeRef<ImageView>(core::DefaultAllocator());
    iv->Measure(BoxConstraints::Expand());
    CHECK(iv->MeasuredSize.x == 0);
    CHECK(iv->MeasuredSize.y == 0);
}

namespace
{
    // Answers one name with one image, after `loadingFor` asks (an asset still loading).
    class LateProvider final : public IResourceProvider
    {
    public:
        foundation::image::ImageDataRef minimap{256, 128};
        foundation::image::ImageDataRef icon{32, 32};
        int asks = 0;
        int loadingFor = 0;

        bool LoadText(StringView, String&) override { return false; }
        const foundation::image::ImageData* LoadImage(StringView path) override
        {
            ++asks;
            if (asks <= loadingFor)
            {
                return nullptr;
            }
            if (path == StringView(u8"minimap"))
            {
                return &minimap;
            }
            return (path == StringView(u8"icon")) ? &icon : nullptr;
        }
    };
}

TEST_CASE("control: ImageView resolves its Source through the context's resource provider")
{
    UIContext ctx{DefaultAllocator()};
    LateProvider provider;
    provider.loadingFor = 1; // the first ask finds it still loading
    ctx.SetResourceProvider(&provider);
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto iv = core::MakeRef<ImageView>(core::DefaultAllocator());
    iv->Source.SetValue(String(u8"minimap"));
    root->AddView(iv.Get());

    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == nullptr); // not loaded yet: nothing shown, asked again later
    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == &provider.minimap);
    const int asksOnceResolved = provider.asks;
    LayoutPass(ctx, root.Get());
    CHECK(provider.asks == asksOnceResolved); // resolved once, not every frame

    // The image's size is the view's natural size.
    iv->Measure(BoxConstraints::Expand());
    CHECK(iv->MeasuredSize.x == doctest::Approx(256));
    CHECK(iv->MeasuredSize.y == doctest::Approx(128));

    // A new source resolves anew.
    iv->Source.SetValue(String(u8"icon"));
    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == &provider.icon);

    // Clearing the source clears its image; setting it again resolves again.
    iv->Source.SetValue(String{});
    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == nullptr);
    iv->Source.SetValue(String(u8"icon"));
    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == &provider.icon);
}

TEST_CASE("control: ImageView without a provider keeps the image SetImage gave")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    foundation::image::ImageDataRef given{16, 16};
    auto iv = core::MakeRef<ImageView>(core::DefaultAllocator(), &given);
    root->AddView(iv.Get());
    LayoutPass(ctx, root.Get());
    CHECK(iv->GetImage() == &given);
}

// === ToggleButton ===
// === ToggleButton ===

TEST_CASE("control: ToggleButton_Toggle")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto toggle = core::MakeRef<ToggleButton>(core::DefaultAllocator(), StringView(u8"Toggle"));
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto radio = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"Option"));
    radio->IsChecked.SetValue(true);
    root->AddView(radio.Get());
    MouseEventArgs args;
    args.Set(5, 5, MouseButton::Left);
    radio->OnMouseDown(args);
    CHECK(radio->IsChecked.Value()); // still checked
}

TEST_CASE("control: RadioGroup_MutualExclusion")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto group = core::MakeRef<RadioGroup>(core::DefaultAllocator());
    auto a = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"A"));
    auto b = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"B"));
    auto c = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"C"));
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto group = core::MakeRef<RadioGroup>(core::DefaultAllocator());
    auto a = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"A"));
    auto b = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"B"));
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
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto sw = core::MakeRef<ToggleSwitch>(core::DefaultAllocator(), StringView(u8"VSync"));
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
    auto sw = core::MakeRef<ToggleSwitch>(core::DefaultAllocator(), StringView(u8"Test"));
    CHECK(!sw->IsChecked.Value());
    sw->OnActivate();
    CHECK(sw->IsChecked.Value());
}

TEST_CASE("control: RadioButton_OnActivate_Selects")
{
    auto rb = core::MakeRef<RadioButton>(core::DefaultAllocator(), StringView(u8"Test"));
    CHECK(!rb->IsChecked.Value());
    rb->OnActivate();
    CHECK(rb->IsChecked.Value());
}

// === Slider ===

TEST_CASE("control: Slider_ValueClamped")
{
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 100.0f, 50.0f);
    CHECK(slider->Value.Value() == 50);
    slider->Value.SetValue(-10.0f);
    CHECK(slider->Value.Value() == 0);
    slider->Value.SetValue(200.0f);
    CHECK(slider->Value.Value() == 100);
}

TEST_CASE("control: Slider_Step")
{
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 100.0f);
    slider->Step.SetValue(10.0f);
    slider->Value.SetValue(33.0f);
    CHECK(slider->Value.Value() == doctest::Approx(30));
}

TEST_CASE("control: Slider_ValueChangedEvent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 100.0f);
    root->AddView(slider.Get());
    f32 lastVal = -1;
    slider->OnValueChanged.Add([&lastVal](Slider*, f32 v) { lastVal = v; });
    slider->Value.SetValue(42.0f);
    CHECK(lastVal == 42);
}

TEST_CASE("control: Slider_KeyboardControl")
{
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 100.0f, 50.0f);
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

TEST_CASE("control: Slider_TakesOnlyTheArrowsAlongItsAxis")
{
    // Sedulous 8c8c0bd4: a horizontal slider leaves up and down unhandled, so a menu of sliders
    // still navigates; a vertical one is the other way round.
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 100.0f, 50.0f);
    slider->Step.SetValue(5.0f);
    KeyEventArgs down;
    down.Set(KeyCode::Down, KeyModifiers::None, false);
    slider->OnKeyDown(down);
    CHECK_FALSE(down.Handled); // horizontal: down is for focus
    CHECK(slider->Value.Value() == 50);
    KeyEventArgs up;
    up.Set(KeyCode::Up, KeyModifiers::None, false);
    slider->OnKeyDown(up);
    CHECK_FALSE(up.Handled); // and so is up
    CHECK(slider->Value.Value() == 50);

    slider->Orientation.SetValue(Orientation::Vertical);
    KeyEventArgs up2;
    up2.Set(KeyCode::Up, KeyModifiers::None, false);
    slider->OnKeyDown(up2);
    CHECK(up2.Handled); // vertical: up steps
    CHECK(slider->Value.Value() == 55);
    KeyEventArgs right;
    right.Set(KeyCode::Right, KeyModifiers::None, false);
    slider->OnKeyDown(right);
    CHECK_FALSE(right.Handled); // and right is for focus
    CHECK(slider->Value.Value() == 55);
}

// === Expander ===

TEST_CASE("control: Expander_DefaultExpanded")
{
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Header"));
    CHECK(expander->IsExpanded());
}

TEST_CASE("control: Expander_Toggle")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Settings"));
    auto content = core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 50.0f);
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
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Header"));
    auto content = core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 50.0f);
    expander->SetContent(content.Get());

    expander->Measure(BoxConstraints::Loose(400, 300));
    const f32 expandedH = expander->MeasuredSize.y;
    expander->SetIsExpanded(false);
    expander->Measure(BoxConstraints::Loose(400, 300));
    const f32 collapsedH = expander->MeasuredSize.y;

    CHECK(collapsedH < expandedH);
    CHECK(collapsedH == doctest::Approx(expander->HeaderHeight.Value()).epsilon(0.02));
}

// A themed border and padding inset the BODY (sides and bottom) and leave the band full width: the
// section reads as one outlined box (Sedulous 766bf314). Unstyled, the body stays flush.
TEST_CASE("control: Expander_BorderedInsetsItsBodyNotItsBand")
{
    UIContext ctx{core::DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Section"));
    auto content = core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 50.0f);
    expander->SetContent(content.Get());
    root->AddView(expander.Get());
    SSSParser::ApplyInlineStyle(expander.Get(), u8"border-width: 1; padding: 0 6 6 6;");

    expander->Measure(BoxConstraints::Loose(400, 400));
    const f32 band = expander->HeaderBandHeight();
    const f32 spacing = expander->ContentSpacing.Value();
    CHECK(expander->MeasuredSize.y == doctest::Approx(band + spacing + 50.0f + 7.0f)); // bottom inset
    expander->Layout(0, 0, 400, expander->MeasuredSize.y);
    CHECK(content->Bounds.x == doctest::Approx(7.0f)); // padding plus border on the left
    CHECK(content->Bounds.width == doctest::Approx(400.0f - 14.0f));
    CHECK(content->Bounds.y == doctest::Approx(band + spacing));

    auto plain = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Plain"));
    auto body = core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 50.0f);
    plain->SetContent(body.Get());
    root->AddView(plain.Get());
    plain->Measure(BoxConstraints::Loose(400, 400));
    plain->Layout(0, 0, 400, plain->MeasuredSize.y);
    CHECK(body->Bounds.x == doctest::Approx(0.0f));
    CHECK(body->Bounds.width == doctest::Approx(400.0f));
}

TEST_CASE("control: Expander_HeaderBandGrowsForOversizedActions")
{
    // Actions taller than HeaderHeight must not overflow the fixed band (and clip when
    // collapsed). The structured band grows to fit them instead.
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Header"));
    auto actions = core::MakeRef<TestView>(core::DefaultAllocator(), 44.0f, 40.0f);
    expander->SetHeaderActions(actions.Get());
    expander->SetIsExpanded(false);

    expander->Measure(BoxConstraints::Loose(400, 300));
    CHECK(expander->HeaderBandHeight() > expander->HeaderHeight.Value());
    CHECK(expander->MeasuredSize.y == doctest::Approx(expander->HeaderBandHeight()));

    // Small actions leave the band at its HeaderHeight minimum.
    auto small = core::MakeRef<TestView>(core::DefaultAllocator(), 30.0f, 18.0f);
    expander->SetHeaderActions(small.Get());
    expander->Measure(BoxConstraints::Loose(400, 300));
    CHECK(expander->HeaderBandHeight() ==
          doctest::Approx(expander->HeaderHeight.Value()).epsilon(0.02));
}

TEST_CASE("control: Expander_HeaderActionsRightAlignedAndCentered")
{
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Header"));
    auto actions = core::MakeRef<TestView>(core::DefaultAllocator(), 44.0f, 18.0f);
    expander->SetHeaderActions(actions.Get());

    expander->Measure(BoxConstraints::Tight(400, 200));
    expander->Layout(0, 0, 400, 200);

    // Right-aligned with the 4px inset, vertically centered in the band.
    CHECK(actions->Bounds.x == doctest::Approx(400.0f - 44.0f - 4.0f));
    const f32 band = expander->HeaderBandHeight();
    CHECK(actions->Bounds.y == doctest::Approx((band - 18.0f) * 0.5f));
}

TEST_CASE("control: Expander_HeaderTitleViewSitsBetweenTheChevronAndTheActions")
{
    // A view in place of the drawn title (an editable name): after the chevron, up to the
    // actions block, centred in the band; clearing it goes back to the drawn HeaderText.
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator(), StringView(u8"Header"));
    auto actions = core::MakeRef<TestView>(core::DefaultAllocator(), 44.0f, 18.0f);
    auto title = core::MakeRef<TestView>(core::DefaultAllocator(), 60.0f, 20.0f);
    expander->SetHeaderActions(actions.Get());
    expander->SetHeaderTitle(title.Get());
    CHECK(expander->HeaderTitle() == title.Get());

    expander->Measure(BoxConstraints::Tight(400, 200));
    expander->Layout(0, 0, 400, 200);
    const f32 band = expander->HeaderBandHeight();
    CHECK(title->Bounds.x == doctest::Approx(24.0f)); // chevron x 8 + size 8 + gap 8
    CHECK(title->Bounds.x + title->Bounds.width <= actions->Bounds.x);
    CHECK(title->Bounds.width > 60.0f); // it takes the room up to the actions
    CHECK(title->Bounds.y == doctest::Approx((band - 20.0f) * 0.5f));

    expander->SetHeaderTitle(nullptr);
    CHECK(expander->HeaderTitle() == nullptr);
}

// === WantsArrowKeys (from DirectionalFocusTests: arrow keys go to the focused control, not focus-nav) ===

TEST_CASE("control: WantsArrowKeys_ButtonFalse")
{
    auto btn = MakeButton(u8"Test");
    CHECK_FALSE(btn->WantsArrowKeys);
}

TEST_CASE("control: WantsArrowKeys_EditTextTrue")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
    root->AddView(edit.Get());
    CHECK(edit->WantsArrowKeys);
}

TEST_CASE("control: WantsArrowKeys_NumericFieldTrue")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto nf = core::MakeRef<NumericField>(core::DefaultAllocator());
    root->AddView(nf.Get());
    CHECK(nf->WantsArrowKeys);
}
