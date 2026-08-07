// Draconic GUI - :control_state partition
//
// ControlState: the visual state a stateful drawable (skins, StateListDrawable) selects
// on. Derived from eepp's UI skin states (uistate.hpp); a flat enum for now - the eepp
// bitmask best-match machinery lands with the skin/UINode phase.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:control_state;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::gui
{
    enum class ControlState : u32
    {
        Normal = 0,
        Hover,
        Pressed,
        Focused,
        Disabled,
        Selected,
    };
}
