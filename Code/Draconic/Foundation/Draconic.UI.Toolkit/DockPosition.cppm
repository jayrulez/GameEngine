// Draconic UI Toolkit - :dock_position partition
//
// Position for docking a panel relative to a target. Ported 1:1 from
// Sedulous.UI.Toolkit/src/Docking/DockPosition.bf (a plain enum).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.toolkit:dock_position;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Position for docking a panel relative to a target.
    enum class DockPosition
    {
        Left,
        Right,
        Top,
        Bottom,
        Center,
        Float
    };
}
