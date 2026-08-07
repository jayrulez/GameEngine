// Draconic UI - :drag_drop_effects partition
//
// Describes the type of operation a drag-and-drop will perform. Ported verbatim from
// Sedulous.UI/src/DragDrop/DragDropEffects.bf (Beef `: int32` -> `: i32`).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:drag_drop_effects;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
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
