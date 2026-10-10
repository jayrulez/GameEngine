// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The input map editor's registration and its headless edits: what the rows' clicks and the
// listen capture do to the map, without a page. Ported back from the Beef port's
// Editor.Input.Tests (2026-09-20).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import foundation.input;
import input.pipeline;
import foundation.content;
import foundation.ui;
import editor.core;
import editor.input;

using namespace foundation::core;
namespace input = foundation::input;
namespace runtime = foundation::runtime;
using namespace editor::input_map_edit;

namespace
{
    // Headless host stub: the registration only stores it.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::Context& Ctx() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow*
        OpenWindow(const foundation::shell::WindowSettings&,
                   const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::Context m_context{DefaultAllocator()};
    };
}

TEST_CASE("input editor: registering routes the input map asset to its factory")
{
    editor::EditorContext context{DefaultAllocator()};
    StubHost host;
    editor::RegisterInputEditor(context, host);
    editor::IEditorPageFactory* found =
        context.Pages().FindFactory(pipeline::InputMapAsset::StaticType());
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

namespace
{
    template <typename T>
    void CollectViews(foundation::ui::View& view, Array<T*>& out)
    {
        if (T* typed = Cast<T>(&view))
        {
            out.PushBack(typed);
        }
        if (auto* group = Cast<foundation::ui::ViewGroup>(&view))
        {
            for (usize i = 0; i < group->VisualChildCount(); ++i)
            {
                if (foundation::ui::View* child = group->GetVisualChild(i))
                {
                    CollectViews(*child, out);
                }
            }
        }
    }

    /// A scratch project holding one input map: Gameplay (Move: four keys and a stick; Jump: a
    /// key) and Menu (Back: a key).
    struct PageFixture
    {
        StringView dir = u8"scratch_input_map_page";
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        StubHost host;
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
            page = UniquePtr<editor::InputMapEditorPage>(
                DefaultAllocator().New<editor::InputMapEditorPage>(context, host, *instance), DefaultAllocator());
        }
        ~PageFixture()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir);
        }

        template <typename T>
        usize Count()
        {
            Array<T*> found;
            CollectViews(page->Body(), found);
            return found.Size();
        }
    };
}

TEST_CASE("input map page: the outline, a collapsible section per set with its actions' cards")
{
    PageFixture f;
    CHECK(f.page->CurrentLayout() == editor::InputMapEditorPage::Layout::Outline);
    CHECK(f.Count<foundation::ui::Expander>() == 2u); // Gameplay, Menu
    // Key caps: Move's four keys and its stick, Jump's key, Back's key.
    const usize capsOpen = f.Count<foundation::ui::Button>();

    // A collapsed set stays collapsed across a rebuild (by name), its body hidden.
    f.page->SetSetCollapsed(0, true);
    CHECK(f.page->IsSetCollapsed(0));
    f.page->RebuildNow();
    Array<foundation::ui::Expander*> sections;
    CollectViews(f.page->Body(), sections);
    REQUIRE(sections.Size() == 2u);
    CHECK_FALSE(sections[0]->IsExpanded());
    CHECK(sections[1]->IsExpanded());
    // Its title is the set's name, editable in place.
    auto* title = Cast<foundation::ui::EditableLabel>(sections[0]->HeaderTitle());
    REQUIRE(title != nullptr);
    CHECK(title->Text() == StringView(u8"Gameplay"));
    f.page->SetSetCollapsed(0, false);
    f.page->RebuildNow();
    CHECK(f.Count<foundation::ui::Button>() == capsOpen);
}

TEST_CASE("input map page: two panes, the list and the selected set or action")
{
    PageFixture f;
    f.page->SetLayout(editor::InputMapEditorPage::Layout::TwoPanes);
    f.page->RebuildNow();
    CHECK(f.Count<foundation::ui::Expander>() == 0u);

    // The selected action's bindings: Move's composite (four caps) and stick (one).
    f.page->Select(0, 0);
    f.page->RebuildNow();
    Array<foundation::ui::Label*> labels;
    CollectViews(f.page->Body(), labels);
    bool fourKeys = false;
    bool stick = false;
    for (foundation::ui::Label* label : labels)
    {
        fourKeys = fourKeys || label->Text.Value() == StringView(u8"Four keys");
        stick = stick || label->Text.Value() == StringView(u8"Gamepad stick");
    }
    CHECK(fourKeys);
    CHECK(stick);

    // A set selected: its name and priority.
    f.page->Select(1, -1);
    f.page->RebuildNow();
    CHECK(f.page->SelectedSet() == 1u);
    CHECK(f.page->SelectedAction() < 0);

    // Removing what is selected keeps the selection on something that exists.
    f.page->Select(1, 0);
    f.page->RebuildNow();
    CHECK(f.page->SelectedAction() == 0);
}

TEST_CASE("input map page: add a binding by source, a field's edits merge into one undo")
{
    PageFixture f;
    f.page->AddBinding(0, 1, input::BindingSource::GamepadButton);
    REQUIRE(f.page->Map().sets[0].actions[1].bindings.Size() == 2u);
    CHECK(f.page->Map().sets[0].actions[1].bindings[1].source == input::BindingSource::GamepadButton);
    f.page->Commands().Undo();
    CHECK(f.page->Map().sets[0].actions[1].bindings.Size() == 1u);

    // Dragging a dead zone: many edits, one step to undo.
    f.page->RebuildNow();
    Array<foundation::ui::NumericField*> numbers;
    CollectViews(f.page->Body(), numbers);
    foundation::ui::NumericField* deadZone = nullptr;
    for (foundation::ui::NumericField* number : numbers)
    {
        deadZone = number->TooltipText.AsView().StartsWith(u8"Movement smaller") ? number : deadZone;
    }
    REQUIRE(deadZone != nullptr); // Move's stick
    const f32 before = f.page->Map().sets[0].actions[0].bindings[1].deadZone;
    deadZone->SetValue(0.2);
    deadZone->SetValue(0.3);
    deadZone->SetValue(0.4);
    CHECK(f.page->Map().sets[0].actions[0].bindings[1].deadZone == doctest::Approx(0.4f));
    f.page->Commands().Undo();
    CHECK(f.page->Map().sets[0].actions[0].bindings[1].deadZone == doctest::Approx(before));

    // Listening: the key cap reads as waiting; the same cap again cancels.
    f.page->BeginListen(0, 1, 0);
    CHECK(f.page->IsListening());
    f.page->BeginListen(0, 1, 0);
    CHECK_FALSE(f.page->IsListening());
}
