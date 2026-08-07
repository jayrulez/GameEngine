// Draconic UI - :ipopup_owner partition
//
// Implemented to receive notification when a popup you opened is closed. In practice every owner is
// also a View; OwnerView() exposes that so PopupLayer can walk parent chains to cascade-close popups
// when an owner's subtree is torn down. Ported from Sedulous.UI/src/Overlay/IPopupOwner.bf. Pattern-B
// injected interface (PopupLayer holds the IPopupOwner* passed to ShowPopup); View is forward-declared
// (only pointers), so no :view dependency.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:ipopup_owner;

export namespace draconic::ui
{
    class View;

    class IPopupOwner
    {
    public:
        virtual ~IPopupOwner() = default;
        /// Called when a popup this owner opened is closed.
        virtual void OnPopupClosed(View* popup) = 0;
        /// The owning View (all owners are Views), or null.
        [[nodiscard]] virtual View* OwnerView() = 0;
    };
}
