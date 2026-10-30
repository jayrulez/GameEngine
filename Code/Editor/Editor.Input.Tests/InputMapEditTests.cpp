// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The input map page's registration, its edits as pure functions over the map (the capture landing,
// a fresh binding, the listen filter), what its grid says of a set's rows, and the page over its
// markup without a host: the tree, an action opened in place, each edit one undo step, the capture,
// and a map the game could not read.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.input;
import input.pipeline;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.app;
import editor.core;
import editor.input;

using namespace foundation::core;
namespace input = foundation::input;
namespace gui = foundation::gui;
using namespace editor::input_map_edit;

TEST_CASE("input editor: registering routes the input map asset to its factory")
{
    editor::EditorContext context{DefaultAllocator()};
    const editor::app::AssetPageServicesSource services;
    editor::RegisterInputMapEditor(context, services);
    editor::IEditorPageFactory* found = context.Pages().FindFactory(pipeline::InputMapAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::InputMapAsset::StaticType());
}

TEST_CASE("input editor: lookups are guarded and a fresh binding follows the action's kind")
{
    pipeline::InputMapAsset asset;
    asset.SeedDefaultContent();
    input::InputMap& map = asset.Map();
    CHECK(SetAt(map, 0) != nullptr);
    CHECK(SetAt(map, 1) == nullptr);
    REQUIRE(ActionAt(map, 0, 3) != nullptr);
    CHECK(ActionAt(map, 0, 3)->name == StringView(u8"Fire"));
    CHECK(ActionAt(map, 0, 4) == nullptr);
    CHECK_FALSE(HasBinding(map, 0, 0, 0));

    input::Action* move = ActionAt(map, 0, 0);
    REQUIRE(move != nullptr);
    move->bindings.PushBack(FreshBinding(move->kind));
    CHECK(HasBinding(map, 0, 0, 0));
    CHECK(move->bindings[0].source == input::BindingSource::GamepadStick); // a 2D axis wants a stick
    CHECK(FreshBinding(input::ActionKind::Button).source == input::BindingSource::Key);
}

TEST_CASE("input editor: cycling the source walks the kind's valid list on a fresh binding")
{
    input::BindingSource valid[8];
    const usize n = input::ValidSources(input::ActionKind::Button, valid);
    REQUIRE(n >= 2u);

    input::Binding binding;
    binding.source = valid[0];
    binding.scale = 7.0f;
    const input::Binding next = CycleSource(input::ActionKind::Button, binding);
    CHECK(next.source == valid[1]);
    CHECK(next.scale == 1.0f); // the source specifics reset

    input::Binding last;
    last.source = valid[n - 1];
    CHECK(CycleSource(input::ActionKind::Button, last).source == valid[0]); // wraps
}

TEST_CASE("input editor: the listen filter follows the action's kind")
{
    const input::CaptureFilter button = FilterFor(input::ActionKind::Button, false);
    CHECK((button.keys && button.mouseButtons && button.gamepadButtons));
    CHECK((!button.gamepadAxes && !button.gamepadSticks));

    const input::CaptureFilter axis = FilterFor(input::ActionKind::Axis1D, false);
    CHECK((axis.keys && axis.gamepadAxes && !axis.gamepadSticks));

    const input::CaptureFilter stick = FilterFor(input::ActionKind::Axis2D, false);
    CHECK((!stick.keys && !stick.mouseButtons && !stick.gamepadButtons && stick.gamepadSticks));

    // One composite direction is always a single key, whatever the kind.
    const input::CaptureFilter direction = FilterFor(input::ActionKind::Axis2D, true);
    CHECK((direction.keys && !direction.mouseButtons && !direction.gamepadButtons));
}

TEST_CASE("input editor: a capture lands in the binding, or in one composite direction")
{
    pipeline::InputMapAsset asset;
    asset.SeedDefaultContent();
    input::InputMap& map = asset.Map();

    input::Action* jump = ActionAt(map, 0, 2);
    REQUIRE(jump != nullptr);
    jump->bindings.PushBack(FreshBinding(input::ActionKind::Button));
    input::Binding captured;
    captured.source = input::BindingSource::MouseButton;
    captured.code = 3;
    ApplyCapture(map, 0, 2, 0, -1, captured);
    CHECK(jump->bindings[0].source == input::BindingSource::MouseButton);
    CHECK(jump->bindings[0].code == 3u);

    input::Action* move = ActionAt(map, 0, 0);
    REQUIRE(move != nullptr);
    input::Binding composite;
    composite.source = input::BindingSource::Composite2D;
    move->bindings.PushBack(composite);
    input::Binding key;
    key.code = 42;
    ApplyCapture(map, 0, 0, 0, 3, key); // +Y
    CHECK(move->bindings[0].source == input::BindingSource::Composite2D);
    CHECK(move->bindings[0].posY == 42u);
    CHECK(move->bindings[0].negX == 0u);

    // Out-of-range targets are ignored.
    ApplyCapture(map, 0, 0, 5, -1, key);
    ApplyCapture(map, 9, 0, 0, -1, key);
    CHECK(move->bindings.Size() == 1u);
}

TEST_CASE("input map rows: an action by its name and a line, a binding by its source, its settings by source")
{
    input::ActionSet set;
    input::Action move;
    move.name = String(u8"Move");
    move.kind = input::ActionKind::Axis2D;
    input::Binding keys;
    keys.source = input::BindingSource::Composite2D;
    keys.negX = 'A';
    move.bindings.PushBack(keys);
    set.actions.PushBack(move);

    gui::PropertyInfo action;
    editor::DescribeInputRow(set, u8"actions.0", action);
    CHECK(action.DisplayName == StringView(u8"Move"));
    CHECK(action.Summary.AsView().StartsWith(KindTitle(input::ActionKind::Axis2D)));

    gui::PropertyInfo binding;
    editor::DescribeInputRow(set, u8"actions.0.bindings.0", binding);
    CHECK(binding.DisplayName == SourceTitle(input::BindingSource::Composite2D));
    CHECK_FALSE(binding.Summary.IsEmpty());
    CHECK_FALSE(binding.Summary.AsView().ContainsIgnoreCase(u8"not taken")); // an Axis2D takes four keys

    // A binding whose source the action's kind does not take says so on its own row.
    input::Action jump;
    jump.name = String(u8"Jump");
    jump.kind = input::ActionKind::Button;
    jump.bindings.PushBack(keys); // four keys on a button
    set.actions.PushBack(jump);
    gui::PropertyInfo wrong;
    editor::DescribeInputRow(set, u8"actions.1.bindings.0", wrong);
    CHECK(wrong.Summary.AsView().ContainsIgnoreCase(u8"(not taken by a "));
    set.actions.RemoveAt(1);

    // A four-key binding: its keys shown and captured, its single input and dead zone not.
    gui::PropertyInfo left, code, deadZone;
    editor::DescribeInputRow(set, u8"actions.0.bindings.0.negX", left);
    editor::DescribeInputRow(set, u8"actions.0.bindings.0.code", code);
    editor::DescribeInputRow(set, u8"actions.0.bindings.0.deadZone", deadZone);
    CHECK(left.Visible);
    CHECK(left.DisplayName == StringView(u8"Left"));
    CHECK(left.Hint == editor::kInputCodeHint);
    CHECK_FALSE(code.Visible);
    CHECK_FALSE(deadZone.Visible);

    // When it fires, in the old page's words: closed, the choice and its time; its seconds only
    // for a hold or a tap, named by which.
    gui::PropertyInfo fires, firesKind, seconds;
    for (i64 v = 0; v < 4; ++v)
        firesKind.Choices.PushBack(gui::PropertyChoice{String(u8"raw"), v});
    editor::DescribeInputRow(set, u8"actions.0.interaction", fires);
    editor::DescribeInputRow(set, u8"actions.0.interaction.kind", firesKind);
    editor::DescribeInputRow(set, u8"actions.0.interaction.seconds", seconds);
    CHECK(fires.DisplayName == StringView(u8"Interaction"));
    CHECK(fires.Summary == StringView(u8"On press"));
    CHECK(firesKind.Choices[0].Name == StringView(u8"On press"));
    CHECK(firesKind.Choices[3].Name == StringView(u8"Double tap"));
    CHECK_FALSE(seconds.Visible);
    set.actions[0].interaction.kind = input::InteractionKind::Hold;
    set.actions[0].interaction.seconds = 0.5f;
    fires = {};
    seconds = {};
    editor::DescribeInputRow(set, u8"actions.0.interaction", fires);
    editor::DescribeInputRow(set, u8"actions.0.interaction.seconds", seconds);
    CHECK(fires.Summary == StringView(u8"Hold for 0.50s"));
    CHECK(seconds.Visible);
    CHECK(seconds.DisplayName == StringView(u8"Hold For"));

    // Processing in the old page's words and bounds.
    gui::PropertyInfo curve, snap;
    editor::DescribeInputRow(set, u8"actions.0.processors.responseExponent", curve);
    editor::DescribeInputRow(set, u8"actions.0.processors.snap", snap);
    CHECK(curve.DisplayName == StringView(u8"Curve"));
    CHECK(curve.Min == doctest::Approx(0.1));
    CHECK(curve.Max == doctest::Approx(5.0));
    CHECK(snap.DisplayName == StringView(u8"Snap on Reverse"));

    // The sources offered: only what the action's kind takes.
    gui::PropertyInfo source;
    for (i64 v = 0; v < 16; ++v)
        source.Choices.PushBack(gui::PropertyChoice{String(u8"raw"), v});
    editor::DescribeInputRow(set, u8"actions.0.bindings.0.source", source);
    input::BindingSource valid[8];
    const usize count = input::ValidSources(input::ActionKind::Axis2D, valid);
    CHECK(source.Choices.Size() == count);
    for (const gui::PropertyChoice& choice : source.Choices)
        CHECK(choice.Name != StringView(u8"raw"));
}

namespace
{
    RefPtr<gui::Node> LoadInputMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    /// A scratch project holding one input map: Gameplay (Move: four keys and a stick; Jump: a
    /// key) and Menu (Back: a key).
    struct PageFixture
    {
        StringView dir = u8"scratch_input_map_page";
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        UniquePtr<editor::InputMapEditorPage> page;

        PageFixture()
        {
            pipeline::RegisterInputMapAsset(); // the instance's type, written and read back
            (void)RemoveDirectoryRecursive(dir);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir);
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance =
                project->SourceDb().RootGroup()->CreateInstance(u8"Controls", pipeline::InputMapAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::InputMapAsset asset;
            input::ActionSet gameplay;
            gameplay.name = String(u8"Gameplay");
            input::Action move;
            move.name = String(u8"Move");
            move.kind = input::ActionKind::Axis2D;
            input::Binding keys;
            keys.source = input::BindingSource::Composite2D;
            move.bindings.PushBack(keys);
            move.bindings.PushBack(FreshBinding(input::ActionKind::Axis2D));
            gameplay.actions.PushBack(move);
            input::Action jump;
            jump.name = String(u8"Jump");
            jump.bindings.PushBack(FreshBinding(input::ActionKind::Button));
            gameplay.actions.PushBack(jump);
            asset.Map().sets.PushBack(gameplay);
            input::ActionSet menu;
            menu.name = String(u8"Menu");
            input::Action back;
            back.name = String(u8"Back");
            back.bindings.PushBack(FreshBinding(input::ActionKind::Button));
            menu.actions.PushBack(back);
            asset.Map().sets.PushBack(menu);
            REQUIRE(instance->WriteObject(asset).IsOk());
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadInputMarkup(path); };
            page = MakeUnique<editor::InputMapEditorPage>(DefaultAllocator(), context, Move(services), *instance);
        }
        ~PageFixture()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir);
        }

        input::InputMap& Map() { return Cast<pipeline::InputMapAsset>(page->Object())->Map(); }

        // What the set's rows list under `parent`, the row named `name` (none when there is none).
        Optional<gui::PropertyInfo> Info(StringView parent, StringView name)
        {
            Array<gui::PropertyInfo> rows;
            page->SetRows().GetProperties(parent, rows);
            for (gui::PropertyInfo& info : rows)
                if (info.Name == name)
                    return Optional<gui::PropertyInfo>(Move(info));
            return {};
        }
        void Edit(StringView path, const gui::Variant& value)
        {
            page->SetRows().BeginEdit(path);
            page->SetRows().CommitEdit(path, gui::PropertyWrite{value});
        }
    };
}

TEST_CASE("input map page: the sets and their actions a tree, the chosen set's rows in the grid")
{
    PageFixture f;
    REQUIRE_FALSE(f.page->LoadFailed());
    CHECK(f.page->Tree() == String(u8"Gameplay\n  Move\n  Jump\nMenu\n  Back"));
    CHECK(f.page->Fact(u8"Holds").AsView().StartsWith(u8"2 sets, 3 actions, ")); // the old page's totals
    CHECK(f.page->ChosenSet() == 0);
    CHECK(f.page->SetRows().GetValue(u8"name", 0).AsString() == StringView(u8"Gameplay"));
    const Optional<gui::PropertyInfo> move = f.Info(u8"actions", u8"0");
    REQUIRE(move.HasValue());
    CHECK(move.Value().DisplayName == StringView(u8"Move"));
    CHECK_FALSE(move.Value().Summary.IsEmpty());
    // An action's rows: its interaction right after its kind.
    Array<gui::PropertyInfo> rows;
    f.page->SetRows().GetProperties(u8"actions.0", rows);
    REQUIRE(rows.Size() == 5u);
    CHECK(rows[1].Name == StringView(u8"kind"));
    CHECK(rows[2].Name == StringView(u8"interaction"));
    CHECK(rows[3].Name == StringView(u8"bindings"));

    // An action chosen in the tree: its set shown, it and its list opened in place.
    f.page->ChooseAction(1, 0);
    CHECK(f.page->ChosenSet() == 1);
    CHECK(f.page->SetRows().GetValue(u8"name", 0).AsString() == StringView(u8"Menu"));
    // The grid's own rows, by its paths (the set's section's: "set/..."): open, the action's
    // bindings listed under it.
    gui::PropertyGrid* grid = f.page->Grid();
    REQUIRE(grid != nullptr);
    CHECK(grid->IsOpen(u8"set/actions"));
    CHECK(grid->IsOpen(u8"set/actions.0"));
    CHECK(grid->FindRow(u8"set/actions.0") >= 0);
    CHECK(grid->FindRow(u8"set/actions.0.bindings") >= 0);

    // The search keeps the actions whose name or input matches, and their sets.
    f.page->Search(u8"jump");
    CHECK(f.page->Tree() == String(u8"Gameplay\n  Jump"));
    f.page->Search(u8"");
    CHECK(f.page->Tree() == String(u8"Gameplay\n  Move\n  Jump\nMenu\n  Back"));
}

TEST_CASE("input map page: a row's edit, an added set and a removed action each one undo step")
{
    PageFixture f;
    f.Edit(u8"actions.1.name", gui::Variant(StringView(u8"Leap")));
    CHECK(f.Map().sets[0].actions[1].name == StringView(u8"Leap"));
    CHECK(f.page->Tree() == String(u8"Gameplay\n  Move\n  Leap\nMenu\n  Back"));
    CHECK(f.page->IsDirty());
    f.page->Commands().Undo();
    CHECK(f.Map().sets[0].actions[1].name == StringView(u8"Jump"));

    f.page->AddSet();
    CHECK(f.Map().sets.Size() == 3u);
    CHECK(f.Map().sets[2].name == StringView(u8"Set 1"));
    CHECK(f.page->ChosenSet() == 2);
    f.page->Commands().Undo();
    CHECK(f.Map().sets.Size() == 2u);
    CHECK(f.page->ChosenSet() == 1); // the set shown gone: the last one

    f.page->ChooseAction(0, 0);
    REQUIRE(f.page->RemoveChosen());
    CHECK(f.Map().sets[0].actions.Size() == 1u);
    CHECK(f.page->Tree() == String(u8"Gameplay\n  Jump\nMenu\n  Back"));
    f.page->Commands().Undo();
    CHECK(f.Map().sets[0].actions.Size() == 2u);
}

TEST_CASE("input map page: a set folded in the tree stays folded when another is chosen")
{
    PageFixture f;
    auto* tree = editor::app::FindById<gui::TreeView>(*f.page->Content(), u8"input-tree");
    REQUIRE(tree != nullptr);
    CHECK(tree->IsIdExpanded(0)); // Gameplay
    tree->SetIdExpanded(0, false);
    f.page->ChooseSet(1);
    CHECK_FALSE(tree->IsIdExpanded(0));
    f.Edit(u8"name", gui::Variant(StringView(u8"Menus"))); // an edit rebuilds the tree
    CHECK_FALSE(tree->IsIdExpanded(0));
}

TEST_CASE("input map page: a set added and undone after a row's edit keeps that edit")
{
    // The whole edit's before is the map as it is, not as the base page's own last step left it.
    PageFixture f;
    f.Edit(u8"actions.1.name", gui::Variant(StringView(u8"Leap")));
    f.page->AddSet();
    REQUIRE(f.Map().sets.Size() == 3u);
    f.page->Commands().Undo();
    CHECK(f.Map().sets.Size() == 2u);
    CHECK(f.Map().sets[0].actions[1].name == StringView(u8"Leap"));
}

TEST_CASE("input map page: a binding's input listened for and landed as one undo step, a four-key one by its key")
{
    PageFixture f;
    const editor::InputCaptureServices& capture = f.page->CaptureServices();
    // The editor asks by the grid's paths. What the input's button says: Jump's key, and a four-key
    // binding's keys one at a time.
    CHECK(capture.describe(u8"set/actions.1.bindings.0.code") == input::DescribeBinding(f.Map().sets[0].actions[1].bindings[0]));
    CHECK_FALSE(capture.describe(u8"set/actions.1.bindings.0.code").IsEmpty());
    f.Map().sets[0].actions[0].bindings[0].posY = 'W';
    CHECK(capture.describe(u8"set/actions.0.bindings.0.posY") == input::KeyName('W'));
    CHECK(capture.describe(u8"set/actions.0.bindings.0.negX") == StringView(u8"Not set"));
    f.Map().sets[0].actions[0].bindings[0].posY = 0;
    // Jump's key.
    capture.toggle(u8"set/actions.1.bindings.0.code");
    CHECK(f.page->IsListening());
    CHECK(capture.listening(u8"set/actions.1.bindings.0.code"));
    CHECK_FALSE(capture.listening(u8"set/actions.0.bindings.0.code"));
    input::Binding captured;
    captured.source = input::BindingSource::MouseButton;
    captured.code = 3;
    f.page->LandCapture(captured);
    CHECK_FALSE(f.page->IsListening());
    CHECK(f.Map().sets[0].actions[1].bindings[0].source == input::BindingSource::MouseButton);
    CHECK(f.Map().sets[0].actions[1].bindings[0].code == 3u);

    // Move's four keys: Up only.
    capture.toggle(u8"set/actions.0.bindings.0.posY");
    input::Binding key;
    key.code = 42;
    f.page->LandCapture(key);
    CHECK(f.Map().sets[0].actions[0].bindings[0].posY == 42u);
    CHECK(f.Map().sets[0].actions[0].bindings[0].source == input::BindingSource::Composite2D);

    // Toggled twice: stops; a path naming no binding: never listens.
    capture.toggle(u8"set/actions.1.bindings.0.code");
    capture.toggle(u8"set/actions.1.bindings.0.code");
    CHECK_FALSE(f.page->IsListening());
    capture.toggle(u8"set/actions.9.bindings.0.code");
    CHECK_FALSE(f.page->IsListening());

    f.page->Commands().Undo();
    f.page->Commands().Undo();
    CHECK(f.Map().sets[0].actions[1].bindings[0].source == input::BindingSource::Key);
    CHECK(f.Map().sets[0].actions[0].bindings[0].posY == 0u);
}

TEST_CASE("input map page: a binding's gamepad is a choice, Any or a pad by its number, as the old page had it")
{
    input::ActionSet set;
    input::Action fire;
    fire.name = String(u8"Fire");
    input::Binding pad;
    pad.source = input::BindingSource::GamepadButton;
    fire.bindings.PushBack(pad);
    set.actions.PushBack(fire);
    gui::PropertyInfo device;
    editor::DescribeInputRow(set, u8"actions.0.bindings.0.device", device);
    CHECK(device.Visible);
    CHECK(device.Kind == gui::PropertyKind::Enum);
    REQUIRE(device.Choices.Size() == 5u);
    CHECK(device.Choices[0].Name == StringView(u8"Any"));
    CHECK(device.Choices[0].Value == -1);
    CHECK(device.Choices[1].Name == StringView(u8"1"));
    CHECK(device.Choices[1].Value == 0);

    // Chosen through the grid's rows: the binding's device its index.
    PageFixture f;
    f.page->ChooseAction(1, 0); // Menu's Back
    f.Map().sets[1].actions[0].bindings[0].source = input::BindingSource::GamepadButton;
    f.Edit(u8"actions.0.bindings.0.device", gui::Variant(i64{2}));
    CHECK(f.Map().sets[1].actions[0].bindings[0].device == 2);
    f.Edit(u8"actions.0.bindings.0.device", gui::Variant(i64{-1}));
    CHECK(f.Map().sets[1].actions[0].bindings[0].device == -1);
}

TEST_CASE("input map page: a map the game cannot read says so, and Save refuses it")
{
    PageFixture f;
    CHECK(f.page->Notice(u8"invalid").IsEmpty());
    // A stick on a button.
    f.page->ChooseAction(0, 1);
    f.Edit(u8"actions.1.kind", gui::Variant(static_cast<i64>(input::ActionKind::Axis2D)));
    CHECK_FALSE(f.page->Notice(u8"invalid").IsEmpty());
    CHECK_FALSE(f.page->Save().IsOk());
    f.page->Commands().Undo();
    CHECK(f.page->Notice(u8"invalid").IsEmpty());
}

// A touch stick's radius is normalised, as the runtime reads it (the touch's offset from its anchor,
// a fraction of the screen, over the radius) and as the touch regions are: its row is bounded and
// said the same way, not in pixels (a typed 100 saturated every touch).
TEST_CASE("input map rows: a touch stick's radius is a fraction of the screen, as its area is")
{
    input::Binding stick;
    stick.source = input::BindingSource::TouchStick;
    gui::PropertyInfo radius;
    editor::input_map_rows::DescribeBindingField(stick, u8"stickRadius", radius);
    gui::PropertyInfo area;
    editor::input_map_rows::DescribeBindingField(stick, u8"regionX", area);
    CHECK(radius.Visible);
    CHECK(radius.HasRange);
    CHECK(radius.Min == area.Min);
    CHECK(radius.Max == area.Max);
    CHECK(radius.Step == area.Step);
    CHECK(radius.Description.AsView().ContainsIgnoreCase(u8"fraction of the screen"));
    CHECK(stick.stickRadius <= radius.Max); // the default sits in the range
}
