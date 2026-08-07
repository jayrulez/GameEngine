// Draconic UI - module implementation unit for the Input managers.
//
// Holds the View-touching bodies of FocusManager / InputManager / ShortcutManager and View's
// manager-querying methods (IsHovered/IsFocused/IsFocusWithin). These call across the whole View
// cluster, so - like UIClusterImpl.cpp - they live in an implementation unit (outside the interface
// partition dependency graph) rather than in the manager partitions (which only forward-declare the
// view types). All subsystem hooks are now wired: DragDrop (UpdateDrag/BeginPotentialDrag/EndDrag/
// Escape-cancel), Tooltip (OnHoverChanged/OnMouseDown), PopupLayer (HandleClickOutside), Shortcuts,
// and accelerator search.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.ui;

using namespace draconic::foundation;

namespace draconic::ui
{
    // ============================ View manager queries ============================

    bool View::IsHovered() const
    {
        return Context != nullptr && Context->GetInputManager()->HoveredId() == Id;
    }

    bool View::IsFocused() const
    {
        return Context != nullptr && Context->GetFocusManager()->FocusedId() == Id;
    }

    bool View::IsFocusWithin() const
    {
        if (Context == nullptr)
        {
            return false;
        }
        View* f = Context->GetFocusManager()->FocusedView();
        while (f != nullptr)
        {
            if (f->Id == Id)
            {
                return true;
            }
            f = f->Parent;
        }
        return false;
    }

    bool UIContext::WantsTextInput() const
    {
        const View* focused = m_focusManager.FocusedView();
        return focused != nullptr && focused->WantsTextInput();
    }

    // ============================ FocusManager ============================

    View* FocusManager::FocusedView() const { return m_context->GetViewById(m_focusedId); }

    void FocusManager::SetFocus(View* view)
    {
        if (view == nullptr)
        {
            ClearFocus();
            return;
        }
        if (view->Id == m_focusedId)
        {
            return;
        }
        View* oldFocused = FocusedView();
        if (oldFocused != nullptr)
        {
            oldFocused->OnFocusLost();
        }
        m_focusedId = view->Id;
        view->OnFocusGained();
    }

    void FocusManager::ClearFocus()
    {
        View* oldFocused = FocusedView();
        if (oldFocused != nullptr)
        {
            oldFocused->OnFocusLost();
        }
        m_focusedId = ViewId::Invalid;
    }

    void FocusManager::PushFocus()
    {
        m_focusStack.PushBack(m_focusedId);
        ClearFocus();
    }

    void FocusManager::PopFocus()
    {
        while (m_focusStack.Size() > 0)
        {
            const ViewId savedId = m_focusStack.Back();
            m_focusStack.PopBack();
            if (!savedId.IsValid())
            {
                continue;
            }
            if (View* view = m_context->GetViewById(savedId))
            {
                SetFocus(view);
                return;
            }
        }
    }

    View* FocusManager::CapturedView() const
    {
        return m_capturedId.IsValid() ? m_context->GetViewById(m_capturedId) : nullptr;
    }
    bool FocusManager::HasCapture() const
    {
        return m_capturedId.IsValid() && CapturedView() != nullptr;
    }
    void FocusManager::SetCapture(View* view)
    {
        m_capturedId = (view != nullptr) ? view->Id : ViewId::Invalid;
    }

    void FocusManager::FocusNext()
    {
        Array<View*> focusables;
        CollectFocusable(GetFocusRoot(), focusables);
        if (focusables.Size() == 0)
        {
            return;
        }
        SortByTabIndex(focusables);
        const isize currentIdx = FindCurrentIndex(focusables);
        const usize nextIdx =
            static_cast<usize>((currentIdx + 1) % static_cast<isize>(focusables.Size()));
        SetFocus(focusables[nextIdx]);
    }

    void FocusManager::FocusPrev()
    {
        Array<View*> focusables;
        CollectFocusable(GetFocusRoot(), focusables);
        if (focusables.Size() == 0)
        {
            return;
        }
        SortByTabIndex(focusables);
        const isize count = static_cast<isize>(focusables.Size());
        const isize currentIdx = FindCurrentIndex(focusables);
        const usize prevIdx = static_cast<usize>((currentIdx - 1 + count) % count);
        SetFocus(focusables[prevIdx]);
    }

    bool FocusManager::MoveFocus(FocusDirection direction)
    {
        View* focused = FocusedView();
        if (focused == nullptr)
        {
            return false;
        }

        // Explicit override first.
        Optional<ViewId> explicitId;
        switch (direction)
        {
        case FocusDirection::Up:
            explicitId = focused->NextFocusUp;
            break;
        case FocusDirection::Down:
            explicitId = focused->NextFocusDown;
            break;
        case FocusDirection::Left:
            explicitId = focused->NextFocusLeft;
            break;
        case FocusDirection::Right:
            explicitId = focused->NextFocusRight;
            break;
        }
        if (explicitId.HasValue() && explicitId.Value().IsValid())
        {
            View* target = m_context->GetViewById(explicitId.Value());
            if (target != nullptr && target->IsFocusable && target->IsEffectivelyEnabled())
            {
                SetFocus(target);
                return true;
            }
        }

        Array<View*> focusables;
        CollectFocusable(GetFocusRoot(), focusables);
        if (focusables.Size() <= 1)
        {
            return false;
        }

        // Descend into a focused container.
        if (ViewGroup* group = Cast<ViewGroup>(focused))
        {
            Array<View*> childFocusables;
            CollectFocusable(group, childFocusables);
            if (childFocusables.Size() > 0)
            {
                SortByTabIndex(childFocusables);
                switch (direction)
                {
                case FocusDirection::Down:
                case FocusDirection::Right:
                    SetFocus(childFocusables[0]);
                    return true;
                case FocusDirection::Up:
                case FocusDirection::Left:
                    SetFocus(childFocusables[childFocusables.Size() - 1]);
                    return true;
                }
            }
        }

        const Float2 focusedScreen = focused->LocalToScreen(Float2{0, 0});
        const f32 focusedCX = focusedScreen.x + focused->Width() * 0.5f;
        const f32 focusedCY = focusedScreen.y + focused->Height() * 0.5f;

        View* bestCandidate = nullptr;
        f32 bestScore = kFloatMax;

        for (View* candidate : focusables)
        {
            if (candidate->Id == focused->Id)
            {
                continue;
            }
            if (IsDescendantOf(candidate, focused))
            {
                continue;
            }

            const Float2 candidateScreen = candidate->LocalToScreen(Float2{0, 0});
            const f32 candidateCX = candidateScreen.x + candidate->Width() * 0.5f;
            const f32 candidateCY = candidateScreen.y + candidate->Height() * 0.5f;
            const f32 dx = candidateCX - focusedCX;
            const f32 dy = candidateCY - focusedCY;

            bool inDirection = false;
            f32 axialDist = 0, perpDist = 0;
            switch (direction)
            {
            case FocusDirection::Up:
                inDirection = dy < 0;
                axialDist = Abs(dy);
                perpDist = Abs(dx);
                break;
            case FocusDirection::Down:
                inDirection = dy > 0;
                axialDist = Abs(dy);
                perpDist = Abs(dx);
                break;
            case FocusDirection::Left:
                inDirection = dx < 0;
                axialDist = Abs(dx);
                perpDist = Abs(dy);
                break;
            case FocusDirection::Right:
                inDirection = dx > 0;
                axialDist = Abs(dx);
                perpDist = Abs(dy);
                break;
            }
            if (!inDirection)
            {
                continue;
            }

            const f32 score = axialDist + perpDist * 2.0f;
            if (score < bestScore)
            {
                bestScore = score;
                bestCandidate = candidate;
            }
        }

        if (bestCandidate != nullptr)
        {
            SetFocus(bestCandidate);
            return true;
        }
        return false;
    }

    bool FocusManager::IsDescendantOf(View* view, View* ancestor)
    {
        View* v = view->Parent;
        while (v != nullptr)
        {
            if (v == ancestor)
            {
                return true;
            }
            v = v->Parent;
        }
        return false;
    }

    void FocusManager::OnViewDeleted(View* view)
    {
        if (m_focusedId == view->Id)
        {
            m_focusedId = ViewId::Invalid;
        }
        if (m_capturedId == view->Id)
        {
            m_capturedId = ViewId::Invalid;
        }
    }

    void FocusManager::CollectFocusable(View* view, Array<View*>& output) const
    {
        if (view == nullptr || view->Visibility == Visibility::Gone ||
            !view->IsEffectivelyEnabled())
        {
            return;
        }
        if (view->IsFocusable && view->IsTabStop)
        {
            output.PushBack(view);
        }
        if (ViewGroup* group = Cast<ViewGroup>(view))
        {
            for (usize i = 0; i < group->ChildCount(); ++i)
            {
                CollectFocusable(group->GetChildAt(i), output);
            }
        }
    }

    void FocusManager::SortByTabIndex(Array<View*>& list) const
    {
        list.Sort(
            [](View* a, View* b) -> bool
            {
                const i32 aIdx = a->TabIndex;
                const i32 bIdx = b->TabIndex;
                if (aIdx > 0 && bIdx > 0)
                {
                    return aIdx < bIdx;
                }
                if (aIdx > 0 && bIdx == 0)
                {
                    return true;
                }
                if (aIdx == 0 && bIdx > 0)
                {
                    return false;
                }
                // Both 0: top-to-bottom, left-to-right.
                const Float2 aScreen = a->LocalToScreen(Float2{0, 0});
                const Float2 bScreen = b->LocalToScreen(Float2{0, 0});
                const f32 yDiff = aScreen.y - bScreen.y;
                if (Abs(yDiff) > 1.0f)
                {
                    return yDiff < 0;
                }
                const f32 xDiff = aScreen.x - bScreen.x;
                if (Abs(xDiff) > 1.0f)
                {
                    return xDiff < 0;
                }
                return false;
            });
    }

    isize FocusManager::FindCurrentIndex(const Array<View*>& list) const
    {
        for (usize i = 0; i < list.Size(); ++i)
        {
            if (list[i]->Id == m_focusedId)
            {
                return static_cast<isize>(i);
            }
        }
        return -1;
    }

    View* FocusManager::GetFocusRoot() const
    {
        // PopupLayer-constrained focus root deferred (Overlay subsystem); use the full root.
        return m_context->ActiveInputRoot();
    }

    // ============================ ShortcutManager ============================

    bool ShortcutManager::TryDispatch(KeyCode key, KeyModifiers modifiers)
    {
        View* focusedView = m_context->GetFocusManager()->FocusedView();

        // Scoped shortcuts first.
        for (const RefPtr<Shortcut>& shortcut : m_shortcuts)
        {
            if (!shortcut->IsEnabled || shortcut->Scope == nullptr)
            {
                continue;
            }
            if (!shortcut->Matches(key, modifiers))
            {
                continue;
            }
            if (focusedView != nullptr && IsInScope(focusedView, shortcut->Scope))
            {
                shortcut->Action();
                return true;
            }
        }
        // Global shortcuts.
        for (const RefPtr<Shortcut>& shortcut : m_shortcuts)
        {
            if (!shortcut->IsEnabled || shortcut->Scope != nullptr)
            {
                continue;
            }
            if (shortcut->Matches(key, modifiers))
            {
                shortcut->Action();
                return true;
            }
        }
        return false;
    }

    bool ShortcutManager::IsInScope(View* view, View* scope)
    {
        View* v = view;
        while (v != nullptr)
        {
            if (v == scope)
            {
                return true;
            }
            v = v->Parent;
        }
        return false;
    }

    // ============================ InputManager ============================

    bool InputManager::ProcessMouseMove(f32 physicalX, f32 physicalY)
    {
        const f32 dpiScale = m_context->DpiScale();
        m_mouseX = physicalX / dpiScale;
        m_mouseY = physicalY / dpiScale;

        // Drag-drop takes priority over normal mouse processing.
        if (m_context->DragDrop()->UpdateDrag(m_mouseX, m_mouseY))
        {
            return true;
        }

        FocusManager* focus = m_context->GetFocusManager();
        if (focus->HasCapture())
        {
            if (View* captured = focus->CapturedView())
            {
                const Float2 local = captured->ScreenToLocal(Float2{m_mouseX, m_mouseY});
                m_mouseArgs.Set(local.x, local.y);
                captured->OnMouseMove(m_mouseArgs);
            }
            return true;
        }

        UpdateHover(m_mouseX, m_mouseY);
        if (View* hovered = m_context->GetViewById(m_hoveredId))
        {
            const Float2 local = hovered->ScreenToLocal(Float2{m_mouseX, m_mouseY});
            m_mouseArgs.Set(local.x, local.y);
            hovered->OnMouseMove(m_mouseArgs);
            return hovered != m_context->ActiveInputRoot();
        }
        return false;
    }

    bool InputManager::ProcessMouseDown(MouseButton button, f32 physicalX, f32 physicalY,
                                        f32 totalTime)
    {
        const f32 dpiScale = m_context->DpiScale();
        m_mouseX = physicalX / dpiScale;
        m_mouseY = physicalY / dpiScale;

        // Hide tooltip on click.
        m_context->Tooltips()->OnMouseDown();

        UpdateHover(m_mouseX, m_mouseY);
        View* hitView = m_context->GetViewById(m_hoveredId);

        // Popup click-outside detection: hand the hit view to the layer so it can close the popups the
        // click landed outside of (inside the topmost popup = no-op; a lower popup = close those above;
        // completely outside = close every close-on-click-outside popup). LMB outside consumes the click.
        if (RootView* root = m_context->ActiveInputRoot())
        {
            PopupLayer* popupLayer = root->GetPopupLayer();
            if (popupLayer != nullptr && popupLayer->PopupCount() > 0)
            {
                if (popupLayer->HandleClickOutside(hitView, static_cast<i32>(button)))
                {
                    return true;
                } // LMB consumed by popup close
            }
        }

        if (hitView != nullptr)
        {
            FocusNearestFocusable(hitView);
        }
        else
        {
            m_context->GetFocusManager()->ClearFocus();
        }

        // Double-click detection.
        const f32 timeDelta = totalTime - m_lastClickTime;
        const f32 dxc = m_mouseX - m_lastClickX;
        const f32 dyc = m_mouseY - m_lastClickY;
        const f32 distSq = dxc * dxc + dyc * dyc;
        if (timeDelta < DoubleClickTime && distSq < DoubleClickDistance * DoubleClickDistance)
        {
            ++m_clickCount;
        }
        else
        {
            m_clickCount = 1;
        }
        m_lastClickTime = totalTime;
        m_lastClickX = m_mouseX;
        m_lastClickY = m_mouseY;

        m_pressedId = hitView != nullptr ? hitView->Id : ViewId::Invalid;
        m_pressedButton = button;

        // Initiate a potential drag on single left-click if the view or an ancestor is an IDragSource.
        if (hitView != nullptr && button == MouseButton::Left && m_clickCount == 1)
        {
            for (View* dragView = hitView; dragView != nullptr; dragView = dragView->Parent)
            {
                if (IDragSource* source = dragView->AsDragSource())
                {
                    m_context->DragDrop()->BeginPotentialDrag(dragView, source, m_mouseX, m_mouseY,
                                                              button);
                    break;
                }
            }
        }

        if (hitView != nullptr)
        {
            const Float2 local = hitView->ScreenToLocal(Float2{m_mouseX, m_mouseY});
            m_mouseArgs.Set(local.x, local.y, button, m_clickCount, totalTime, m_currentModifiers);
            DispatchMouseDown(hitView, m_mouseArgs);
            return hitView != m_context->ActiveInputRoot();
        }
        return false;
    }

    bool InputManager::ProcessMouseUp(MouseButton button, f32 physicalX, f32 physicalY)
    {
        const f32 dpiScale = m_context->DpiScale();
        m_mouseX = physicalX / dpiScale;
        m_mouseY = physicalY / dpiScale;

        // Drag-drop end takes priority.
        if (m_context->DragDrop()->EndDrag(m_mouseX, m_mouseY))
        {
            return true;
        }

        FocusManager* focus = m_context->GetFocusManager();
        if (focus->HasCapture())
        {
            focus->ReleaseCapture();
        }

        View* pressedView = m_context->GetViewById(m_pressedId);
        m_pressedId = ViewId::Invalid;

        bool handled = false;
        if (pressedView != nullptr)
        {
            const Float2 local = pressedView->ScreenToLocal(Float2{m_mouseX, m_mouseY});
            m_mouseArgs.Set(local.x, local.y, button);
            DispatchMouseUp(pressedView, m_mouseArgs);
            handled = pressedView != m_context->ActiveInputRoot();
        }

        UpdateHover(m_mouseX, m_mouseY);
        return handled;
    }

    bool InputManager::ProcessMouseWheel(f32 physicalX, f32 physicalY, f32 deltaX, f32 deltaY,
                                         KeyModifiers modifiers)
    {
        const f32 dpiScale = m_context->DpiScale();
        const f32 scaledX = physicalX / dpiScale;
        const f32 scaledY = physicalY / dpiScale;

        m_wheelArgs.Reset();
        m_wheelArgs.X = scaledX;
        m_wheelArgs.Y = scaledY;
        m_wheelArgs.DeltaX = deltaX;
        m_wheelArgs.DeltaY = deltaY;
        m_wheelArgs.Modifiers = modifiers;

        RootView* root = m_context->ActiveInputRoot();
        if (root == nullptr)
        {
            return false;
        }
        if (View* target = root->HitTest(Float2{scaledX, scaledY}))
        {
            DispatchMouseWheel(target, m_wheelArgs);
            return target != root;
        }
        return false;
    }

    bool InputManager::ProcessKeyDown(KeyCode key, KeyModifiers modifiers, bool isRepeat,
                                      f32 timestamp)
    {
        m_currentModifiers = modifiers;
        // Escape cancels an active drag.
        if (key == KeyCode::Escape && m_context->DragDrop()->IsDragging())
        {
            m_context->DragDrop()->CancelDrag();
            return true;
        }

        FocusManager* focus = m_context->GetFocusManager();
        View* focused = focus->FocusedView();

        // Tab drives focus traversal - except when the focused view opts in via WantsTabKey
        // (code editors inserting indentation): those views get Tab through normal dispatch
        // first, and traversal runs as the fallback below when they leave it unhandled (the
        // same dispatch-first shape as the Return/OnActivate fallback).
        const bool isTab = key == KeyCode::Tab && !isRepeat;
        const auto traverseFocus = [&] {
            if (HasFlag(modifiers, KeyModifiers::Shift))
            {
                focus->FocusPrev();
            }
            else
            {
                focus->FocusNext();
            }
        };
        if (isTab && (focused == nullptr || !focused->WantsTabKey))
        {
            traverseFocus();
            return true;
        }

        if (focused != nullptr)
        {
            m_keyArgs.Set(key, modifiers, isRepeat, timestamp);
            DispatchKeyDown(focused, m_keyArgs);
            if (m_keyArgs.Handled)
            {
                return true;
            }
        }

        if (isTab)
        {
            traverseFocus();
            return true;
        }

        // Return ACTIVATES the focused view - but only as a fallback AFTER normal dispatch
        // (deliberate deviation from Sedulous, which converted Return pre-dispatch: that locked
        // text controls out of ever seeing Return in OnKeyDown - commit-on-Enter, multiline
        // newlines - and swallowed it even when OnActivate was a no-op). Buttons and other
        // activatables don't handle Return in OnKeyDown, so they activate exactly as before.
        if (focused != nullptr && !isRepeat && key == KeyCode::Return)
        {
            focused->OnActivate();
            return true;
        }

        if (focused != nullptr && !isRepeat)
        {
            Optional<FocusDirection> dir;
            switch (key)
            {
            case KeyCode::Up:
                dir = FocusDirection::Up;
                break;
            case KeyCode::Down:
                dir = FocusDirection::Down;
                break;
            case KeyCode::Left:
                dir = FocusDirection::Left;
                break;
            case KeyCode::Right:
                dir = FocusDirection::Right;
                break;
            default:
                break;
            }
            if (dir.HasValue() && focus->MoveFocus(dir.Value()))
            {
                return true;
            }
        }

        if (m_context->GetShortcuts()->TryDispatch(key, modifiers))
        {
            return true;
        }

        if (HasFlag(modifiers, KeyModifiers::Alt))
        {
            if (RootView* root = m_context->ActiveInputRoot();
                root != nullptr && SearchAccelerator(root, key, modifiers))
            {
                return true;
            }
        }
        return false;
    }

    bool InputManager::ProcessKeyUp(KeyCode key, KeyModifiers modifiers, f32 timestamp)
    {
        m_currentModifiers = modifiers;
        View* focused = m_context->GetFocusManager()->FocusedView();
        if (focused == nullptr)
        {
            return false;
        }
        m_keyArgs.Set(key, modifiers, false, timestamp);
        DispatchKeyUp(focused, m_keyArgs);
        return m_keyArgs.Handled;
    }

    bool InputManager::ProcessTextInput(char32_t character)
    {
        View* focused = m_context->GetFocusManager()->FocusedView();
        if (focused == nullptr)
        {
            return false;
        }
        m_textArgs.Reset();
        m_textArgs.Character = character;
        DispatchTextInput(focused, m_textArgs);
        return m_textArgs.Handled;
    }

    void InputManager::OnViewDeleted(View* view)
    {
        if (m_hoveredId == view->Id)
        {
            m_hoveredId = ViewId::Invalid;
        }
        if (m_pressedId == view->Id)
        {
            m_pressedId = ViewId::Invalid;
        }
    }

    bool InputManager::SearchAccelerator(View* view, KeyCode key, KeyModifiers modifiers)
    {
        if (IAcceleratorHandler* handler = view->AsAcceleratorHandler())
        {
            if (handler->HandleAccelerator(key, modifiers))
            {
                return true;
            }
        }
        if (ViewGroup* group = Cast<ViewGroup>(view))
        {
            for (usize i = 0; i < group->ChildCount(); ++i)
            {
                if (SearchAccelerator(group->GetChildAt(i), key, modifiers))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void InputManager::UpdateHover(f32 x, f32 y)
    {
        RootView* root = m_context->ActiveInputRoot();
        View* hitView = (root != nullptr) ? root->HitTest(Float2{x, y}) : nullptr;
        const ViewId newHoverId = (hitView != nullptr) ? hitView->Id : ViewId::Invalid;

        if (newHoverId != m_hoveredId)
        {
            if (View* oldHovered = m_context->GetViewById(m_hoveredId))
            {
                oldHovered->OnMouseLeave();
            }
            m_hoveredId = newHoverId;
            if (hitView != nullptr)
            {
                hitView->OnMouseEnter();
            }
            // Notify tooltip manager of hover change.
            m_context->Tooltips()->OnHoverChanged(hitView);
        }

        m_currentCursor =
            (hitView != nullptr) ? hitView->EffectiveCursor(Float2{x, y}) : CursorType::Default;
    }

    void InputManager::FocusNearestFocusable(View* view)
    {
        View* v = view;
        while (v != nullptr)
        {
            if (v->IsFocusable)
            {
                m_context->GetFocusManager()->SetFocus(v);
                return;
            }
            v = v->Parent;
        }
        m_context->GetFocusManager()->ClearFocus();
    }

    i32 InputManager::BuildAncestorChain(View* target)
    {
        i32 count = 0;
        View* v = target;
        while (v != nullptr && count < static_cast<i32>(kMaxAncestors))
        {
            m_ancestorChain[count++] = v;
            v = v->Parent;
        }
        for (i32 i = 0; i < count / 2; ++i)
        {
            View* tmp = m_ancestorChain[i];
            m_ancestorChain[i] = m_ancestorChain[count - 1 - i];
            m_ancestorChain[count - 1 - i] = tmp;
        }
        return count;
    }

    void InputManager::DispatchMouseDown(View* target, MouseEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnMouseDownCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnMouseDown(args);
        args.Phase = EventPhase::Bubble;
        args.X += target->Bounds.x;
        args.Y += target->Bounds.y;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnMouseDown(args);
            args.X += v->Bounds.x;
            args.Y += v->Bounds.y;
            v = v->Parent;
        }
    }

    void InputManager::DispatchMouseUp(View* target, MouseEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnMouseUpCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnMouseUp(args);
        args.Phase = EventPhase::Bubble;
        args.X += target->Bounds.x;
        args.Y += target->Bounds.y;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnMouseUp(args);
            args.X += v->Bounds.x;
            args.Y += v->Bounds.y;
            v = v->Parent;
        }
    }

    void InputManager::DispatchKeyDown(View* target, KeyEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnKeyDownCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnKeyDown(args);
        args.Phase = EventPhase::Bubble;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnKeyDown(args);
            v = v->Parent;
        }
    }

    void InputManager::DispatchKeyUp(View* target, KeyEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnKeyUpCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnKeyUp(args);
        args.Phase = EventPhase::Bubble;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnKeyUp(args);
            v = v->Parent;
        }
    }

    void InputManager::DispatchMouseWheel(View* target, MouseWheelEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnMouseWheelCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnMouseWheel(args);
        args.Phase = EventPhase::Bubble;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnMouseWheel(args);
            v = v->Parent;
        }
    }

    void InputManager::DispatchTextInput(View* target, TextInputEventArgs& args)
    {
        const i32 chainLen = BuildAncestorChain(target);
        args.Phase = EventPhase::Capture;
        for (i32 i = 0; i < chainLen - 1; ++i)
        {
            if (args.Handled)
            {
                return;
            }
            m_ancestorChain[i]->OnTextInputCapture(args);
        }
        if (args.Handled)
        {
            return;
        }
        args.Phase = EventPhase::Target;
        target->OnTextInput(args);
        args.Phase = EventPhase::Bubble;
        View* v = target->Parent;
        while (v != nullptr && !args.Handled)
        {
            v->OnTextInput(args);
            v = v->Parent;
        }
    }
}
