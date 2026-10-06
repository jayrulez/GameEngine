// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/MarkupLoaderTests.bf (faithful). Beef `as X` -> Cast<X>; `scope`/
// `new`+delete -> RefPtr (RAII); property .Value -> .Value(); Color byte-literal -> float(/255). The
// static ctor (MarkupLoader.Initialize + StyleSheetLoader.InitializeGlobals) -> an idempotent EnsureInit()
// called per test.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

static void EnsureInit()
{
    MarkupLoader::Initialize();
    StyleSheetLoader::InitializeGlobals();
}

// === Basic element creation ===

TEST_CASE("markup: CreatesLabel")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"Hello\"/>");
    REQUIRE(view);
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->Text.Value() == u8"Hello");
}

TEST_CASE("markup: an ImageView's source attribute names what it shows")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<ImageView source=\"{0a1b}\"/>");
    REQUIRE(view);
    ImageView* image = Cast<ImageView>(view.Get());
    REQUIRE(image != nullptr);
    CHECK(image->Source.Value() == u8"{0a1b}");
}

TEST_CASE("markup: CreatesButton")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Button text=\"Click Me\"/>");
    REQUIRE(view);
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->Text.Value() == u8"Click Me");
}

TEST_CASE("markup: TextContent_SetsText")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Button>Click Me</Button>");
    REQUIRE(view);
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->Text.Value() == u8"Click Me");
}

// === Hierarchy ===

TEST_CASE("markup: FlexWithChildren")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"vertical\" spacing=\"8\">\n"
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"vertical\">\n"
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
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"vertical\">\n"
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label class=\"primary large\" text=\"Styled\"/>");
    REQUIRE(view);
    Label* label = Cast<Label>(view.Get());
    CHECK(label->HasClass(u8"primary"));
    CHECK(label->HasClass(u8"large"));
}

TEST_CASE("markup: VisibilityAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label visibility=\"gone\" text=\"Hidden\"/>");
    REQUIRE(view);
    CHECK(view->Visibility == Visibility::Gone);
}

TEST_CASE("markup: IsEnabledAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Button is-enabled=\"false\" text=\"Disabled\"/>");
    REQUIRE(view);
    CHECK(view->IsEnabled == false);
}

TEST_CASE("markup: OpacityAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label opacity=\"0.5\" text=\"Faded\"/>");
    REQUIRE(view);
    CHECK(view->Opacity == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("markup: PaddingAttribute")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex padding=\"8 12\">\n"
                                             u8"  <Label text=\"Padded\"/>\n"
                                             u8"</Flex>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    CHECK(flex->Padding.Top == 8);
    CHECK(flex->Padding.Left == 12);
}

// === Layout params ===

TEST_CASE("markup: WidthHeight_LayoutStyle")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"horizontal\">\n"
                                     u8"  <Label text=\"Fixed\" width=\"200\" height=\"40\"/>\n"
                                     u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    View* child = flex->GetChildAt(0);
    CHECK(child->Layout().Width->kind == SizeSpec::Kind::Fixed);
    CHECK(child->Layout().Height->kind == SizeSpec::Kind::Fixed);
}

TEST_CASE("markup: MatchWrap_LayoutStyle")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex>\n"
                                     u8"  <Label text=\"Match\" width=\"match\" height=\"wrap\"/>\n"
                                     u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    View* child = flex->GetChildAt(0);
    CHECK(child->Layout().Width->kind == SizeSpec::Kind::Match);
    CHECK(child->Layout().Height->kind == SizeSpec::Kind::Wrap);
}

TEST_CASE("markup: FlexGrow_LayoutStyle")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        DefaultAllocator(),
        u8"<Flex direction=\"horizontal\">\n"
        u8"  <Button text=\"A\" flex-grow=\"1\"/>\n"
        u8"  <Button text=\"B\" flex-grow=\"2\" flex-shrink=\"0.5\" align-self=\"center\"/>\n"
        u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    CHECK(flex->GetChildAt(0)->Layout().FlexGrow == 1);
    CHECK(flex->GetChildAt(1)->Layout().FlexGrow == 2);
    CHECK(flex->GetChildAt(1)->Layout().FlexShrink.Value() == doctest::Approx(0.5f));
    REQUIRE(flex->GetChildAt(1)->Layout().AlignSelf.HasValue());
    CHECK(flex->GetChildAt(1)->Layout().AlignSelf.Value() == Align::Center);
    CHECK(!flex->GetChildAt(0)->Layout().AlignSelf.HasValue());
}

TEST_CASE("markup: FrameGravity_LayoutStyle")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Frame>\n"
                                             u8"  <Label text=\"Centered\" gravity=\"Center\"/>\n"
                                             u8"</Frame>");
    FrameLayout* frame = Cast<FrameLayout>(view.Get());
    REQUIRE(frame != nullptr);
    CHECK(frame->GetChildAt(0)->Layout().Gravity == Gravity::Center);
}

TEST_CASE("markup: Dock_LayoutStyle")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Dock>\n"
                                             u8"  <Label text=\"Top\" dock=\"top\"/>\n"
                                             u8"  <Label text=\"Fill\" dock=\"fill\"/>\n"
                                             u8"</Dock>");
    DockLayout* dock = Cast<DockLayout>(view.Get());
    REQUIRE(dock != nullptr);
    CHECK(dock->ChildCount() == 2);
    CHECK(dock->GetChildAt(0)->Layout().Dock == Dock::Top);
    CHECK(dock->GetChildAt(1)->Layout().Dock == Dock::Fill);
}

TEST_CASE("markup: Grid_And_Absolute_LayoutStyle")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        DefaultAllocator(),
        u8"<Grid>\n"
        u8"  <Label text=\"Cell\" grid-row=\"1\" grid-column=\"2\" grid-row-span=\"3\""
        u8" grid-column-span=\"4\" left=\"7\" top=\"9\"/>\n"
        u8"</Grid>");
    GridLayout* grid = Cast<GridLayout>(view.Get());
    REQUIRE(grid != nullptr);
    const LayoutStyle& ls = grid->GetChildAt(0)->Layout();
    CHECK(ls.GridRow == 1);
    CHECK(ls.GridColumn == 2);
    CHECK(ls.GridRowSpan == 3);
    CHECK(ls.GridColumnSpan == 4);
    CHECK(ls.Left == 7);
    CHECK(ls.Top == 9);
}

TEST_CASE("markup: LayoutAttributes_ApplyRegardlessOfParent")
{
    // The vocabulary is the same on every element: a Frame child may carry flex-grow and
    // dock (they wait for a Flex/Dock parent), and width/height/margin apply even at the root.
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(
        DefaultAllocator(), u8"<Frame width=\"300\" margin=\"4\">\n"
                            u8"  <Label text=\"X\" flex-grow=\"1\" dock=\"right\" gravity=\"Bottom\"/>\n"
                            u8"</Frame>");
    REQUIRE(view.Get() != nullptr);
    CHECK(view->Layout().Width->kind == SizeSpec::Kind::Fixed);
    CHECK(view->Layout().Margin->Left == 4);
    FrameLayout* frame = Cast<FrameLayout>(view.Get());
    REQUIRE(frame != nullptr);
    const LayoutStyle& ls = frame->GetChildAt(0)->Layout();
    CHECK(ls.FlexGrow == 1);
    CHECK(ls.Dock == Dock::Right);
    CHECK(ls.Gravity == Gravity::Bottom);
}

TEST_CASE("markup: OldParentTypedAttributes_AreUnknown")
{
    // No aliases: the pre-LayoutStyle `grow=` spelling is an unknown attribute (surfaced as
    // a diagnostic), not silently mapped.
    EnsureInit();
    Array<String> warnings;
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(),
                                             u8"<Flex><Button text=\"A\" grow=\"1\"/></Flex>",
                                             nullptr, &warnings);
    REQUIRE(view.Get() != nullptr);
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].AsView().StartsWith(u8"unknown attribute 'grow'"));
    CHECK(Cast<FlexLayout>(view.Get())->GetChildAt(0)->Layout().FlexGrow == 0);
}

TEST_CASE("markup: an unknown gravity name is surfaced, the known ones still apply")
{
    // Gravity names are PascalCase and matched exactly: `top|right` places nothing, which used
    // to pass in silence (a HUD panel left in the wrong corner).
    EnsureInit();
    Array<String> warnings;
    auto view = MarkupLoader::LoadFromString(
        DefaultAllocator(), u8"<FrameLayout><Panel gravity=\"top|Right\"/></FrameLayout>", nullptr,
        &warnings);
    REQUIRE(view.Get() != nullptr);
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].AsView().StartsWith(u8"unknown gravity 'top' on <Panel>"));
    CHECK(Cast<ViewGroup>(view.Get())->GetChildAt(0)->Layout().Gravity == Gravity::Right);

    warnings.Clear();
    (void)MarkupLoader::LoadFromString(
        DefaultAllocator(), u8"<FrameLayout><Panel gravity=\"Top|Right\"/></FrameLayout>", nullptr,
        &warnings);
    CHECK(warnings.IsEmpty());
}

TEST_CASE("markup: LayoutAttributeNames_CoverTheVocabulary")
{
    Array<String> names;
    MarkupRegistry::CollectAttributeNames(u8"Label", names);
    for (const StringView& expected : MarkupRegistry::LayoutAttributeNames())
    {
        bool found = false;
        for (const String& n : names)
        {
            found = found || n.AsView() == expected;
        }
        CHECK_MESSAGE(found, "missing layout attribute in completion vocabulary");
    }
}

// === Control properties ===

TEST_CASE("markup: CheckBox_Properties")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<CheckBox text=\"Accept\" is-checked=\"true\"/>");
    CheckBox* cb = Cast<CheckBox>(view.Get());
    REQUIRE(cb != nullptr);
    CHECK(cb->Text.Value() == u8"Accept");
    CHECK(cb->IsChecked.Value() == true);
}

TEST_CASE("markup: Slider_Properties")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Slider min=\"0\" max=\"100\" value=\"50\" step=\"5\"/>");
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<EditText text=\"hello\" placeholder=\"Type here\" "
                                             u8"is-read-only=\"false\" max-length=\"100\"/>");
    EditText* edit = Cast<EditText>(view.Get());
    REQUIRE(edit != nullptr);
    CHECK(edit->Text() == u8"hello");
    CHECK(edit->MaxLength.Value() == 100);
}

TEST_CASE("markup: ProgressBar_Properties")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<ProgressBar value=\"0.75\"/>");
    ProgressBar* pb = Cast<ProgressBar>(view.Get());
    REQUIRE(pb != nullptr);
    CHECK(pb->Value.Value() == doctest::Approx(0.75f).epsilon(0.01));
}

TEST_CASE("markup: Label_FontSize")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"Big\" font-size=\"24\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    REQUIRE(label->FontSize.Value().HasValue());
    CHECK(label->FontSize.Value().Value() == 24);
}

TEST_CASE("markup: Label_FontFamily")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), 
        u8"<Label text=\"Decorative\" font-family=\"Chewy\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->FontFamily.Value() == u8"Chewy");
}

TEST_CASE("markup: Button_FontFamily")
{
    EnsureInit();
    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Button text=\"Go\" font-family=\"LilitaOne\"/>");
    Button* btn = Cast<Button>(view.Get());
    REQUIRE(btn != nullptr);
    CHECK(btn->FontFamily.Value() == u8"LilitaOne");
}

// === style="..." inline-style attribute ===

TEST_CASE("markup: Style_SinglePrimitive")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"hi\" style=\"font-size: 22;\"/>");
    Label* label = Cast<Label>(view.Get());
    REQUIRE(label != nullptr);
    CHECK(label->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 22.0f);
}

TEST_CASE("markup: Style_MultipleDeclarations")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), 
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), 
        u8"<Label text=\"hi\" style=\"font-family: Chewy;\"/>");
    Label* label = Cast<Label>(view.Get());
    CHECK(label->GetInlineStyle(StyleProperty::FontFamily).AsString().Value() ==
          u8"Chewy");
}

TEST_CASE("markup: Style_BeatsContextSheetRule")
{
    EnsureInit();
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    ctx.SetStyleSheet(sheet);
    sheet->ForType(&Label::StaticType()).Set(StyleProperty::TextColor, Color{0, 0, 1, 1});

    auto view =
        MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"hi\" style=\"text-color: #00ff00;\"/>", &ctx);
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Panel style=\"background: rgb(40, 120, 60);\"/>");
    Panel* panel = Cast<Panel>(view.Get());
    REQUIRE(panel != nullptr);
    Drawable* bg = panel->GetInlineStyle(StyleProperty::Background).AsDrawable();
    REQUIRE(bg != nullptr);
    CHECK(Cast<ColorDrawable>(bg) != nullptr);
}

TEST_CASE("markup: Style_OnVariousTags")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex>\n"
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<FlexLayout direction=\"horizontal\">\n"
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
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<NonExistentWidget/>");
    CHECK(!view);
}

TEST_CASE("markup: InvalidXml_ReturnsNull")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<not valid xml");
    CHECK(!view);
}

TEST_CASE("markup: EmptyContainer")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"vertical\"/>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    CHECK(flex->ChildCount() == 0);
}

// === Margin on layout params ===

TEST_CASE("markup: Margin_LayoutParam")
{
    EnsureInit();
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex>\n"
                                             u8"  <Label text=\"Margined\" margin=\"4 8\"/>\n"
                                             u8"</Flex>");
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    View* child = flex->GetChildAt(0);
    CHECK(child->Layout().Margin->Top == 4);
    CHECK(child->Layout().Margin->Left == 8);
}

// === Style resolution with markup (theme) ===

TEST_CASE("markup: StyleClass_ResolvesTheme")
{
    EnsureInit();
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto sheet = DarkTheme::Create(DefaultAllocator());
    ctx.SetStyleSheet(sheet);

    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex direction=\"vertical\">\n"
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

TEST_CASE("markup: MarkupLoader::Initialize is safe to call from many threads at once")
{
    // The UI document cook initializes the markup registry per build on job workers; a plain
    // static bool let two of them rehash the builtin maps under each other (a use-after-free
    // in Tools.Cook). Hammer it, then the registry must still resolve a builtin.
    JobSystem jobs(DefaultAllocator(), 4);
    jobs.ParallelFor(64, [](u32) { MarkupLoader::Initialize(); }, 1);
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"Hello\"/>");
    REQUIRE(view);
    CHECK(Cast<Label>(view.Get()) != nullptr);
}

// === A button with any content (a picture card) ===

TEST_CASE("markup: a ContentButton takes its one child element as its content, found by name")
{
    EnsureInit();
    Array<String> warnings;
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(),
                                             u8"<Flex>\n"
                                             u8"  <ContentButton id=\"card\">\n"
                                             u8"    <Flex direction=\"vertical\">\n"
                                             u8"      <ImageView id=\"card-picture\" source=\"{0a1b}\"/>\n"
                                             u8"      <Label id=\"card-best\" text=\"Gold\"/>\n"
                                             u8"    </Flex>\n"
                                             u8"  </ContentButton>\n"
                                             u8"</Flex>",
                                             nullptr, &warnings);
    REQUIRE(view);
    CHECK(warnings.IsEmpty());
    ViewGroup* root = Cast<ViewGroup>(view.Get());
    REQUIRE(root != nullptr);
    ContentButton* card = root->FindByName<ContentButton>(u8"card");
    REQUIRE(card != nullptr);
    REQUIRE(card->Content() != nullptr);
    CHECK(card->ContentChild() == card->Content());
    // The finders reach into the content: its label and its picture.
    Label* best = root->FindByName<Label>(u8"card-best");
    REQUIRE(best != nullptr);
    CHECK(best->Text.Value() == u8"Gold");
    CHECK(root->FindByName<ImageView>(u8"card-picture") != nullptr);
    // A ContentButton is a button: a click reaches its handler.
    i32 clicks = 0;
    card->OnClick.Add([&clicks](ButtonBase*) { ++clicks; });
    card->OnClick.Invoke(card);
    CHECK(clicks == 1);
}

TEST_CASE("markup: a second content element, or children on a view that takes none, is dropped with a warning")
{
    EnsureInit();
    Array<String> warnings;
    auto view = MarkupLoader::LoadFromString(DefaultAllocator(),
                                             u8"<ContentButton><Label text=\"a\"/><Label text=\"b\"/></ContentButton>",
                                             nullptr, &warnings);
    REQUIRE(view);
    ContentButton* button = Cast<ContentButton>(view.Get());
    REQUIRE(button != nullptr);
    Label* content = Cast<Label>(button->Content());
    REQUIRE(content != nullptr);
    CHECK(content->Text.Value() == u8"a"); // the first is the content
    REQUIRE(warnings.Size() == 1u);
    CHECK(warnings[0].AsView().ContainsIgnoreCase(u8"holds one element"));

    warnings.Clear();
    auto label = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Label text=\"x\"><Label text=\"y\"/></Label>",
                                              nullptr, &warnings);
    REQUIRE(label);
    REQUIRE(warnings.Size() == 1u);
    CHECK(warnings[0].AsView().ContainsIgnoreCase(u8"holds no children"));
}
