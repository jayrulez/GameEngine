// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :idrop_target partition
//
// Implement on a View subclass to make it a drop target. Ported from Sedulous.UI/src/DragDrop/IDropTarget.bf.
// Pattern A (tree-queried) - the interface is a plain abstract base; a View exposes it via a virtual
// AsDropTarget() capability query, and the DragDropManager walks the parent chain of the hit view to find
// one. DragData forward-declared (used behind a pointer).

module;
#include "Core/Prelude.h"

export module foundation.ui:idrop_target;

import foundation.core;
import :drag_drop_effects;

using namespace foundation::core;

export namespace foundation::ui
{
    class DragData;

    class IDropTarget
    {
    public:
        virtual ~IDropTarget() = default;

        /// Check if this target can accept the given drag data at this position. Called each frame while
        /// dragging over this target.
        [[nodiscard]] virtual DragDropEffects CanAcceptDrop(DragData* data, f32 localX,
                                                            f32 localY) = 0;

        /// Called when a drag enters this target's bounds.
        virtual void OnDragEnter(DragData* data, f32 localX, f32 localY) = 0;

        /// Called each frame while a drag is over this target.
        virtual void OnDragOver(DragData* data, f32 localX, f32 localY) = 0;

        /// Called when a drag leaves this target's bounds.
        virtual void OnDragLeave(DragData* data) = 0;

        /// Called when data is dropped on this target. Returns the actual effect performed.
        [[nodiscard]] virtual DragDropEffects OnDrop(DragData* data, f32 localX, f32 localY) = 0;
    };
}
