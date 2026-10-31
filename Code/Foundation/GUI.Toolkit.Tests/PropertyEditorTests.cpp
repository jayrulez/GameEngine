// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the property grid's editors. An editor is chosen by hint, then by kind, and
// otherwise a row says that none fits. Each shows a value or the mixed state and edits in gestures:
// a scrub previews and commits on release, or Escape cancels it; a click, a pick or a typed value
// commits at once; and a flag is written on its own, through its mask.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::gui;
namespace core = foundation::core;
using core::f32;
using core::i64;
using core::u64;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // Records each call as text: "begin p", "preview p=v", "commit p=v", "commit p=v&mask", "cancel p".
    struct Host : IPropertyEditHost
    {
        core::Array<core::String> calls{core::DefaultAllocator()};
        void BeginPropertyEdit(core::StringView path) override { calls.PushBack(core::Format(u8"begin {}", path)); }
        void PreviewPropertyEdit(core::StringView path, const PropertyWrite& write) override
        {
            calls.PushBack(core::Format(u8"preview {}={}", path, write.Value.ToString().AsView()));
        }
        void CommitPropertyEdit(core::StringView path, const PropertyWrite& write) override
        {
            if (write.Mask == ~u64{0})
                calls.PushBack(core::Format(u8"commit {}={}", path, write.Value.ToString().AsView()));
            else
                calls.PushBack(core::Format(u8"commit {}={}&{}", path, write.Value.ToString().AsView(), write.Mask));
        }
        void CancelPropertyEdit(core::StringView path) override { calls.PushBack(core::Format(u8"cancel {}", path)); }
        bool EditPropertyList(core::StringView path, const ListEdit& edit) override
        {
            calls.PushBack(core::Format(u8"list {} {} {}", path, static_cast<int>(edit.Kind), edit.Index));
            return true;
        }
        void GetChildProperties(core::StringView, core::Array<PropertyInfo>&) const override {}
        [[nodiscard]] PropertyValue ReadPropertyValue(core::StringView, PropertyKind) const override { return {}; }
        [[nodiscard]] core::StringView Last() const { return calls.IsEmpty() ? core::StringView{} : calls.Back().AsView(); }
    };

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        Host host;

        Bed()
        {
            root->SetSize(core::Float2{400.0f, 300.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
        }
        template <typename T>
        core::RefPtr<T> Editor(const PropertyInfo& info, const PropertyValue& value, core::StringView path = core::StringView(u8"p"))
        {
            auto editor = core::MakeRef<T>(core::DefaultAllocator());
            editor->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(160.0f);
                s.Height = LayoutLength::Px(24.0f);
            });
            root->AddChild(editor.Get());
            editor->Bind(host, path, info, value);
            (void)root->UpdateLayout();
            return editor;
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        static core::u32 K(KeyCode k) { return static_cast<core::u32>(k); }
        void Click(core::Float2 at)
        {
            D().InjectMouseMove(at);
            D().InjectMouseDown(at, MouseButton::Left);
            D().InjectMouseUp(at, MouseButton::Left);
            (void)root->UpdateLayout();
        }
    };

    PropertyInfo Info(PropertyKind kind)
    {
        PropertyInfo info;
        info.Name = core::String(u8"p");
        info.Kind = kind;
        return info;
    }
    PropertyValue Value(const Variant& value, bool mixed = false)
    {
        PropertyValue out;
        out.Value = value;
        out.Mixed = mixed;
        return out;
    }
    core::Float2 Centre(const Node& node)
    {
        const Rect r = node.GetScreenBounds();
        return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    }
}

TEST_CASE("property editors: chosen by hint, then kind, else a row that says none fits")
{
    PropertyEditorRegistry registry(core::DefaultAllocator());
    RegisterDefaultPropertyEditors(registry);
    PropertyInfo info = Info(PropertyKind::Float);
    const core::u32 number = registry.Choose(info);
    CHECK(number != kPropertyEditorRegistryNoEditor);
    CHECK(core::Cast<NumberPropertyEditor>(registry.Create(number, core::DefaultAllocator()).Get()) != nullptr);

    info.Hint = core::String(u8"slider"); // a number over its range
    CHECK(core::Cast<SliderPropertyEditor>(registry.Create(registry.Choose(info), core::DefaultAllocator()).Get()) != nullptr);

    info.Hint = core::String(u8"dial"); // no editor takes the hint, so the kind's editor does
    CHECK(registry.Choose(info) == number);
    const core::u32 before = registry.GetGeneration();
    registry.RegisterHint(SV(u8"dial"), [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<BoolPropertyEditor>(a); });
    CHECK(registry.GetGeneration() != before);
    const core::u32 dial = registry.Choose(info);
    CHECK(dial != number);
    CHECK(core::Cast<BoolPropertyEditor>(registry.Create(dial, core::DefaultAllocator()).Get()) != nullptr);

    info.Kind = PropertyKind::Group;
    info.Hint = core::String(u8"curve");
    CHECK(registry.Choose(info) == kPropertyEditorRegistryNoEditor);
    Bed bed;
    auto unknown = bed.Editor<UnknownPropertyEditor>(info, PropertyValue{});
    CHECK(unknown->GetMessage() == SV(u8"No editor for curve"));
    info.Hint.Clear();
    unknown->Bind(bed.host, SV(u8"p"), info, PropertyValue{});
    CHECK(unknown->GetMessage() == SV(u8"No editor for group"));
}

TEST_CASE("property editors: a bool shows mixed as indeterminate, and a click commits at once")
{
    Bed bed;
    auto editor = bed.Editor<BoolPropertyEditor>(Info(PropertyKind::Bool), Value(Variant(false), true), SV(u8"visible"));
    CHECK(editor->GetCheckBox()->IsIndeterminate());
    bed.Click(Centre(*editor->GetCheckBox()));
    REQUIRE(bed.host.calls.Size() == 2u);
    CHECK(bed.host.calls[0] == core::String(u8"begin visible"));
    CHECK(bed.host.calls[1] == core::String(u8"commit visible=true"));

    PropertyInfo locked = Info(PropertyKind::Bool);
    locked.ReadOnly = true;
    editor->Bind(bed.host, SV(u8"visible"), locked, Value(Variant(true)));
    bed.Click(Centre(*editor->GetCheckBox()));
    CHECK(bed.host.calls.Size() == 2u); // read only, so no calls
}

TEST_CASE("property editors: a number's scrub previews, commits on release, or cancels on Escape")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Float);
    info.HasRange = true;
    info.Min = 0.0;
    info.Max = 1000.0;
    info.Step = 1.0;
    auto editor = bed.Editor<NumberPropertyEditor>(info, Value(Variant(10.0)), SV(u8"speed"));
    NumberField& field = *editor->GetField();
    CHECK(field.GetValue() == doctest::Approx(10.0));
    CHECK_FALSE(field.StepsOnWheel()); // the grid scrolls

    const core::Float2 at = Centre(field) - core::Float2{40.0f, 0.0f};
    bed.D().InjectMouseDown(at, MouseButton::Left);
    bed.D().InjectMouseMove(at + core::Float2{20.0f, 0.0f}); // five steps
    bed.D().InjectMouseMove(at + core::Float2{40.0f, 0.0f}); // ten
    bed.D().InjectMouseUp(at + core::Float2{40.0f, 0.0f}, MouseButton::Left);
    REQUIRE(bed.host.calls.Size() == 4u);
    CHECK(bed.host.calls[0] == core::String(u8"begin speed"));
    CHECK(bed.host.calls[1] == core::String(u8"preview speed=15"));
    CHECK(bed.host.calls[2] == core::String(u8"preview speed=20"));
    CHECK(bed.host.calls[3] == core::String(u8"commit speed=20"));

    bed.host.calls.Clear();
    bed.D().InjectMouseDown(at, MouseButton::Left);
    bed.D().InjectMouseMove(at + core::Float2{20.0f, 0.0f});
    bed.D().InjectKeyDown(Bed::K(KeyCode::Escape));
    bed.D().InjectMouseUp(at + core::Float2{20.0f, 0.0f}, MouseButton::Left);
    REQUIRE(bed.host.calls.Size() == 3u);
    CHECK(bed.host.Last() == SV(u8"cancel speed"));
    CHECK(field.GetValue() == doctest::Approx(20.0));
}

TEST_CASE("property editors: a slider's drag previews and commits on release, the field following; Escape puts it back")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Float);
    info.Hint = core::String(u8"slider");
    info.HasRange = true;
    info.Min = 0.0;
    info.Max = 1.0;
    info.Step = 0.01;
    auto editor = bed.Editor<SliderPropertyEditor>(info, Value(Variant(0.25)), SV(u8"rough"));
    Slider& slider = *editor->GetSlider();
    NumberField& field = *editor->GetField();
    CHECK(slider.GetValue() == doctest::Approx(0.25));
    CHECK(field.GetValue() == doctest::Approx(0.25));
    CHECK_FALSE(slider.StepsOnWheel()); // the grid scrolls
    CHECK_FALSE(field.StepsOnWheel());
    const Rect track = slider.GetScreenBounds();
    CHECK(track.width > 40.0f);                                           // the slider takes the free room
    CHECK(field.GetScreenBounds().x >= track.x + track.width - 0.5f);    // the value field comes after it

    const f32 y = track.y + track.height * 0.5f;
    const core::Float2 start{track.x + track.width * 0.5f, y};
    const core::Float2 end{track.x + track.width + 50.0f, y}; // past its end, which gives the top value
    slider.RequestFocus();
    bed.D().InjectMouseDown(start, MouseButton::Left);
    bed.D().InjectMouseMove(end);
    CHECK(field.GetValue() == doctest::Approx(1.0)); // the field follows the drag
    bed.D().InjectMouseUp(end, MouseButton::Left);
    REQUIRE(bed.host.calls.Size() == 4u);
    CHECK(bed.host.calls[0] == core::String(u8"begin rough"));
    CHECK(bed.host.calls[1] == core::String(u8"preview rough=0.5"));
    CHECK(bed.host.calls[2] == core::String(u8"preview rough=1"));
    CHECK(bed.host.calls[3] == core::String(u8"commit rough=1"));

    bed.host.calls.Clear();
    bed.D().InjectMouseDown(start, MouseButton::Left);
    bed.D().InjectKeyDown(Bed::K(KeyCode::Escape));
    bed.D().InjectMouseUp(start, MouseButton::Left);
    REQUIRE(bed.host.calls.Size() == 3u);
    CHECK(bed.host.Last() == SV(u8"cancel rough"));
    CHECK(slider.GetValue() == doctest::Approx(1.0)); // both are back
    CHECK(field.GetValue() == doctest::Approx(1.0));
}

TEST_CASE("property editors: an integer writes whole numbers; mixed shows the placeholder")
{
    Bed bed;
    auto editor = bed.Editor<NumberPropertyEditor>(Info(PropertyKind::Integer), Value(Variant(i64{3}), true), SV(u8"count"));
    NumberField& field = *editor->GetField();
    CHECK(field.IsMixed());
    CHECK(field.GetText().Size() == 0u);
    CHECK(field.GetPlaceholder() == MixedValueText());
    field.RequestFocus();
    bed.D().InjectKeyDown(Bed::K(KeyCode::Up)); // a step from mixed sets them all
    REQUIRE(bed.host.calls.Size() == 3u);
    CHECK(bed.host.calls[1] == core::String(u8"preview count=4"));
    CHECK(bed.host.calls[2] == core::String(u8"commit count=4"));
}

TEST_CASE("property editors: text commits on Return, and Escape puts back what it had")
{
    Bed bed;
    auto editor = bed.Editor<StringPropertyEditor>(Info(PropertyKind::String), Value(Variant(SV(u8"Lamp"))), SV(u8"name"));
    TextField& field = *editor->GetField();
    CHECK(field.GetText() == SV(u8"Lamp"));
    field.RequestFocus();
    field.GetModel().SelectAll();
    bed.D().InjectText(SV(u8"Torch"));
    bed.D().InjectKeyDown(Bed::K(KeyCode::Escape));
    CHECK(field.GetText() == SV(u8"Lamp"));
    CHECK(bed.host.calls.IsEmpty());
    field.GetModel().SelectAll();
    bed.D().InjectText(SV(u8"Torch"));
    bed.D().InjectKeyDown(Bed::K(KeyCode::Return));
    REQUIRE(bed.host.calls.Size() == 2u);
    CHECK(bed.host.calls[1] == core::String(u8"commit name=Torch"));

    editor->Bind(bed.host, SV(u8"name"), Info(PropertyKind::String), Value(Variant(SV(u8"Lamp")), true));
    CHECK(field.GetText().Size() == 0u);
    CHECK(field.GetPlaceholder() == MixedValueText());
}

TEST_CASE("property editors: an enum picks among its choices; mixed selects none")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Enum);
    info.Choices.PushBack(PropertyChoice{core::String(u8"Point"), 0});
    info.Choices.PushBack(PropertyChoice{core::String(u8"Spot"), 2});
    info.Choices.PushBack(PropertyChoice{core::String(u8"Area"), 5});
    auto editor = bed.Editor<EnumPropertyEditor>(info, Value(Variant(i64{2})), SV(u8"type"));
    ComboBox& combo = *editor->GetComboBox();
    CHECK(combo.ItemCount() == 3u);
    CHECK(combo.GetSelectedIndex() == 1);
    CHECK(bed.host.calls.IsEmpty()); // showing is not editing

    combo.SelectByUser(2);
    REQUIRE(bed.host.calls.Size() == 2u);
    CHECK(bed.host.calls[1] == core::String(u8"commit type=5"));

    editor->Bind(bed.host, SV(u8"type"), info, Value(Variant(i64{2}), true));
    CHECK(combo.GetSelectedIndex() == -1);
    CHECK(combo.GetPlaceholder() == core::String(MixedValueText()));
    CHECK(bed.host.calls.Size() == 2u);
}

TEST_CASE("property editors: flags name what is set and write one flag alone; a differing flag is indeterminate")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Flags);
    info.Choices.PushBack(PropertyChoice{core::String(u8"Cast"), 1});
    info.Choices.PushBack(PropertyChoice{core::String(u8"Receive"), 2});
    info.Choices.PushBack(PropertyChoice{core::String(u8"Static"), 4});
    PropertyValue value = Value(Variant(i64{0b001}));
    auto editor = bed.Editor<FlagsPropertyEditor>(info, value, SV(u8"shadows"));
    CHECK(editor->GetButton()->GetText() == SV(u8"Cast"));
    value.MixedBits = 0b010;
    value.Mixed = true;
    editor->Bind(bed.host, SV(u8"shadows"), info, value);
    CHECK(editor->GetButton()->GetText() == MixedValueText());

    bed.Click(Centre(*editor->GetButton()));
    REQUIRE(editor->GetList() != nullptr);
    CHECK(editor->GetFlagBox(0)->IsChecked());
    CHECK(editor->GetFlagBox(1)->IsIndeterminate());
    CHECK_FALSE(editor->GetFlagBox(2)->IsChecked());
    bed.Click(Centre(*editor->GetFlagBox(2)));
    REQUIRE(bed.host.calls.Size() == 2u);
    CHECK(bed.host.calls[1] == core::String(u8"commit shadows=4&4"));
    CHECK(editor->GetList() != nullptr); // still open for the next flag

    value.MixedBits = 0;
    value.Mixed = false;
    value.Value = Variant(i64{0});
    editor->Bind(bed.host, SV(u8"shadows"), info, value);
    CHECK(editor->GetButton()->GetText() == SV(u8"None"));
}

TEST_CASE("property editors: a colour's picker previews its drags and commits once on closing, or cancels on Escape")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Color);
    auto editor = bed.Editor<ColorPropertyEditor>(info, Value(Variant(core::Color{1.0f, 0.0f, 0.0f, 1.0f})), SV(u8"tint"));
    CHECK(editor->GetHexField()->GetText() == SV(u8"#FF0000"));
    CHECK(editor->GetPicker() == nullptr);
    bed.Click(Centre(*editor->GetSwatch()));
    ColorPicker* picker = editor->GetPicker();
    REQUIRE(picker != nullptr);
    CHECK(picker->GetColor().r == doctest::Approx(1.0f));
    CHECK(bed.host.calls.IsEmpty()); // opening is not editing

    // Two steps on the plane: the edit begins and previews twice; the picker stays open and
    // nothing is committed.
    ColorPickerArea& plane = *picker->GetPlane();
    const Rect r = plane.GetScreenBounds();
    bed.D().InjectMouseDown(core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f}, MouseButton::Left);
    bed.D().InjectMouseUp(core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f}, MouseButton::Left);
    REQUIRE(bed.host.calls.Size() == 3u);
    CHECK(bed.host.calls[0] == core::String(u8"begin tint"));
    CHECK(bed.host.calls[1].AsView().SubStr(0, 13) == SV(u8"preview tint="));
    // Closing it (a press elsewhere) commits once.
    bed.D().ClosePopup(*editor->GetPicker()->GetParent());
    REQUIRE(bed.host.calls.Size() == 4u);
    CHECK(bed.host.Last().SubStr(0, 12) == SV(u8"commit tint="));

    // Escape cancels the pick.
    bed.host.calls.Clear();
    bed.Click(Centre(*editor->GetSwatch()));
    REQUIRE(editor->GetPicker() != nullptr);
    bed.D().InjectMouseDown(core::Float2{r.x + 5.0f, r.y + 5.0f}, MouseButton::Left);
    bed.D().InjectMouseUp(core::Float2{r.x + 5.0f, r.y + 5.0f}, MouseButton::Left);
    bed.D().InjectKeyDown(Bed::K(KeyCode::Escape));
    CHECK(editor->GetPicker() == nullptr);
    CHECK(bed.host.Last() == SV(u8"cancel tint"));
}

TEST_CASE("property editors: a colour's hex field commits what is typed; mixed is hatched; without alpha none shows")
{
    Bed bed;
    PropertyInfo info = Info(PropertyKind::Color);
    info.ShowsAlpha = false;
    auto editor = bed.Editor<ColorPropertyEditor>(info, Value(Variant(core::Color{0.0f, 0.0f, 1.0f, 0.5f})), SV(u8"tint"));
    CHECK(editor->GetHexField()->GetText() == SV(u8"#0000FF")); // no alpha shown
    TextField& hex = *editor->GetHexField();
    hex.RequestFocus();
    hex.GetModel().SelectAll();
    bed.D().InjectText(SV(u8"#00FF0080"));
    bed.D().InjectKeyDown(Bed::K(KeyCode::Return));
    REQUIRE(bed.host.calls.Size() == 2u);
    CHECK(bed.host.calls[1] == core::String(u8"commit tint=#00FF00FF")); // the alpha is left as it was

    editor->Bind(bed.host, SV(u8"tint"), info, Value(Variant(core::Color{}), true));
    CHECK(editor->GetSwatch()->IsMixed());
    CHECK(editor->GetHexField()->GetPlaceholder() == MixedValueText());
}

TEST_CASE("property editors: a colour's row shows the pick as it goes, and the picker's channel fields have room")
{
    Bed bed;
    auto editor = bed.Editor<ColorPropertyEditor>(Info(PropertyKind::Color), Value(Variant(core::Color{1.0f, 0.0f, 0.0f, 1.0f})), SV(u8"tint"));
    bed.Click(Centre(*editor->GetSwatch()));
    ColorPicker* picker = editor->GetPicker();
    REQUIRE(picker != nullptr);
    (void)bed.root->UpdateLayout();
    const Rect r = picker->GetPlane()->GetScreenBounds();
    bed.D().InjectMouseDown(core::Float2{r.x + 2.0f, r.y + 2.0f}, MouseButton::Left); // near white
    CHECK(editor->GetHexField()->GetText() != SV(u8"#FF0000"));
    CHECK(editor->GetHexField()->GetText() == picker->GetHexField()->GetText());
    bed.D().InjectMouseUp(core::Float2{r.x + 2.0f, r.y + 2.0f}, MouseButton::Left);
    for (int i = 0; i < 4; ++i)
        CHECK(picker->GetChannelField(i)->GetScreenBounds().width >= 44.0f); // "255" beside its arrows
}
