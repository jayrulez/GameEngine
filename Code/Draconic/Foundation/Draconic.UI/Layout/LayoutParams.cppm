// Draconic UI - :layout_params partition
//
// Base layout parameters for a view within a container. Container-specific subclasses add fields
// (e.g. FlexLayoutParams adds Grow/Shrink). Ported from Sedulous.UI/src/Layout/LayoutParams.bf.
// Object + DRACONIC_OBJECT so the layout algorithms can Cast<T> down to their param subclasses.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:layout_params;

import draconic.foundation; // Object
import :thickness;
import :size_spec;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class LayoutParams : public Object
    {
        DRACONIC_OBJECT(LayoutParams, Object)
    public:
        /// Desired width. Default: Wrap (fit to content).
        SizeSpec Width = SizeSpec::Wrap();
        /// Desired height. Default: Wrap (fit to content).
        SizeSpec Height = SizeSpec::Wrap();
        /// Margin (space between this view and siblings/parent).
        Thickness Margin{};

        LayoutParams() = default;
    };

    DRACONIC_DEFINE_OBJECT(LayoutParams, "draconic::ui")
}
