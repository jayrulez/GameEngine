// Draconic UI - :iaccelerator_handler partition
//
// Implement on a View to receive Alt+key accelerator events. Accelerators are searched top-down
// through the tree (via View::AsAcceleratorHandler()), bypassing normal focus-based key routing.
// Ported from Sedulous.UI/src/Input/IAcceleratorHandler.bf. Pattern A (tree-queried) - the interface
// is a plain abstract base; a View exposes it via a virtual AsAcceleratorHandler() capability query.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:iaccelerator_handler;

import draconic.foundation;
import :input_enums; // KeyCode, KeyModifiers

using namespace draconic::foundation;

export namespace draconic::ui
{
    class IAcceleratorHandler
    {
    public:
        virtual ~IAcceleratorHandler() = default;
        /// Return true if this handler consumed the accelerator.
        virtual bool HandleAccelerator(KeyCode key, KeyModifiers modifiers) = 0;
    };
}
