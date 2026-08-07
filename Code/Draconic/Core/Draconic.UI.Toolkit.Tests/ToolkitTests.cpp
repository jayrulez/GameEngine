// Verbatim port of Sedulous.UI.Tests/src/ToolkitTests.bf (SedulousEngine).
// Standalone-construction coverage for the toolkit bars (MenuBar/Toolbar/StatusBar/SplitView/
// BreadcrumbBar), ColorPicker (+ static HSV<->RGB), PropertyGrid, and every PropertyEditor. No UIContext
// needed. Beef `scope X()`/`new X()` -> MakeRef<X>(DefaultAllocator()); ref-equality `===` -> pointer ==.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    [[nodiscard]] foundation::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255)
    {
        return foundation::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Exposes PropertyEditor's protected BeginEdit/EndEdit for the transaction test (Beef `[Friend]`).
    struct EditTxnEditor : BoolEditor
    {
        EditTxnEditor(StringView name, bool value) : BoolEditor(name, value) {}
        using PropertyEditor::BeginEdit;
        using PropertyEditor::EndEdit;
    };
}

// === MenuBar ===

TEST_CASE("toolkit: MenuBar_AddMenu")
{
    auto menuBar = MakeRef<MenuBar>(DefaultAllocator());
    ContextMenu* menu = menuBar->AddMenu(u8"File");
    CHECK(menu != nullptr);
    CHECK(menuBar->MenuCount() == 1);

    menuBar->AddMenu(u8"Edit");
    CHECK(menuBar->MenuCount() == 2);
}

// === Toolbar ===

TEST_CASE("toolkit: Toolbar_AddButton")
{
    auto toolbar = MakeRef<Toolbar>(DefaultAllocator());
    ToolbarButton* btn = toolbar->AddButton(u8"Save");
    CHECK(btn != nullptr);
    CHECK(toolbar->ChildCount() == 1);
}

TEST_CASE("toolkit: Toolbar_AddSeparator")
{
    auto toolbar = MakeRef<Toolbar>(DefaultAllocator());
    toolbar->AddButton(u8"A");
    toolbar->AddSeparator();
    toolbar->AddButton(u8"B");
    CHECK(toolbar->ChildCount() == 3);
}

TEST_CASE("toolkit: Toolbar_AddToggle")
{
    auto toolbar = MakeRef<Toolbar>(DefaultAllocator());
    ToolbarToggle* toggle = toolbar->AddToggle(u8"Bold");
    CHECK(toggle != nullptr);
    CHECK(!toggle->IsChecked());
    toggle->SetIsChecked(true);
    CHECK(toggle->IsChecked());
}

// === StatusBar ===

TEST_CASE("toolkit: StatusBar_SetText")
{
    auto statusBar = MakeRef<StatusBar>(DefaultAllocator());
    statusBar->SetText(u8"Ready");
    // SetText creates a default label as first child.
    CHECK(statusBar->ChildCount() >= 1);
}

TEST_CASE("toolkit: StatusBar_AddSection")
{
    auto statusBar = MakeRef<StatusBar>(DefaultAllocator());
    Label* section = statusBar->AddSection(u8"UTF-8");
    CHECK(section != nullptr);
}

// === SplitView ===

TEST_CASE("toolkit: SplitView_RatioClamping")
{
    auto sv = MakeRef<SplitView>(DefaultAllocator());
    sv->SetSplitRatio(-1);
    CHECK(sv->SplitRatio() == 0);

    sv->SetSplitRatio(2);
    CHECK(sv->SplitRatio() == 1);
}

TEST_CASE("toolkit: SplitView_SetPanes")
{
    auto sv = MakeRef<SplitView>(DefaultAllocator());
    auto first = MakeRef<Label>(DefaultAllocator(), StringView(u8"A"));
    auto second = MakeRef<Label>(DefaultAllocator(), StringView(u8"B"));
    sv->SetPanes(first.Get(), second.Get());

    CHECK(sv->FirstPane() == first.Get());
    CHECK(sv->SecondPane() == second.Get());
}

// === BreadcrumbBar ===

TEST_CASE("toolkit: BreadcrumbBar_SetPath")
{
    auto bar = MakeRef<BreadcrumbBar>(DefaultAllocator());
    bar->SetPath(u8"Project/Assets/Textures");

    CHECK(bar->SegmentCount() == 3);
    CHECK(bar->GetSegment(0) == StringView(u8"Project"));
    CHECK(bar->GetSegment(1) == StringView(u8"Assets"));
    CHECK(bar->GetSegment(2) == StringView(u8"Textures"));
}

TEST_CASE("toolkit: BreadcrumbBar_SetSegments")
{
    auto bar = MakeRef<BreadcrumbBar>(DefaultAllocator());
    StringView segs[] = {u8"Home", u8"Documents", u8"File.txt"};
    bar->SetSegments(Span<StringView>(segs, 3));

    CHECK(bar->SegmentCount() == 3);
    CHECK(bar->GetSegment(0) == StringView(u8"Home"));
    CHECK(bar->GetSegment(2) == StringView(u8"File.txt"));
}

TEST_CASE("toolkit: BreadcrumbBar_GetPathUpTo")
{
    auto bar = MakeRef<BreadcrumbBar>(DefaultAllocator());
    bar->SetPath(u8"A/B/C/D");

    String path;
    bar->GetPathUpTo(1, path);
    CHECK(path == StringView(u8"A/B"));
}

TEST_CASE("toolkit: BreadcrumbBar_OnSegmentClicked")
{
    auto bar = MakeRef<BreadcrumbBar>(DefaultAllocator());
    bar->SetPath(u8"A/B/C");

    bool fired = false;
    i32 firedIndex = -1;
    bar->OnSegmentClicked.Add(
        [&fired, &firedIndex](BreadcrumbBar*, i32 idx)
        {
            fired = true;
            firedIndex = idx;
        });

    // Can't easily simulate click without context, but event should be wired.
    CHECK(!fired);
    CHECK(firedIndex == -1);
}

// === ColorPicker ===

TEST_CASE("toolkit: ColorPicker_DefaultColor")
{
    auto picker = MakeRef<ColorPicker>(DefaultAllocator());
    const foundation::Color color = picker->CurrentColor();
    CHECK(color.a == 1.0f);
}

TEST_CASE("toolkit: ColorPicker_SetColor")
{
    auto picker = MakeRef<ColorPicker>(DefaultAllocator());
    picker->SetColor(Rgb(128, 64, 32, 255));
    const foundation::Color c = picker->CurrentColor();
    // Should round-trip approximately (HSV conversion may lose precision).
    CHECK(Abs(c.r - 128 / 255.0f) <= 2 / 255.0f);
    CHECK(Abs(c.g - 64 / 255.0f) <= 2 / 255.0f);
    CHECK(Abs(c.b - 32 / 255.0f) <= 2 / 255.0f);
    CHECK(c.a == 1.0f);
}

TEST_CASE("toolkit: ColorPicker_SetOriginalColor")
{
    auto picker = MakeRef<ColorPicker>(DefaultAllocator());
    picker->SetOriginalColor(Rgb(255, 0, 0, 255));
    // No crash, original preview updated.
}

TEST_CASE("toolkit: ColorPicker_OnColorChanged")
{
    auto picker = MakeRef<ColorPicker>(DefaultAllocator());

    bool fired = false;
    picker->OnColorChanged.Add([&fired](ColorPicker*, foundation::Color) { fired = true; });

    // Programmatic SetColor does NOT fire OnColorChanged (avoids feedback loops).
    picker->SetColor(Rgb(0, 255, 0, 255));
    CHECK(!fired);
}

TEST_CASE("toolkit: ColorPicker_HSVToRGB_Red")
{
    const foundation::Color c = ColorPicker::HSVToRGB(0, 1, 1);
    CHECK(c.r == 1.0f);
    CHECK(c.g == 0);
    CHECK(c.b == 0);
}

TEST_CASE("toolkit: ColorPicker_HSVToRGB_Green")
{
    const foundation::Color c = ColorPicker::HSVToRGB(120, 1, 1);
    CHECK(c.r == 0);
    CHECK(c.g == 1.0f);
    CHECK(c.b == 0);
}

TEST_CASE("toolkit: ColorPicker_HSVToRGB_Blue")
{
    const foundation::Color c = ColorPicker::HSVToRGB(240, 1, 1);
    CHECK(c.r == 0);
    CHECK(c.g == 0);
    CHECK(c.b == 1.0f);
}

TEST_CASE("toolkit: ColorPicker_HSVToRGB_White")
{
    const foundation::Color c = ColorPicker::HSVToRGB(0, 0, 1);
    CHECK(c.r == 1.0f);
    CHECK(c.g == 1.0f);
    CHECK(c.b == 1.0f);
}

TEST_CASE("toolkit: ColorPicker_HSVToRGB_Black")
{
    const foundation::Color c = ColorPicker::HSVToRGB(0, 0, 0);
    CHECK(c.r == 0);
    CHECK(c.g == 0);
    CHECK(c.b == 0);
}

TEST_CASE("toolkit: ColorPicker_RGBToHSV_Roundtrip")
{
    f32 h = 0, s = 0, v = 0;
    ColorPicker::RGBToHSV(1.0f, 0.5f, 0.25f, h, s, v);
    const foundation::Color c = ColorPicker::HSVToRGB(h, s, v);
    CHECK(Abs(c.r - 1.0f) <= 1.0f / 255.0f);
    CHECK(Abs(c.g - 0.5f) <= 1.0f / 255.0f);
    CHECK(Abs(c.b - 0.25f) <= 1.0f / 255.0f);
}

// === PropertyGrid ===

TEST_CASE("toolkit: PropertyGrid_AddProperty")
{
    auto grid = MakeRef<PropertyGrid>(DefaultAllocator());
    auto editor = MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"Enabled"), true);
    grid->AddProperty(editor);
    CHECK(grid->PropertyCount() == 1);
}

TEST_CASE("toolkit: PropertyGrid_GetProperty")
{
    auto grid = MakeRef<PropertyGrid>(DefaultAllocator());
    auto editor = MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"Enabled"), true);
    PropertyEditor* raw = editor.Get();
    grid->AddProperty(editor);

    PropertyEditor* found = grid->GetProperty(u8"Enabled");
    CHECK(found == raw);

    PropertyEditor* notFound = grid->GetProperty(u8"Missing");
    CHECK(notFound == nullptr);
}

TEST_CASE("toolkit: PropertyGrid_RemoveProperty")
{
    auto grid = MakeRef<PropertyGrid>(DefaultAllocator());
    grid->AddProperty(MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"A"), false));
    grid->AddProperty(MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"B"), true));
    CHECK(grid->PropertyCount() == 2);

    grid->RemoveProperty(u8"A");
    CHECK(grid->PropertyCount() == 1);
}

TEST_CASE("toolkit: PropertyGrid_Clear")
{
    auto grid = MakeRef<PropertyGrid>(DefaultAllocator());
    grid->AddProperty(MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"A"), false));
    grid->AddProperty(MakeRef<IntEditor>(DefaultAllocator(), StringView(u8"B"), i64{42}));
    grid->Clear();
    CHECK(grid->PropertyCount() == 0);
}

// === PropertyEditor types ===

TEST_CASE("toolkit: BoolEditor_Value")
{
    auto editor = MakeRef<BoolEditor>(DefaultAllocator(), StringView(u8"Flag"), true);
    CHECK(editor->Value() == true);
    editor->SetValue(false);
    CHECK(editor->Value() == false);
}

TEST_CASE("toolkit: FloatEditor_Value")
{
    auto editor = MakeRef<FloatEditor>(DefaultAllocator(), StringView(u8"Speed"), 3.14);
    CHECK(Abs(editor->Value() - 3.14) < 0.01);
    editor->SetValue(2.0);
    CHECK(Abs(editor->Value() - 2.0) < 0.01);
}

TEST_CASE("toolkit: IntEditor_Value")
{
    auto editor = MakeRef<IntEditor>(DefaultAllocator(), StringView(u8"Count"), i64{42});
    CHECK(editor->Value() == 42);
    editor->SetValue(100);
    CHECK(editor->Value() == 100);
}

TEST_CASE("toolkit: StringEditor_Value")
{
    auto editor =
        MakeRef<StringEditor>(DefaultAllocator(), StringView(u8"Name"), StringView(u8"Hello"));
    CHECK(editor->Value() == StringView(u8"Hello"));
    editor->SetValue(u8"World");
    CHECK(editor->Value() == StringView(u8"World"));
}

TEST_CASE("toolkit: EnumEditor_Value")
{
    StringView items[] = {u8"Off", u8"On", u8"Auto"};
    auto editor = MakeRef<EnumEditor>(DefaultAllocator(), StringView(u8"Mode"), i32{1},
                                      Span<const StringView>(items, 3));
    CHECK(editor->Value() == 1);
    editor->SetValue(2);
    CHECK(editor->Value() == 2);
}

TEST_CASE("toolkit: RangeEditor_Value")
{
    auto editor =
        MakeRef<RangeEditor>(DefaultAllocator(), StringView(u8"Volume"), 0.5f, 0.0f, 1.0f);
    CHECK(Abs(editor->Value() - 0.5f) < 0.01f);
    editor->SetValue(0.8f);
    CHECK(Abs(editor->Value() - 0.8f) < 0.01f);
}

TEST_CASE("toolkit: ColorEditor_Value")
{
    auto editor =
        MakeRef<ColorEditor>(DefaultAllocator(), StringView(u8"Tint"), Rgb(255, 0, 0, 255));
    CHECK(editor->Value().r == 1.0f);
    CHECK(editor->Value().g == 0);
    editor->SetValue(Rgb(0, 255, 0, 255));
    CHECK(editor->Value().g == 1.0f);
}

TEST_CASE("toolkit: Float3Editor_Value")
{
    auto editor =
        MakeRef<Float3Editor>(DefaultAllocator(), StringView(u8"Position"), Float3{1, 2, 3});
    CHECK(editor->Value().x == 1);
    CHECK(editor->Value().y == 2);
    CHECK(editor->Value().z == 3);
    editor->SetValue(Float3{4, 5, 6});
    CHECK(editor->Value().x == 4);
}

TEST_CASE("toolkit: PropertyEditor_EditTransaction")
{
    auto editor = MakeRef<EditTxnEditor>(DefaultAllocator(), StringView(u8"Test"), false);

    bool began = false;
    bool ended = false;
    editor->OnEditBegin.Add([&began](PropertyEditor*) { began = true; });
    editor->OnEditEnd.Add([&ended](PropertyEditor*) { ended = true; });

    editor->BeginEdit();
    CHECK(began);
    CHECK(editor->IsEditing());

    editor->EndEdit();
    CHECK(ended);
    CHECK(!editor->IsEditing());
}
