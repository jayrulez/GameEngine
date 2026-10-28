// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drag_drop_manager partition
//
// Drag-and-drop state machine within a UIContext, driven by the InputManager at the mouse-event points.
// Ported from Sedulous.UI/src/DragDrop/DragDropManager.bf. Like the Input/Tooltip managers this is a
// by-value member of UIContext; all View-touching bodies live in the module impl unit (DragDrop/DragImpl.cpp),
// so View/UIContext/RootView/PopupLayer/DragAdorner/IDragSource/IDropTarget are only forward-declared here.
// Ownership: Beef `DragData ~ delete _` -> RefPtr<DragData>; the adorner is a raw pointer (PopupLayer owns
// it, ownsView:true); source/drop-target views are borrowed raw pointers cleared by OnViewDeleted.

module;
#include "Core/Prelude.h"

export module foundation.ui:drag_drop_manager;

import foundation.core;
import :drag_drop_effects;
import :drag_data;   // RefPtr<DragData> member (complete type -> no incomplete-RefPtr gcc issue)
import :input_enums; // MouseButton
import :enums;       // CursorType

using namespace foundation::core;

export namespace foundation::ui
{
    class View;
    class UIContext;
    class DragAdorner;
    class PopupLayer;
    class IDragSource;
    class IDropTarget;

    /// Drag state machine states.
    enum class DragState
    {
        /// No drag in progress.
        Idle,
        /// Mouse was pressed on a drag source, waiting for threshold.
        Potential,
        /// Drag is active: adorner shown, drop targets being queried.
        Active
    };

    /// Manages drag-and-drop operations within a UIContext. Called by InputManager at the right points in
    /// the mouse event pipeline.
    class DragDropManager
    {
    public:
        explicit DragDropManager(UIContext* context) : m_context(context) {}
        ~DragDropManager();

        DragDropManager(const DragDropManager&) = delete;
        DragDropManager& operator=(const DragDropManager&) = delete;

        // === Tunables (a drag source may customize offset/cursors in OnDragStarted) ===
        /// Drag threshold in screen pixels.
        f32 DragThreshold = 4.0f;
        /// Offset of the drag visual from the cursor.
        f32 AdornerOffsetX = 4.0f;
        f32 AdornerOffsetY = 4.0f;
        /// Cursor shown when over an accepting drop target.
        CursorType AcceptCursor = CursorType::Move;
        /// Cursor shown when over a rejecting drop target.
        CursorType RejectCursor = CursorType::NotAllowed;

        // === Queries ===
        [[nodiscard]] f32 LastScreenX() const noexcept { return m_lastScreenX; }
        [[nodiscard]] f32 LastScreenY() const noexcept { return m_lastScreenY; }
        [[nodiscard]] DragState State() const noexcept { return m_state; }
        [[nodiscard]] bool IsDragging() const noexcept { return m_state == DragState::Active; }
        [[nodiscard]] bool IsPotentialDrag() const noexcept
        {
            return m_state == DragState::Potential;
        }
        [[nodiscard]] DragData* CurrentDragData() const noexcept { return m_dragData.Get(); }
        [[nodiscard]] DragDropEffects CurrentEffect() const noexcept { return m_currentEffect; }

        // === Public API (called by InputManager) ===
        bool BeginPotentialDrag(View* sourceView, IDragSource* source, f32 screenX, f32 screenY,
                                MouseButton button);
        bool UpdateDrag(f32 screenX, f32 screenY);
        bool EndDrag(f32 screenX, f32 screenY);
        void CancelDrag();
        void OnViewDeleted(View* view);

    private:
        bool ActivateDrag();
        void UpdateAdornerPosition(f32 screenX, f32 screenY);
        void UpdateDropTarget(f32 screenX, f32 screenY);
        void FindDropTarget(View* hitView, View*& targetView, IDropTarget*& target);
        void CompleteDrag(DragDropEffects effect, bool cancelled);

        UIContext* m_context = nullptr;

        DragState m_state = DragState::Idle;

        // Drag session data.
        View* m_sourceView = nullptr;
        IDragSource* m_dragSource = nullptr;
        RefPtr<DragData> m_dragData;
        DragAdorner* m_adorner = nullptr; // owned by PopupLayer (ownsView:true)
        PopupLayer* m_adornerPopupLayer = nullptr;
        MouseButton m_dragButton = MouseButton::Left;

        // Potential-drag tracking.
        f32 m_startScreenX = 0.0f;
        f32 m_startScreenY = 0.0f;

        // Drop-target tracking.
        View* m_currentDropTargetView = nullptr;
        IDropTarget* m_currentDropTarget = nullptr;
        DragDropEffects m_currentEffect = DragDropEffects::None;

        // Last known screen position during drag.
        f32 m_lastScreenX = 0.0f;
        f32 m_lastScreenY = 0.0f;
    };
}
