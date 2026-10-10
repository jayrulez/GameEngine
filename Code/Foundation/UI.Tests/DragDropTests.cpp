// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The drag-and-drop state machine, driven through the input manager as a real session would
// be: the Potential state that keeps a draggable row clickable, the threshold, activation
// (adorner popup + pointer capture), enter/over/leave bookkeeping across targets, drop and
// completion, cancel by Escape, a rejecting target, a source with no data, and mid-drag
// teardown of the target or the source. Mirrored from the Beef port's suite - Raptor had none.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.ui;

#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    class SourceView final : public TestView, public IDragSource
    {
        RTTI_OBJECT(SourceView, TestView)
    public:
        i32 Started = 0;
        i32 Completed = 0;
        DragDropEffects LastEffect = DragDropEffects::None;
        bool LastCancelled = false;
        bool ProvideData = true;

        SourceView() : TestView(50.0f, 30.0f) {}
        [[nodiscard]] IDragSource* AsDragSource() override { return this; }
        [[nodiscard]] core::RefPtr<DragData> CreateDragData() override
        {
            return ProvideData ? core::MakeRef<DragData>(core::DefaultAllocator(), StringView(u8"text"))
                               : core::RefPtr<DragData>{};
        }
        [[nodiscard]] core::RefPtr<View> CreateDragVisual(DragData*) override
        {
            return core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
        }
        void OnDragStarted(DragData*) override { ++Started; }
        void OnDragCompleted(DragData*, DragDropEffects effect, bool cancelled) override
        {
            ++Completed;
            LastEffect = effect;
            LastCancelled = cancelled;
        }
    };
    RTTI_DEFINE_OBJECT(SourceView, "rtti::ui::tests")

    class TargetView final : public TestView, public IDropTarget
    {
        RTTI_OBJECT(TargetView, TestView)
    public:
        i32 Enter = 0, Over = 0, Leave = 0, Drop = 0;
        DragDropEffects Accept = DragDropEffects::Copy;

        TargetView() : TestView(50.0f, 30.0f) {}
        [[nodiscard]] IDropTarget* AsDropTarget() override { return this; }
        [[nodiscard]] DragDropEffects CanAcceptDrop(DragData*, f32, f32) override { return Accept; }
        void OnDragEnter(DragData*, f32, f32) override { ++Enter; }
        void OnDragOver(DragData*, f32, f32) override { ++Over; }
        void OnDragLeave(DragData*) override { ++Leave; }
        [[nodiscard]] DragDropEffects OnDrop(DragData*, f32, f32) override
        {
            ++Drop;
            return Accept;
        }
    };
    RTTI_DEFINE_OBJECT(TargetView, "rtti::ui::tests")

    /// Source at (0,0) and target at (100,0), both 50x30, in an absolute layout under the root.
    struct Fixture
    {
        UIContext ctx{DefaultAllocator()};
        core::RefPtr<RootView> root = core::MakeRef<RootView>(core::DefaultAllocator());
        core::RefPtr<AbsoluteLayout> layout = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
        core::RefPtr<SourceView> source = core::MakeRef<SourceView>(core::DefaultAllocator());
        core::RefPtr<TargetView> target = core::MakeRef<TargetView>(core::DefaultAllocator());

        Fixture()
        {
            Init(ctx, root.Get(), 400, 300);
            LayoutStyle at;
            at.Left = 0;
            at.Top = 0;
            layout->AddView(source.Get(), at);
            at.Left = 100;
            layout->AddView(target.Get(), at);
            root->AddView(layout.Get());
            LayoutPass(ctx, root.Get());
        }
        InputManager* Input() { return ctx.GetInputManager(); }
        DragDropManager* Drag() { return ctx.DragDrop(); }
        [[nodiscard]] usize Popups() { return root->GetPopupLayer()->PopupCount(); }
        /// Press on the source and move past the threshold: an ACTIVE drag.
        void Activate()
        {
            Input()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
            Input()->ProcessMouseMove(20, 10);
        }
    };
}

TEST_CASE("drag-drop: a press on a source is only POTENTIAL until the pointer passes the threshold")
{
    Fixture f;
    f.Input()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(f.Drag()->IsPotentialDrag());
    CHECK_FALSE(f.Drag()->IsDragging());
    f.Input()->ProcessMouseMove(12, 10); // 2 px: under the 4 px threshold
    CHECK(f.Drag()->IsPotentialDrag());
    CHECK(f.source->Started == 0);
    CHECK(f.Popups() == 0);
    f.Input()->ProcessMouseMove(20, 10); // 10 px: past it
    CHECK(f.Drag()->IsDragging());
    CHECK(f.source->Started == 1);
    CHECK(f.Popups() == 1); // the adorner rides in the popup layer
    CHECK(f.ctx.GetFocusManager()->CapturedView() == f.source.Get());
    CHECK(f.Drag()->CurrentDragData() != nullptr);
    CHECK(f.Drag()->CurrentDragData()->Format() == StringView(u8"text"));
}

TEST_CASE("drag-drop: a press that never moves ends quietly and starts nothing")
{
    Fixture f;
    f.Input()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    f.Input()->ProcessMouseUp(MouseButton::Left, 10, 10);
    CHECK(f.Drag()->State() == DragState::Idle);
    CHECK(f.source->Started == 0);
    CHECK(f.source->Completed == 0);
    CHECK_FALSE(f.ctx.GetFocusManager()->HasCapture());
}

TEST_CASE("drag-drop: enter, over and leave follow the pointer across a target; a drop completes")
{
    Fixture f;
    f.Activate();
    f.Input()->ProcessMouseMove(120, 10); // onto the target
    CHECK(f.target->Enter == 1);
    CHECK(f.target->Over == 0);
    CHECK(f.Drag()->CurrentEffect() == DragDropEffects::Copy);
    f.Input()->ProcessMouseMove(125, 12); // within it
    CHECK(f.target->Over == 1);
    f.Input()->ProcessMouseMove(300, 10); // off it
    CHECK(f.target->Leave == 1);
    CHECK(f.Drag()->CurrentEffect() == DragDropEffects::None);
    f.Input()->ProcessMouseMove(120, 10); // back on
    CHECK(f.target->Enter == 2);
    f.Input()->ProcessMouseUp(MouseButton::Left, 120, 10);
    CHECK(f.target->Drop == 1);
    CHECK(f.target->Leave == 2); // completion leaves the target it dropped on
    CHECK(f.source->Completed == 1);
    CHECK(f.source->LastEffect == DragDropEffects::Copy);
    CHECK_FALSE(f.source->LastCancelled);
    CHECK(f.Drag()->State() == DragState::Idle);
    CHECK(f.Popups() == 0);
    CHECK_FALSE(f.ctx.GetFocusManager()->HasCapture());
    CHECK(f.Drag()->CurrentDragData() == nullptr);
}

TEST_CASE("drag-drop: a rejecting target takes no drop and the source hears a cancel")
{
    Fixture f;
    f.target->Accept = DragDropEffects::None;
    f.Activate();
    f.Input()->ProcessMouseMove(120, 10);
    CHECK(f.target->Enter == 1);
    CHECK(f.Drag()->CurrentEffect() == DragDropEffects::None);
    f.Input()->ProcessMouseUp(MouseButton::Left, 120, 10);
    CHECK(f.target->Drop == 0);
    CHECK(f.source->Completed == 1);
    CHECK(f.source->LastCancelled);
    CHECK(f.Drag()->State() == DragState::Idle);
}

TEST_CASE("drag-drop: Escape cancels an active drag")
{
    Fixture f;
    f.Activate();
    f.Input()->ProcessMouseMove(120, 10);
    f.Input()->ProcessKeyDown(KeyCode::Escape, KeyModifiers::None, false, 0);
    CHECK(f.Drag()->State() == DragState::Idle);
    CHECK(f.source->Completed == 1);
    CHECK(f.source->LastCancelled);
    CHECK(f.target->Leave == 1);
    CHECK(f.Popups() == 0);
    CHECK_FALSE(f.ctx.GetFocusManager()->HasCapture());
}

TEST_CASE("drag-drop: a source with no data never activates")
{
    Fixture f;
    f.source->ProvideData = false;
    f.Activate();
    CHECK(f.Drag()->State() == DragState::Idle);
    CHECK(f.source->Started == 0);
    CHECK(f.Popups() == 0);
}

TEST_CASE("drag-drop: a target leaving mid-drag is told so and the drag carries on; the source leaving cancels")
{
    Fixture f;
    f.Activate();
    f.Input()->ProcessMouseMove(120, 10);
    REQUIRE(f.target->Enter == 1);
    f.layout->RemoveView(f.target.Get()); // the drop target goes away under the pointer
    CHECK(f.target->Leave == 1);
    CHECK(f.Drag()->IsDragging());
    CHECK(f.Drag()->CurrentEffect() == DragDropEffects::None);
    f.layout->RemoveView(f.source.Get()); // then the source itself
    CHECK(f.Drag()->State() == DragState::Idle);
    CHECK(f.source->Completed == 1);
    CHECK(f.source->LastCancelled);
    CHECK(f.Popups() == 0);
}

TEST_CASE("drag-drop: a press on a scrollbar inside a source scrolls, and starts no drag")
{
    // A source holding a scrollbar, as a draggable tree holds its list's overlay bar.
    class Panel final : public AbsoluteLayout, public IDragSource
    {
    public:
        [[nodiscard]] IDragSource* AsDragSource() override { return this; }
        [[nodiscard]] core::RefPtr<DragData> CreateDragData() override
        {
            return core::MakeRef<DragData>(core::DefaultAllocator(), StringView(u8"row"));
        }
        [[nodiscard]] core::RefPtr<View> CreateDragVisual(DragData*) override
        {
            return core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
        }
        void OnDragStarted(DragData*) override {}
        void OnDragCompleted(DragData*, DragDropEffects, bool) override {}
    };

    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = core::MakeRef<RootView>(core::DefaultAllocator());
    core::RefPtr<Panel> panel = core::MakeRef<Panel>(core::DefaultAllocator());
    core::RefPtr<TestView> row = core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 200.0f);
    core::RefPtr<ScrollBar> bar = core::MakeRef<ScrollBar>(core::DefaultAllocator(), false);
    bar->SetMaxValue(400.0f);
    bar->SetViewportSize(200.0f);
    Init(ctx, root.Get(), 400, 300);
    LayoutStyle at;
    at.Left = 0;
    at.Top = 0;
    panel->AddView(row.Get(), at);
    at.Left = 90; // over the row's right edge, as an overlay bar sits
    at.Width = SizeSpec::Fixed(Unit::Dp(10.0f));
    at.Height = SizeSpec::Fixed(Unit::Dp(200.0f));
    panel->AddView(bar.Get(), at);
    root->AddView(panel.Get());
    LayoutPass(ctx, root.Get());
    InputManager* input = ctx.GetInputManager();
    DragDropManager* drag = ctx.DragDrop();

    // On the bar's thumb (its top): the bar takes the gesture and scrolls; nothing drags.
    input->ProcessMouseDown(MouseButton::Left, 95, 20, 0);
    CHECK_FALSE(drag->IsPotentialDrag());
    input->ProcessMouseMove(95, 80);
    CHECK_FALSE(drag->IsDragging());
    CHECK(bar->Value() > 0.0f);
    input->ProcessMouseUp(MouseButton::Left, 95, 80);

    // On the row beside it: the source's drag, as before.
    input->ProcessMouseDown(MouseButton::Left, 40, 20, 0);
    CHECK(drag->IsPotentialDrag());
    input->ProcessMouseMove(40, 60);
    CHECK(drag->IsDragging());
    input->ProcessMouseUp(MouseButton::Left, 40, 60);
}
