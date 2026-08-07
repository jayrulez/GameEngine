// Ported from Sedulous.UI.Tests Input suites: FocusManagerTests, CapturePhaseTests, ShortcutManagerTests,
// and the control-independent subset of DirectionalFocusTests (MoveFocus + OnCancel). Deferred: the
// OnActivate/WantsArrowKeys cases (need Button/CheckBox/EditText controls) and InputFilterTests (the
// InputFilter class is an Editing/text-field concern, not yet ported). Directional MoveFocus tests use
// a focusable TestView in place of Beef's Button (which is only used there as a default-focusable view).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    foundation::RefPtr<RootView> MakeRoot() { return foundation::MakeRef<RootView>(foundation::DefaultAllocator()); }
    foundation::RefPtr<TestView> Focusable(f32 w = 50, f32 h = 30)
    {
        auto v = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), w, h);
        v->IsFocusable = true;
        v->IsTabStop = true;
        return v;
    }

    // View that records which phases it received events in.
    class PhaseTrackingView : public View
    {
        DRACONIC_OBJECT(PhaseTrackingView, View)
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
    DRACONIC_DEFINE_OBJECT(PhaseTrackingView, "draconic::ui::tests")

    // ViewGroup that tracks capture phase.
    class PhaseTrackingGroup : public ViewGroup
    {
        DRACONIC_OBJECT(PhaseTrackingGroup, ViewGroup)
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
    DRACONIC_DEFINE_OBJECT(PhaseTrackingGroup, "draconic::ui::tests")

    class CancelTrackingGroup : public ViewGroup
    {
        DRACONIC_OBJECT(CancelTrackingGroup, ViewGroup)
    public:
        foundation::Function<void()> OnCancelCalled;
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
    DRACONIC_DEFINE_OBJECT(CancelTrackingGroup, "draconic::ui::tests")
}

// ============================ FocusManager ============================

TEST_CASE("focus: SetFocus_ViewBecomesFocused")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    ctx.GetFocusManager()->ClearFocus();
    CHECK(!view->IsFocused());
    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
}

TEST_CASE("focus: PushPop_RestoresFocus")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    ctx.GetFocusManager()->PushFocus();
    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
    ctx.GetFocusManager()->PopFocus();
    CHECK(ctx.GetFocusManager()->FocusedView() == view.Get());
}

TEST_CASE("focus: PushPop_StackDepth")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto a = Focusable();
    auto b = Focusable();
    root->AddView(a.Get());
    root->AddView(b.Get());
    auto* fm = ctx.GetFocusManager();
    CHECK(fm->FocusStackDepth() == 0u);
    fm->SetFocus(a.Get());
    fm->PushFocus();
    CHECK(fm->FocusStackDepth() == 1u);
    CHECK(fm->FocusedView() == nullptr);
    fm->SetFocus(b.Get());
    fm->PushFocus();
    CHECK(fm->FocusStackDepth() == 2u);
    fm->PopFocus();
    CHECK(fm->FocusStackDepth() == 1u);
    CHECK(fm->FocusedView() == b.Get());
    fm->PopFocus();
    CHECK(fm->FocusStackDepth() == 0u);
    CHECK(fm->FocusedView() == a.Get());
}

TEST_CASE("focus: PushPop_SkipsDeletedView")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto view = Focusable();
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());
    ctx.GetFocusManager()->PushFocus();
    root->RemoveView(view.Get(), true);
    view = nullptr; // drop the test's ref so the view is actually gone
    ctx.GetFocusManager()->PopFocus();
    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
}

TEST_CASE("focus: Capture_SetAndRelease")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto group = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
    child->IsFocusable = true;
    root->AddView(child.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 10, 10, 0);
    CHECK(child->TargetReceived);
}

TEST_CASE("capture: KeyDown_CapturePhase_Works")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto grandparent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto grandparent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    auto parent = foundation::MakeRef<PhaseTrackingGroup>(foundation::DefaultAllocator());
    parent->BlockInCapture = true;
    auto child = foundation::MakeRef<PhaseTrackingView>(foundation::DefaultAllocator());
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::D, KeyModifiers::LeftCtrl));
    CHECK(!fired);
}

TEST_CASE("shortcut: Global_WrongModifiers_DoesNotFire")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool fired = false;
    ctx.GetShortcuts()->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [&fired]() { fired = true; });
    CHECK(!ctx.GetShortcuts()->TryDispatch(KeyCode::S, KeyModifiers::None));
    CHECK(!fired);
}

TEST_CASE("shortcut: Scoped_FiresWhenInScope")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panelA = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
    auto panelB = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto panel = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->AddView(Focusable().Get());
    CHECK(!ctx.GetFocusManager()->MoveFocus(FocusDirection::Down));
}

TEST_CASE("directional: OnCancel_BubblesToParent")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    bool parentCancelCalled = false;
    auto parent = foundation::MakeRef<CancelTrackingGroup>(foundation::DefaultAllocator());
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

    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    ctx.AddRootView(root.Get());
    auto probe = foundation::MakeRef<ReturnProbe>(foundation::DefaultAllocator());
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

    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    ctx.AddRootView(root.Get());
    auto editor = foundation::MakeRef<TabProbe>(foundation::DefaultAllocator());
    auto next = foundation::MakeRef<TabProbe>(foundation::DefaultAllocator());
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
