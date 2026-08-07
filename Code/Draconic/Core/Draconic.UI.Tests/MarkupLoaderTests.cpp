// Ported from Sedulous.UI.Tests/src/MarkupLoaderTests.bf (faithful). Beef `as X` -> Cast<X>; `scope`/
// `new`+delete -> RefPtr (RAII); property .Value -> .Value(); Color byte-literal -> float(/255). The
// static ctor (MarkupLoader.Initialize + StyleSheetLoader.InitializeGlobals) -> an idempotent EnsureInit()
// called per test.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static void EnsureInit()
{
    MarkupLoader::Initialize();
    StyleSheetLoader::InitializeGlobals();
}

// === Basic element creation ===

TEST_CASE("markup: CreatesLabel")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label text=\"Hello\"/>");
    REQUIRE(view);
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->Text.Value() == u8"Hello");
}

TEST_CASE("markup: CreatesButton")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Button text=\"Click Me\"/>");
    REQUIRE(view);
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->Text.Value() == u8"Click Me");
}

TEST_CASE("markup: TextContent_SetsText")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Button>Click Me</Button>");
    REQUIRE(view);
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->Text.Value() == u8"Click Me");
}

// === Hierarchy ===

TEST_CASE("markup: FlexWithChildren")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"vertical\" spacing=\"8\">\n"
                                             u8"  <Label text=\"First\"/>\n"
                                             u8"  <Label text=\"Second\"/>\n"
                                             u8"</Flex>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    CHECK(flex->Direction == Orientation::Vertical);
    CHECK(flex->Spacing == 8);
    CHECK(flex->ChildCount() == 2);

    Label* first = Cast<Label>(flex->GetChildAt(0));
    REQUIRE(first != nullptr);
    CHECK(first->Text.Value() == u8"First");
    Label* second = Cast<Label>(flex->GetChildAt(1));
    REQUIRE(second != nullptr);
    CHECK(second->Text.Value() == u8"Second");
}

TEST_CASE("markup: NestedHierarchy")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"vertical\">\n"
                                             u8"  <Flex direction=\"horizontal\" spacing=\"4\">\n"
                                             u8"    <Button text=\"A\"/>\n"
                                             u8"    <Button text=\"B\"/>\n"
                                             u8"  </Flex>\n"
                                             u8"  <Label text=\"Footer\"/>\n"
                                             u8"</Flex>");
    REQUIRE(view);
    FlexLayout* outer = Cast<FlexLayout>(view.Get());
    REQUIRE(outer != nullptr);
    CHECK(outer->ChildCount() == 2);
    FlexLayout* inner = Cast<FlexLayout>(outer->GetChildAt(0));
    REQUIRE(inner != nullptr);
    CHECK(inner->Direction == Orientation::Horizontal);
    CHECK(inner->ChildCount() == 2);
}

// === Special attributes ===

TEST_CASE("markup: IdAttribute_RegistersName")
{
    EnsureInit();
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"vertical\">\n"
                                             u8"  <Button id=\"my-btn\" text=\"OK\"/>\n"
                                             u8"  <Label id=\"my-label\" text=\"Status\"/>\n"
                                             u8"</Flex>",
                                             &ctx);
    root->AddView(view.Get());

    Button* btn = root->FindByName<Button>(u8"my-btn");
    REQUIRE(btn != nullptr);
    CHECK(btn->Text.Value() == u8"OK");
    Label* label = root->FindByName<Label>(u8"my-label");
    REQUIRE(label != nullptr);
    CHECK(label->Text.Value() == u8"Status");
}

TEST_CASE("markup: ClassAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label class=\"primary large\" text=\"Styled\"/>");
    REQUIRE(view);
    Label* label = Cast<Label>(view.Get());
    CHECK(label->HasClass(u8"primary"));
    CHECK(label->HasClass(u8"large"));
}

TEST_CASE("markup: VisibilityAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label visibility=\"gone\" text=\"Hidden\"/>");
    REQUIRE(view);
    CHECK(view->Visibility == Visibility::Gone);
}

TEST_CASE("markup: IsEnabledAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Button is-enabled=\"false\" text=\"Disabled\"/>");
    REQUIRE(view);
    CHECK(view->IsEnabled == false);
}

TEST_CASE("markup: OpacityAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label opacity=\"0.5\" text=\"Faded\"/>");
    REQUIRE(view);
    CHECK(view->Opacity == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("markup: PaddingAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex padding=\"8 12\">\n"
                                             u8"  <Label text=\"Padded\"/>\n"
                                             u8"</Flex>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    CHECK(flex->Padding.Top == 8);
    CHECK(flex->Padding.Left == 12);
}

// === Layout params ===

TEST_CASE("markup: WidthHeight_LayoutParams")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(u8"<Flex direction=\"horizontal\">\n"
                                     u8"  <Label text=\"Fixed\" width=\"200\" height=\"40\"/>\n"
                                     u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    View* child = flex->GetChildAt(0);
    REQUIRE(child->LayoutParams);
    CHECK(child->LayoutParams->Width.kind == SizeSpec::Kind::Fixed);
    CHECK(child->LayoutParams->Height.kind == SizeSpec::Kind::Fixed);
}

TEST_CASE("markup: MatchWrap_LayoutParams")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(u8"<Flex>\n"
                                     u8"  <Label text=\"Match\" width=\"match\" height=\"wrap\"/>\n"
                                     u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    View* child = flex->GetChildAt(0);
    CHECK(child->LayoutParams->Width.kind == SizeSpec::Kind::Match);
    CHECK(child->LayoutParams->Height.kind == SizeSpec::Kind::Wrap);
}

TEST_CASE("markup: FlexGrow_LayoutParam")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"horizontal\">\n"
                                             u8"  <Button text=\"A\" grow=\"1\"/>\n"
                                             u8"  <Button text=\"B\" grow=\"2\"/>\n"
                                             u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    FlexLayoutParams* alpA = Cast<FlexLayoutParams>(flex->GetChildAt(0)->LayoutParams.Get());
    FlexLayoutParams* alpB = Cast<FlexLayoutParams>(flex->GetChildAt(1)->LayoutParams.Get());
    REQUIRE(alpA != nullptr);
    CHECK(alpA->Grow == 1);
    REQUIRE(alpB != nullptr);
    CHECK(alpB->Grow == 2);
}

TEST_CASE("markup: FrameGravity_LayoutParam")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Frame>\n"
                                             u8"  <Label text=\"Centered\" gravity=\"Center\"/>\n"
                                             u8"</Frame>");
    FrameLayout* frame = Cast<FrameLayout>(view.Get());
    FrameLayoutParams* flp = Cast<FrameLayoutParams>(frame->GetChildAt(0)->LayoutParams.Get());
    REQUIRE(flp != nullptr);
    CHECK(flp->Gravity == Gravity::Center);
}

TEST_CASE("markup: DockLayout_Param")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Dock>\n"
                                             u8"  <Label text=\"Top\" dock=\"top\"/>\n"
                                             u8"  <Label text=\"Fill\" dock=\"fill\"/>\n"
                                             u8"</Dock>");
    DockLayout* dock = Cast<DockLayout>(view.Get());
    CHECK(dock->ChildCount() == 2);
    DockLayoutParams* dlp = Cast<DockLayoutParams>(dock->GetChildAt(0)->LayoutParams.Get());
    REQUIRE(dlp != nullptr);
    CHECK(dlp->Dock == Dock::Top);
}

// === Control properties ===

TEST_CASE("markup: CheckBox_Properties")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<CheckBox text=\"Accept\" is-checked=\"true\"/>");
    CheckBox* cb = Cast<CheckBox>(view.Get());
    REQUIRE(cb != nullptr);
    CHECK(cb->Text.Value() == u8"Accept");
    CHECK(cb->IsChecked.Value() == true);
}

TEST_CASE("markup: Slider_Properties")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(u8"<Slider min=\"0\" max=\"100\" value=\"50\" step=\"5\"/>");
    Slider* slider = Cast<Slider>(view.Get());
    REQUIRE(slider != nullptr);
    CHECK(slider->Min.Value() == 0);
    CHECK(slider->Max.Value() == 100);
    CHECK(slider->Value.Value() == 50);
    CHECK(slider->Step.Value() == 5);
}

TEST_CASE("markup: EditText_Properties")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<EditText text=\"hello\" placeholder=\"Type here\" "
                                             u8"is-read-only=\"false\" max-length=\"100\"/>");
    EditText* edit = Cast<EditText>(view.Get());
    REQUIRE(edit != nullptr);
    CHECK(edit->Text() == u8"hello");
    CHECK(edit->MaxLength.Value() == 100);
}

TEST_CASE("markup: ProgressBar_Properties")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<ProgressBar value=\"0.75\"/>");
    ProgressBar* pb = Cast<ProgressBar>(view.Get());
    REQUIRE(pb != nullptr);
    CHECK(pb->Value.Value() == doctest::Approx(0.75f).epsilon(0.01));
}

TEST_CASE("markup: Label_FontSize")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label text=\"Big\" font-size=\"24\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    REQUIRE(label->FontSize.Value().HasValue());
    CHECK(label->FontSize.Value().Value() == 24);
}

TEST_CASE("markup: Label_FontFamily")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        u8"<Label text=\"Decorative\" font-family=\"JungleAdventurer\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->FontFamily.Value() == u8"JungleAdventurer");
}

TEST_CASE("markup: Button_FontFamily")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(u8"<Button text=\"Go\" font-family=\"AttackOfMonster\"/>");
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->FontFamily.Value() == u8"AttackOfMonster");
}

// === style="..." inline-style attribute ===

TEST_CASE("markup: Style_SinglePrimitive")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Label text=\"hi\" style=\"font-size: 22;\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 22.0f);
}

TEST_CASE("markup: Style_MultipleDeclarations")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        u8"<Label text=\"hi\" style=\"font-size: 18; text-color: #ff0000; padding: 4 8;\"/>");
    Label* label = Cast<Label>(view.Get());
    CHECK(label->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 18.0f);
    Optional<Color> c = label->GetInlineStyle(StyleProperty::TextColor).AsColor();
    REQUIRE(c.HasValue());
    CHECK(c.Value().r == 1.0f);
    CHECK(c.Value().g == 0);
    Optional<Thickness> pad = label->GetInlineStyle(StyleProperty::Padding).AsThickness();
    REQUIRE(pad.HasValue());
    CHECK(pad.Value().Top == 4);
    CHECK(pad.Value().Left == 8);
}

TEST_CASE("markup: Style_StringProperty")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        u8"<Label text=\"hi\" style=\"font-family: JungleAdventurer;\"/>");
    Label* label = Cast<Label>(view.Get());
    CHECK(label->GetInlineStyle(StyleProperty::FontFamily).AsString().Value() ==
          u8"JungleAdventurer");
}

TEST_CASE("markup: Style_BeatsContextSheetRule")
{
    EnsureInit();
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto sheet = foundation::MakeRef<StyleSheet>(foundation::DefaultAllocator());
    ctx.SetStyleSheet(sheet);
    sheet->ForType(&Label::StaticType()).Set(StyleProperty::TextColor, Color{0, 0, 1, 1});

    auto view =
        MarkupLoader::LoadFromString(u8"<Label text=\"hi\" style=\"text-color: #00ff00;\"/>", &ctx);
    root->AddView(view.Get());

    Label* label = Cast<Label>(view.Get());
    // Inline (green) wins over context-sheet (blue).
    const Color c = label->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(c.g == 1.0f);
    CHECK(c.b == 0);
}

TEST_CASE("markup: Style_DrawableValue_OwnedByView")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Panel style=\"background: rgb(40, 120, 60);\"/>");
    Panel* panel = Cast<Panel>(view.Get());
    REQUIRE(panel != nullptr);
    Drawable* bg = panel->GetInlineStyle(StyleProperty::Background).AsDrawable();
    REQUIRE(bg != nullptr);
    CHECK(Cast<ColorDrawable>(bg) != nullptr);
}

TEST_CASE("markup: Style_OnVariousTags")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex>\n"
                                             u8"  <Button text=\"A\" style=\"font-size: 14;\"/>\n"
                                             u8"  <CheckBox text=\"B\" style=\"font-size: 16;\"/>\n"
                                             u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    Button* btn = Cast<Button>(flex->GetChildAt(0));
    CheckBox* cb = Cast<CheckBox>(flex->GetChildAt(1));
    CHECK(btn->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 14.0f);
    CHECK(cb->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 16.0f);
}

// === Aliases ===

TEST_CASE("markup: FlexLayout_Alias")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<FlexLayout direction=\"horizontal\">\n"
                                             u8"  <Label text=\"A\"/>\n"
                                             u8"</FlexLayout>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    CHECK(flex->Direction == Orientation::Horizontal);
}

// === Unknown elements / invalid / empty ===

TEST_CASE("markup: UnknownElement_ReturnsNull")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<NonExistentWidget/>");
    CHECK(!view);
}

TEST_CASE("markup: InvalidXml_ReturnsNull")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<not valid xml");
    CHECK(!view);
}

TEST_CASE("markup: EmptyContainer")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"vertical\"/>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    CHECK(flex->ChildCount() == 0);
}

// === Margin on layout params ===

TEST_CASE("markup: Margin_LayoutParam")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(u8"<Flex>\n"
                                             u8"  <Label text=\"Margined\" margin=\"4 8\"/>\n"
                                             u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    View* child = flex->GetChildAt(0);
    CHECK(child->LayoutParams->Margin.Top == 4);
    CHECK(child->LayoutParams->Margin.Left == 8);
}

// === Style resolution with markup (theme) ===

TEST_CASE("markup: StyleClass_ResolvesTheme")
{
    EnsureInit();
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto sheet = DarkTheme::Create();
    ctx.SetStyleSheet(sheet);

    auto view = MarkupLoader::LoadFromString(u8"<Flex direction=\"vertical\">\n"
                                             u8"  <Button id=\"btn\" text=\"Themed\"/>\n"
                                             u8"</Flex>",
                                             &ctx);
    root->AddView(view.Get());

    Button* btn = root->FindByName<Button>(u8"btn");
    REQUIRE(btn != nullptr);
    // Button resolves its background from the theme (ButtonBase type selector).
    Drawable* bg = btn->ResolveStyleDrawable(StyleProperty::Background);
    CHECK(bg != nullptr);
}
