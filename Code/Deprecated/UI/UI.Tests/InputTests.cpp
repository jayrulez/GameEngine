// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests Input suites: FocusManagerTests, CapturePhaseTests, ShortcutManagerTests,
// and the control-independent subset of DirectionalFocusTests (MoveFocus + OnCancel). Deferred: the
// OnActivate/WantsArrowKeys cases (need Button/CheckBox/EditText controls) and InputFilterTests (the
// InputFilter class is an Editing/text-field concern, not yet ported). Directional MoveFocus tests use
// a focusable TestView in place of Beef's Button (which is only used there as a default-focusable view).
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
    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }
    core::RefPtr<TestView> Focusable(f32 w = 50, f32 h = 30)
    {
        auto v = core::MakeRef<TestView>(core::DefaultAllocator(), w, h);
        v->IsFocusable = true;
        v->IsTabStop = true;
        return v;
    }

    // View that records which phases it received events in.
    class PhaseTrackingView : public View
    {
        RTTI_OBJECT(PhaseTrackingView, View)
    public:
        f32 DesiredWidth = 50, DesiredHeight = 30;
        bool CaptureReceived = false, TargetReceived = false, BubbleReceived = false,
             BlockInCapture = false;

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Phase == EventPhase::Target)
            {
                TargetReceived = true;
            }
            else if (e.Phase == EventPhase::Bubble)
            {
                BubbleReceived = true;
            }
        }
        void OnMouseDownCapture(MouseEventArgs& e) override
        {
            CaptureReceived = true;
            if (BlockInCapture)
            {
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Phase == EventPhase::Target)
            {
                TargetReceived = true;
            }
            else if (e.Phase == EventPhase::Bubble)
            {
                BubbleReceived = true;
            }
        }
        void OnKeyDownCapture(KeyEventArgs& e) override
        {
            CaptureReceived = true;
            if (BlockInCapture)
            {
                e.Handled = true;
            }
        }

    protected:
        void OnMeasure(BoxConstraints c) override
        {
            MeasuredSize = Float2{c.ConstrainWidth(DesiredWidth), c.ConstrainHeight(DesiredHeight)};
        }
    };
    RTTI_DEFINE_OBJECT(PhaseTrackingView, "rtti::ui::tests")

    // ViewGroup that tracks capture phase.
    class PhaseTrackingGroup : public ViewGroup
    {
        RTTI_OBJECT(PhaseTrackingGroup, ViewGroup)
    public:
        bool CaptureReceived = false, BubbleReceived = false, BlockInCapture = false;

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Phase == EventPhase::Bubble)
            {
                BubbleReceived = true;
            }
        }
        void OnMouseDownCapture(MouseEventArgs& e) override
        {
            CaptureReceived = true;
            if (BlockInCapture)
            {
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Phase == EventPhase::Bubble)
            {
                BubbleReceived = true;
            }
        }
        void OnKeyDownCapture(KeyEventArgs& e) override
        {
            CaptureReceived = true;
            if (BlockInCapture)
            {
                e.Handled = true;
            }
        }

    protected:
        void OnLayout(f32, f32, f32 w, f32 h) override
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* c = GetChildAt(i);
                if (c->Visibility != Visibility::Gone)
                {
                    c->Layout(0, 0, w, h);
                }
            }
        }
    };
    RTTI_DEFINE_OBJECT(PhaseTrackingGroup, "rtti::ui::tests")

    class CancelTrackingGroup : public ViewGroup
    {
        RTTI_OBJECT(CancelTrackingGroup, ViewGroup)
    public:
        core::Function<void()> OnCancelCalled;
        void OnCancel() override
        {
            if (OnCancelCalled)
            {
                OnCancelCalled();
            }
        }

    protected:
        void OnLayout(f32, f32, f32, f32) override {}
    };
    RTTI_DEFINE_OBJECT(CancelTrackingGroup, "rtti::ui::tests")

    // Reproduces the self-destroying-button UAF: on MouseUp it detaches itself from its parent,
    // dropping the tree's last strong ref. Without DispatchMouseUp pinning the target, the
    // dispatcher's post-invoke reads (target->Bounds / target->Parent) would touch freed memory.
    class SelfRemovingView : public View
    {
        RTTI_OBJECT(SelfRemovingView, View)
    public:
        bool* HandlerRan = nullptr;

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Phase != EventPhase::Target)
            {
                return;
            }
            if (HandlerRan != nullptr)
            {
                *HandlerRan = true;
            }
            e.Handled = true;
            if (auto* group = core::Cast<ViewGroup>(Parent))
            {
                group->RemoveView(this, true); // frees `this` if no external strong ref
            }
        }

    protected:
        void OnMeasure(BoxConstraints c) override
        {
            MeasuredSize = Float2{c.ConstrainWidth(50.0f), c.ConstrainHeight(30.0f)};
        }
    };
    RTTI_DEFINE_OBJECT(SelfRemovingView, "rtti::ui::tests")
}

// ============================ FocusManager ============================

TEST_CASE("focus: SetFocus_ViewBecomesFocused")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    CHECK(view->IsFocused());
    CHECK(ctx.GetFocusManager()->FocusedView() == view.Get());
}

TEST_CASE("focus: SetFocus_OldViewLosesFocus")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    ctx.GetFocusManager()->SetFocus(a.Get());
    CHECK(a->IsFocused());
    ctx.GetFocusManager()->SetFocus(b.Get());
    CHECK(!a->IsFocused());
    CHECK(b->IsFocused());
}

TEST_CASE("focus: ClearFocus_NoViewFocused")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    ctx.GetFocusManager()->ClearFocus();
    CHECK(!view->IsFocused());
    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
}

TEST_CASE("focus: SaveRestore_RestoresFocusWithSource")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(view.Get(), FocusSource::Keyboard);
    const FocusManager::SavedFocus saved = fm->SaveAndClearFocus();
    CHECK(fm->FocusedView() == nullptr);
    fm->RestoreFocus(saved);
    CHECK(fm->FocusedView() == view.Get());
    CHECK(fm->Source() == FocusSource::Keyboard); // the ORIGINAL modality comes back
}

TEST_CASE("focus: SaveRestore_EachEntryIndependent_OutOfOrderCloseCannotCrossRestore")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    auto* fm = ctx.GetFocusManager();
    CHECK(fm->FocusStackDepth() == 0u);
    fm->SetFocus(a.Get());
    const FocusManager::SavedFocus first = fm->SaveAndClearFocus(); // popup 1 saved {a}
    CHECK(fm->FocusStackDepth() == 1u);
    CHECK(fm->FocusedView() == nullptr);
    fm->SetFocus(b.Get());
    const FocusManager::SavedFocus second = fm->SaveAndClearFocus(); // popup 2 saved {b}
    CHECK(fm->FocusStackDepth() == 2u);
    // Popup 1 closes FIRST (out of LIFO order): it restores only what IT saved.
    fm->RestoreFocus(first);
    CHECK(fm->FocusStackDepth() == 1u);
    CHECK(fm->FocusedView() == a.Get());
    fm->RestoreFocus(second);
    CHECK(fm->FocusStackDepth() == 0u);
    CHECK(fm->FocusedView() == b.Get());
}

TEST_CASE("focus: SaveRestore_SkipsDeletedView")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    const FocusManager::SavedFocus saved = ctx.GetFocusManager()->SaveAndClearFocus();
    root->RemoveView(view.Get(), true);
    view = nullptr; // drop the test's ref so the view is actually gone
    ctx.GetFocusManager()->RestoreFocus(saved);
    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
}

TEST_CASE("focus: SaveRestore_SkipsViewDisabledWhilePopupWasOpen")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(view.Get());
    const FocusManager::SavedFocus saved = fm->SaveAndClearFocus();
    view->IsEnabled = false; // disabled while the popup was open
    fm->RestoreFocus(saved);
    CHECK(fm->FocusedView() == nullptr); // never strand focus on a disabled view
}

TEST_CASE("focus-visible: pointer focus is HELD but not DRAWN; keyboard focus draws")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();

    fm->SetFocus(view.Get(), FocusSource::Pointer);
    CHECK(view->IsFocused());                // logic keeps working (typing, nav, tab continuity)
    CHECK_FALSE(view->IsFocusVisible());     // but no focus ring
    CHECK((view->GetControlState() & ControlState::Focused) == ControlState::Normal);

    fm->SetFocus(view.Get(), FocusSource::Keyboard); // same view, new modality
    CHECK(view->IsFocusVisible());
    CHECK((view->GetControlState() & ControlState::Focused) == ControlState::Focused);
}

TEST_CASE("focus-visible: modal restore brings a pointer-focused view back RINGLESS")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(view.Get(), FocusSource::Pointer); // clicked before the modal opened
    const FocusManager::SavedFocus saved = fm->SaveAndClearFocus();
    fm->RestoreFocus(saved);
    CHECK(view->IsFocused());            // focus is retained across the modal
    CHECK_FALSE(view->IsFocusVisible()); // the ring does NOT relight - the original complaint
}

TEST_CASE("focus: Capture_SetAndRelease")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetCapture(view.Get());
    CHECK(fm->HasCapture());
    CHECK(fm->CapturedView() == view.Get());
    fm->ReleaseCapture();
    CHECK(!fm->HasCapture());
}

TEST_CASE("focus: OnViewDeleted_ClearsFocusAndCapture")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(view.Get());
    fm->SetCapture(view.Get());
    root->RemoveView(view.Get(), true); // -> Unregister -> OnViewDeleted
    CHECK(fm->FocusedView() == nullptr);
    CHECK(!fm->HasCapture());
}

TEST_CASE("focus: FocusNext_CyclesThroughTabStops")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    auto c = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    root->AddView(c.Get());
    auto* fm = ctx.GetFocusManager();
    fm->FocusNext();
    CHECK(fm->FocusedView() == a.Get());
    fm->FocusNext();
    CHECK(fm->FocusedView() == b.Get());
    fm->FocusNext();
    CHECK(fm->FocusedView() == c.Get());
    fm->FocusNext();
    CHECK(fm->FocusedView() == a.Get()); // wraps
}

TEST_CASE("focus: FocusPrev_CyclesBackward")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(a.Get());
    fm->FocusPrev();
    CHECK(fm->FocusedView() == b.Get());
}

TEST_CASE("focus: FocusNext_SkipsNonTabStop")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    b->IsTabStop = false;
    auto c = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    root->AddView(c.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(a.Get());
    fm->FocusNext();
    CHECK(fm->FocusedView() == c.Get());
}

TEST_CASE("focus: IsFocusWithin_AncestorOfFocused")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = Focusable();
    root->AddView(group.Get());
    group->AddView(child.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    CHECK(group->IsFocusWithin());
    CHECK(root->IsFocusWithin());
    CHECK(child->IsFocusWithin());
}

// ============================ CapturePhase ============================

TEST_CASE("capture: MouseDown_CapturePhase_ParentSeesFirst")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(parent->CaptureReceived);
    CHECK(child->TargetReceived);
    CHECK(parent->BubbleReceived);
}

TEST_CASE("capture: MouseDown_CaptureBlocks_TargetNotReached")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(parent->CaptureReceived);
    CHECK(!child->TargetReceived);
    CHECK(!child->CaptureReceived);
    CHECK(!parent->BubbleReceived);
}

TEST_CASE("capture: MouseDown_PhaseFieldSet")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    root->AddView(child.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(child->TargetReceived);
}

TEST_CASE("dispatch: target that frees itself during MouseUp does not UAF the dispatcher")
{
    // Regression for the Input Map "Listen" crash: a click handler destroyed the clicked button
    // in-line, then FireClick/DispatchMouseUp dereferenced the freed view. DispatchMouseUp now pins
    // the target for the whole capture/target/bubble sequence, so a self-freeing handler is safe.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<SelfRemovingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    bool handlerRan = false;
    child->HandlerRan = &handlerRan;
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    LayoutPass(ctx, root.Get());

    child = nullptr; // parent's m_children now holds the ONLY strong ref to the child

    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, 10, 10);

    // Reaching here without a crash (and with the handler having run) is the assertion: pre-fix this
    // read freed memory in DispatchMouseUp right after target->OnMouseUp returned.
    CHECK(handlerRan);
    CHECK(parent->ChildCount() == 0);
}

TEST_CASE("capture: KeyDown_CapturePhase_Works")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    ctx.GetInputManager()->ProcessKeyDown(KeyCode::A, KeyModifiers::None, false);
    CHECK(parent->CaptureReceived);
    CHECK(child->TargetReceived);
    CHECK(parent->BubbleReceived);
}

TEST_CASE("capture: KeyDown_CaptureBlocks")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    ctx.GetInputManager()->ProcessKeyDown(KeyCode::A, KeyModifiers::None, false);
    CHECK(parent->CaptureReceived);
    CHECK(!child->TargetReceived);
    CHECK(!parent->BubbleReceived);
}

TEST_CASE("capture: DeepHierarchy_CaptureOrder")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto grandparent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    grandparent->AddView(parent.Get());
    parent->AddView(child.Get());
    root->AddView(grandparent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(grandparent->CaptureReceived);
    CHECK(parent->CaptureReceived);
    CHECK(child->TargetReceived);
    CHECK(parent->BubbleReceived);
    CHECK(grandparent->BubbleReceived);
}

TEST_CASE("capture: DeepHierarchy_MidCapture_Blocks")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto grandparent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    auto parent = core::MakeRef<PhaseTrackingGroup>(core::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = core::MakeRef<PhaseTrackingView>(core::DefaultAllocator());
    child->IsFocusable = true;
    grandparent->AddView(parent.Get());
    parent->AddView(child.Get());
    root->AddView(grandparent.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(grandparent->CaptureReceived);
    CHECK(parent->CaptureReceived);
    CHECK(!child->TargetReceived);
    CHECK(!grandparent->BubbleReceived);
    CHECK(!parent->BubbleReceived);
}

// ============================ ShortcutManager ============================

TEST_CASE("shortcut: Global_Fires")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    const bool result = ctx.GetShortcuts()->TryDispatch(KeyCode::S, KeyModifiers::LeftCtrl);
    CHECK(fired);
    CHECK(result);
}

TEST_CASE("shortcut: Global_WrongKey_DoesNotFire")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::D, KeyModifiers::LeftCtrl));
    CHECK(!fired);
}

TEST_CASE("shortcut: Global_WrongModifiers_DoesNotFire")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::S, KeyModifiers::None));
    CHECK(!fired);
}

TEST_CASE("shortcut: Scoped_FiresWhenInScope")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = Focusable();
    root->AddView(panel.Get());
    panel->AddView(child.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddScoped(
        KeyCode::Delete, KeyModifiers::None, [&fired]() { fired = true; }, panel.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    CHECK(ctx.GetShortcuts()->TryDispatch(KeyCode::Delete, KeyModifiers::None));
    CHECK(fired);
}

TEST_CASE("shortcut: Scoped_DoesNotFireWhenOutOfScope")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panelA = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto panelB = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = Focusable();
    root->AddView(panelA.Get());
    root->AddView(panelB.Get());
    panelB->AddView(child.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddScoped(
        KeyCode::Delete, KeyModifiers::None, [&fired]() { fired = true; }, panelA.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::Delete, KeyModifiers::None));
    CHECK(!fired);
}

TEST_CASE("shortcut: Remove_StopsShortcut")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    Shortcut* s =
        ctx.GetShortcuts()->AddGlobal(KeyCode::Z, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    ctx.GetShortcuts()->Remove(s);
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::Z, KeyModifiers::LeftCtrl));
    CHECK(!fired);
}

TEST_CASE("shortcut: ScopedRemoved_OnViewDelete")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = core::MakeRef<TestGroup>(core::DefaultAllocator());
    root->AddView(panel.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddScoped(
        KeyCode::F2, KeyModifiers::None, [&fired]() { fired = true; }, panel.Get());
    root->RemoveView(panel.Get(), true);
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::F2, KeyModifiers::None));
    CHECK(!fired);
}

TEST_CASE("shortcut: Scoped_PriorityOverGlobal")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = Focusable();
    root->AddView(panel.Get());
    panel->AddView(child.Get());
    bool globalFired = false, scopedFired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl,
                                  [&globalFired]() { globalFired = true; });
    ctx.GetShortcuts()->AddScoped(
        KeyCode::S, KeyModifiers::Ctrl, [&scopedFired]() { scopedFired = true; }, panel.Get());
    ctx.GetFocusManager()->SetFocus(child.Get());
    ctx.GetShortcuts()->TryDispatch(KeyCode::S, KeyModifiers::LeftCtrl);
    CHECK(scopedFired);
    CHECK(!globalFired);
}

// ============================ Directional focus (MoveFocus) ============================

TEST_CASE("directional: MoveFocus_Down")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto top = Focusable(100, 30);
    auto bottom = Focusable(100, 30);
    root->AddView(top.Get());
    root->AddView(bottom.Get());
    top->Layout(100, 50, 100, 30);
    bottom->Layout(100, 150, 100, 30);
    ctx.GetFocusManager()->SetFocus(top.Get());
    CHECK(ctx.GetFocusManager()->MoveFocus(FocusDirection::Down));
    CHECK(ctx.GetFocusManager()->FocusedView() == bottom.Get());
}

TEST_CASE("directional: MoveFocus_Up")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto top = Focusable(100, 30);
    auto bottom = Focusable(100, 30);
    root->AddView(top.Get());
    root->AddView(bottom.Get());
    top->Layout(100, 50, 100, 30);
    bottom->Layout(100, 150, 100, 30);
    ctx.GetFocusManager()->SetFocus(bottom.Get());
    CHECK(ctx.GetFocusManager()->MoveFocus(FocusDirection::Up));
    CHECK(ctx.GetFocusManager()->FocusedView() == top.Get());
}

TEST_CASE("directional: MoveFocus_LeftRight")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto left = Focusable(100, 30);
    auto right = Focusable(100, 30);
    root->AddView(left.Get());
    root->AddView(right.Get());
    left->Layout(50, 100, 100, 30);
    right->Layout(250, 100, 100, 30);
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(left.Get());
    CHECK(fm->MoveFocus(FocusDirection::Right));
    CHECK(fm->FocusedView() == right.Get());
    CHECK(fm->MoveFocus(FocusDirection::Left));
    CHECK(fm->FocusedView() == left.Get());
}

TEST_CASE("directional: MoveFocus_NoCandidate")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto only = Focusable(100, 30);
    root->AddView(only.Get());
    only->Layout(100, 100, 100, 30);
    ctx.GetFocusManager()->SetFocus(only.Get());
    CHECK(!ctx.GetFocusManager()->MoveFocus(FocusDirection::Down));
    CHECK(ctx.GetFocusManager()->FocusedView() == only.Get());
}

TEST_CASE("directional: MoveFocus_PrefersClosest")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto top = Focusable(100, 30);
    auto nearBottom = Focusable(100, 30);
    auto farBottom = Focusable(100, 30);
    root->AddView(top.Get());
    root->AddView(nearBottom.Get());
    root->AddView(farBottom.Get());
    top->Layout(100, 50, 100, 30);
    nearBottom->Layout(100, 120, 100, 30);
    farBottom->Layout(100, 300, 100, 30);
    ctx.GetFocusManager()->SetFocus(top.Get());
    ctx.GetFocusManager()->MoveFocus(FocusDirection::Down);
    CHECK(ctx.GetFocusManager()->FocusedView() == nearBottom.Get());
}

TEST_CASE("directional: MoveFocus_ExplicitOverride")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable(100, 30);
    auto b = Focusable(100, 30);
    auto c = Focusable(100, 30);
    root->AddView(a.Get());
    root->AddView(b.Get());
    root->AddView(c.Get());
    a->Layout(100, 50, 100, 30);
    b->Layout(100, 150, 100, 30);
    c->Layout(300, 300, 100, 30);
    a->NextFocusDown = c->Id;
    ctx.GetFocusManager()->SetFocus(a.Get());
    ctx.GetFocusManager()->MoveFocus(FocusDirection::Down);
    CHECK(ctx.GetFocusManager()->FocusedView() == c.Get());
}

TEST_CASE("directional: MoveFocus_NoFocused_ReturnsFalse")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->AddView(Focusable().Get());
    CHECK(!ctx.GetFocusManager()->MoveFocus(FocusDirection::Down));
}

TEST_CASE("directional: OnCancel_BubblesToParent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool parentCancelCalled = false;
    auto parent = core::MakeRef<CancelTrackingGroup>(core::DefaultAllocator());
    parent->OnCancelCalled = [&parentCancelCalled]() { parentCancelCalled = true; };
    auto child = Focusable();
    parent->AddView(child.Get());
    root->AddView(parent.Get());
    child->OnCancel();
    CHECK(parentCancelCalled);
}

// Return dispatch order (deliberate deviation from Sedulous): the focused view gets Return in
// OnKeyDown FIRST; only an UNHANDLED Return activates (OnActivate). Text controls can consume
// Enter (commit-on-Enter, multiline newlines) while buttons keep Enter-to-activate.
TEST_CASE("keys: Return_DispatchesBeforeActivation")
{
    class ReturnProbe final : public View
    {
    public:
        bool consumeReturn = false;
        i32 keyDowns = 0;
        i32 activations = 0;
        ReturnProbe() { IsFocusable = true; }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Key == KeyCode::Return)
            {
                ++keyDowns;
                e.Handled = consumeReturn;
            }
        }
        void OnActivate() override { ++activations; }
    };

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    ctx.AddRootView(root.Get());
    auto probe = core::MakeRef<ReturnProbe>(core::DefaultAllocator());
    root->AddView(probe.Get());
    ctx.GetFocusManager()->SetFocus(probe.Get());

    // Unhandled Return: view saw the key, then activation fired (button behavior preserved).
    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::Return, KeyModifiers::None, false));
    CHECK(probe->keyDowns == 1);
    CHECK(probe->activations == 1);

    // Handled Return: NO activation (text-control behavior - commit consumed the key).
    probe->consumeReturn = true;
    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::Return, KeyModifiers::None, false));
    CHECK(probe->keyDowns == 2);
    CHECK(probe->activations == 1);
}

TEST_CASE("keys: Tab_DispatchesToWantsTabKeyViews")
{
    class TabProbe final : public View
    {
    public:
        bool consumeTab = false;
        i32 tabDowns = 0;
        TabProbe()
        {
            IsFocusable = true;
            IsTabStop = true;
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Key == KeyCode::Tab)
            {
                ++tabDowns;
                e.Handled = consumeTab;
            }
        }
    };

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    ctx.AddRootView(root.Get());
    auto editor = core::MakeRef<TabProbe>(core::DefaultAllocator());
    auto next = core::MakeRef<TabProbe>(core::DefaultAllocator());
    root->AddView(editor.Get());
    root->AddView(next.Get());
    ctx.GetFocusManager()->SetFocus(editor.Get());

    // Default (WantsTabKey false): Tab never reaches the view; focus traverses.
    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::Tab, KeyModifiers::None, false));
    CHECK(editor->tabDowns == 0);
    CHECK(ctx.GetFocusManager()->FocusedView() == next.Get());

    // Opted in + handled: the view consumes Tab and keeps focus (indent behavior).
    ctx.GetFocusManager()->SetFocus(editor.Get());
    editor->WantsTabKey = true;
    editor->consumeTab = true;
    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::Tab, KeyModifiers::None, false));
    CHECK(editor->tabDowns == 1);
    CHECK(ctx.GetFocusManager()->FocusedView() == editor.Get());

    // Opted in but UNHANDLED: traversal is still the fallback.
    editor->consumeTab = false;
    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::Tab, KeyModifiers::None, false));
    CHECK(editor->tabDowns == 2);
    CHECK(ctx.GetFocusManager()->FocusedView() == next.Get());
}

TEST_CASE("keycode: every function key F1 to F24 has a display name, distinct, and Unknown has none")
{
    // The enum declares the function keys in two contiguous runs (F1..F12, F13..F24); the name
    // table must keep up with both (F17 to F24 once returned the empty string, so a chord on
    // them showed no key).
    const struct
    {
        KeyCode first;
        KeyCode last;
    } runs[] = {{KeyCode::F1, KeyCode::F12}, {KeyCode::F13, KeyCode::F24}};
    Array<StringView> seen;
    for (const auto& run : runs)
    {
        const auto first = static_cast<u32>(run.first);
        const auto last = static_cast<u32>(run.last);
        REQUIRE(last - first == 11u);
        for (u32 v = first; v <= last; ++v)
        {
            const StringView name{KeyCodeName(static_cast<KeyCode>(v))};
            CHECK(name.StartsWith(u8"F"));
            CHECK(name.Size() >= 2u);
            for (const StringView& other : seen)
            {
                CHECK(name != other);
            }
            seen.PushBack(name);
        }
    }
    CHECK(seen.Size() == 24u);
    CHECK(StringView{KeyCodeName(KeyCode::F24)} == u8"F24");
    CHECK(StringView{KeyCodeName(KeyCode::Unknown)}.IsEmpty());
}
