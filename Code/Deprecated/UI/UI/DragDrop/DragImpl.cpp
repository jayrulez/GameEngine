// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - module implementation unit for DragDropManager.
//
// Holds the View-touching bodies of DragDropManager (it reaches across the whole View cluster: UIContext,
// RootView, PopupLayer, DragAdorner, IDragSource, IDropTarget). Like InputImpl.cpp / TooltipImpl.cpp it
// lives in an implementation unit outside the interface-partition dependency graph, so :drag_drop_manager
// only forward-declares the view types. Ported from Sedulous.UI/src/DragDrop/DragDropManager.bf.

module;
#include "Core/Prelude.h"

module foundation.ui;

using namespace foundation::core;

namespace foundation::ui
{
    DragDropManager::~DragDropManager()
    {
        // Don't call CancelDrag/CompleteDrag during destruction - other managers may already be gone.
        // Just clean up owned data (RefPtr drop).
        m_dragData = nullptr;
        m_state = DragState::Idle;
    }

    bool DragDropManager::BeginPotentialDrag(View* sourceView, IDragSource* source, f32 screenX,
                                             f32 screenY, MouseButton button)
    {
        if (m_state != DragState::Idle)
        {
            return false;
        }
        if (button != MouseButton::Left)
        {
            return false;
        }

        m_sourceView = sourceView;
        m_dragSource = source;
        m_dragButton = button;
        m_startScreenX = screenX;
        m_startScreenY = screenY;
        m_state = DragState::Potential;
        return true;
    }

    bool DragDropManager::UpdateDrag(f32 screenX, f32 screenY)
    {
        m_context->MarkNeedsRedraw(); // the adorner follows the cursor (damage gating)
        if (m_state == DragState::Idle)
        {
            return false;
        }

        m_lastScreenX = screenX;
        m_lastScreenY = screenY;

        if (m_state == DragState::Potential)
        {
            const f32 dx = screenX - m_startScreenX;
            const f32 dy = screenY - m_startScreenY;
            const f32 dist = Sqrt(dx * dx + dy * dy);

            if (dist < DragThreshold)
            {
                return false;
            } // Not yet - let normal mouse processing continue.

            // Threshold exceeded - activate drag.
            if (!ActivateDrag())
            {
                m_state = DragState::Idle;
                m_sourceView = nullptr;
                m_dragSource = nullptr;
                return false;
            }
        }

        // Active drag - update adorner and drop target.
        UpdateAdornerPosition(screenX, screenY);
        UpdateDropTarget(screenX, screenY);
        return true;
    }

    bool DragDropManager::EndDrag(f32 screenX, f32 screenY)
    {
        if (m_state == DragState::Idle)
        {
            return false;
        }

        m_lastScreenX = screenX;
        m_lastScreenY = screenY;

        if (m_state == DragState::Potential)
        {
            // Never reached threshold - cancel silently.
            m_state = DragState::Idle;
            m_sourceView = nullptr;
            m_dragSource = nullptr;
            return false;
        }

        // Active drag - attempt drop.
        UpdateDropTarget(screenX, screenY);

        // Close adorner BEFORE OnDrop. OnDrop may destroy views (and their PopupLayer) as part of re-docking.
        if (m_adorner != nullptr)
        {
            if (m_adornerPopupLayer != nullptr)
            {
                m_adornerPopupLayer->ClosePopup(m_adorner);
            }
            m_adorner = nullptr;
            m_adornerPopupLayer = nullptr;
        }

        // Clear source view reference before OnDrop so that if OnDrop triggers tree modifications,
        // OnViewDeleted won't prematurely fire CompleteDrag.
        View* savedSourceView = m_sourceView;
        m_sourceView = nullptr;

        DragDropEffects effect = DragDropEffects::None;
        if (m_currentDropTarget != nullptr && m_currentEffect != DragDropEffects::None)
        {
            const Float2 local = m_currentDropTargetView->ScreenToLocal(Float2{screenX, screenY});
            effect = m_currentDropTarget->OnDrop(m_dragData.Get(), local.x, local.y);
        }

        m_sourceView = savedSourceView;
        CompleteDrag(effect, effect == DragDropEffects::None);
        return true;
    }

    void DragDropManager::CancelDrag()
    {
        if (m_state == DragState::Idle)
        {
            return;
        }

        if (m_state == DragState::Active)
        {
            CompleteDrag(DragDropEffects::None, true);
        }
        else
        {
            m_state = DragState::Idle;
            m_sourceView = nullptr;
            m_dragSource = nullptr;
        }
    }

    void DragDropManager::OnViewDeleted(View* view)
    {
        if (m_state == DragState::Idle)
        {
            return;
        }

        if (view == m_sourceView)
        {
            if (m_state == DragState::Active)
            {
                CompleteDrag(DragDropEffects::None, true);
            }
            else
            {
                m_state = DragState::Idle;
                m_sourceView = nullptr;
                m_dragSource = nullptr;
            }
            return;
        }

        if (view == m_currentDropTargetView)
        {
            m_currentDropTarget->OnDragLeave(m_dragData.Get());
            m_currentDropTargetView = nullptr;
            m_currentDropTarget = nullptr;
            m_currentEffect = DragDropEffects::None;
        }
    }

    bool DragDropManager::ActivateDrag()
    {
        // Reset customizable properties to defaults.
        AdornerOffsetX = 4.0f;
        AdornerOffsetY = 4.0f;
        AcceptCursor = CursorType::Move;
        RejectCursor = CursorType::NotAllowed;

        // Ask source for data.
        m_dragData = m_dragSource->CreateDragData();
        if (!m_dragData)
        {
            return false;
        }

        // Ask source for visual.
        RefPtr<View> visual = m_dragSource->CreateDragVisual(m_dragData.Get());

        // Notify source (can customize offset/cursor here).
        m_dragSource->OnDragStarted(m_dragData.Get());

        // Create adorner with final offset values.
        RefPtr<DragAdorner> adorner =
            MakeRef<DragAdorner>(m_context->Allocator(), visual.Get(), AdornerOffsetX, AdornerOffsetY);
        m_adorner = adorner.Get();

        // Show adorner via PopupLayer.
        RootView* root = m_context->ActiveInputRoot();
        if (root == nullptr)
        {
            m_adorner = nullptr;
            return false;
        }

        m_adornerPopupLayer = root->GetPopupLayer();
        m_adornerPopupLayer->ShowPopup(adorner.Get(), nullptr, m_startScreenX + AdornerOffsetX,
                                       m_startScreenY + AdornerOffsetY, false, false, true);

        // Set mouse capture on the source view.
        m_context->GetFocusManager()->SetCapture(m_sourceView);

        m_state = DragState::Active;
        return true;
    }

    void DragDropManager::UpdateAdornerPosition(f32 screenX, f32 screenY)
    {
        if (m_adorner == nullptr || m_adornerPopupLayer == nullptr)
        {
            return;
        }

        m_adornerPopupLayer->UpdatePopupPosition(m_adorner, screenX + m_adorner->OffsetX(),
                                                 screenY + m_adorner->OffsetY());
    }

    void DragDropManager::UpdateDropTarget(f32 screenX, f32 screenY)
    {
        RootView* root = m_context->ActiveInputRoot();
        View* hitView = (root != nullptr) ? root->HitTest(Float2{screenX, screenY}) : nullptr;

        // Walk parent chain to find an IDropTarget.
        View* newTargetView = nullptr;
        IDropTarget* newTarget = nullptr;
        FindDropTarget(hitView, newTargetView, newTarget);

        if (newTarget != m_currentDropTarget)
        {
            // Leave old target.
            if (m_currentDropTarget != nullptr)
            {
                m_currentDropTarget->OnDragLeave(m_dragData.Get());
            }

            m_currentDropTargetView = newTargetView;
            m_currentDropTarget = newTarget;

            // Enter new target.
            if (m_currentDropTarget != nullptr)
            {
                const Float2 local = newTargetView->ScreenToLocal(Float2{screenX, screenY});
                m_currentDropTarget->OnDragEnter(m_dragData.Get(), local.x, local.y);
                m_currentEffect =
                    m_currentDropTarget->CanAcceptDrop(m_dragData.Get(), local.x, local.y);
            }
            else
            {
                m_currentEffect = DragDropEffects::None;
            }
        }
        else if (m_currentDropTarget != nullptr)
        {
            // Same target - fire over.
            const Float2 local = m_currentDropTargetView->ScreenToLocal(Float2{screenX, screenY});
            m_currentDropTarget->OnDragOver(m_dragData.Get(), local.x, local.y);
            m_currentEffect =
                m_currentDropTarget->CanAcceptDrop(m_dragData.Get(), local.x, local.y);
        }
    }

    void DragDropManager::FindDropTarget(View* hitView, View*& targetView, IDropTarget*& target)
    {
        targetView = nullptr;
        target = nullptr;

        View* current = hitView;
        while (current != nullptr)
        {
            if (IDropTarget* dt = current->AsDropTarget())
            {
                targetView = current;
                target = dt;
                return;
            }
            current = current->Parent;
        }
    }

    void DragDropManager::CompleteDrag(DragDropEffects effect, bool cancelled)
    {
        // Leave current drop target.
        if (m_currentDropTarget != nullptr)
        {
            m_currentDropTarget->OnDragLeave(m_dragData.Get());
            m_currentDropTargetView = nullptr;
            m_currentDropTarget = nullptr;
            m_currentEffect = DragDropEffects::None;
        }

        // Release capture.
        m_context->GetFocusManager()->ReleaseCapture();

        // Remove adorner (PopupLayer owns it, will drop it).
        if (m_adorner != nullptr)
        {
            if (m_adornerPopupLayer != nullptr)
            {
                m_adornerPopupLayer->ClosePopup(m_adorner);
            }
            m_adorner = nullptr;
            m_adornerPopupLayer = nullptr;
        }

        // Notify source.
        if (m_dragSource != nullptr)
        {
            m_dragSource->OnDragCompleted(m_dragData.Get(), effect, cancelled);
        }

        // Clean up.
        m_dragData = nullptr;
        m_sourceView = nullptr;
        m_dragSource = nullptr;
        m_state = DragState::Idle;
    }
}
