// Draconic UI - :itooltip_provider partition
//
// Implement on a View to provide custom tooltip content instead of plain text. TooltipManager checks
// for this interface first (via View::AsTooltipProvider()); if absent, falls back to View.TooltipText.
// Ported from Sedulous.UI/src/Overlay/ITooltipProvider.bf. Pattern A (tree-queried) - the interface is
// a plain abstract base; a View exposes it via a virtual AsTooltipProvider() capability query. Beef
// `View CreateTooltipContent()` transfers ownership to the TooltipView -> returns RefPtr<View> (RAII).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:itooltip_provider;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;

    class ITooltipProvider
    {
    public:
        virtual ~ITooltipProvider() = default;
        /// Create the tooltip content view. Ownership transfers to the TooltipView. Return null to
        /// suppress the tooltip.
        [[nodiscard]] virtual RefPtr<View> CreateTooltipContent() = 0;
    };
}
