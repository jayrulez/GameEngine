// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drag_drop_effects partition
//
// Describes the type of operation a drag-and-drop will perform. Ported verbatim from
// Sedulous.UI/src/DragDrop/DragDropEffects.bf (Beef `: int32` -> `: i32`).

module;
#include "Core/Prelude.h"

export module foundation.ui:drag_drop_effects;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Describes the type of operation a drag-and-drop will perform.
    enum class DragDropEffects : i32
    {
        /// No drop allowed.
        None = 0,
        /// The data will be moved from source to target.
        Move = 1,
        /// The data will be copied to the target.
        Copy = 2,
        /// A link/reference will be created at the target.
        Link = 4
    };
}
