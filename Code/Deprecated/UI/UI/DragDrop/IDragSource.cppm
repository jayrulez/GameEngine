// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :idrag_source partition
//
// Implement on a View subclass to make it draggable. Ported from Sedulous.UI/src/DragDrop/IDragSource.bf.
// Pattern A (tree-queried) - the interface is a plain abstract base; a View exposes it via a virtual
// AsDragSource() capability query, and the InputManager walks the parent chain of the pressed view to
// find one. Beef owned returns -> RefPtr (CreateDragData / CreateDragVisual transfer ownership to the
// DragDropManager); View/DragData forward-declared (only used behind pointers/RefPtr).

module;
#include "Core/Prelude.h"

export module foundation.ui:idrag_source;

import foundation.core;
import :drag_drop_effects;

using namespace foundation::core;

export namespace foundation::ui
{
    class View;
    class DragData;

    class IDragSource
    {
    public:
        virtual ~IDragSource() = default;

        /// Create the data payload for this drag. Return null to cancel the drag before it starts.
        [[nodiscard]] virtual RefPtr<DragData> CreateDragData() = 0;

        /// Create a visual preview shown during the drag. Return null for a default semi-transparent
        /// indicator. Ownership transfers to the DragDropManager (dropped on drag end).
        [[nodiscard]] virtual RefPtr<View> CreateDragVisual(DragData* data) = 0;

        /// Called when the drag actually starts (threshold exceeded). Use to customize DragDropManager
        /// properties (AdornerOffset, cursors).
        virtual void OnDragStarted(DragData* data) = 0;

        /// Called when the drag ends (completed or cancelled).
        virtual void OnDragCompleted(DragData* data, DragDropEffects effect, bool cancelled) = 0;
    };
}
