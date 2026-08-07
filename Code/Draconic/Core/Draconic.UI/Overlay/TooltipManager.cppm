// Draconic UI - :tooltip_manager partition
//
// Manages tooltip display timing. Owns a single reusable TooltipView; ticked by UIContext each frame
// (Update), notified of hover/press by the InputManager and of deletions by UIContext. Ported from
// Sedulous.UI/src/Overlay/TooltipManager.bf. Like the Input managers, this is a by-value member of
// UIContext; all View-touching bodies live in the module impl unit (Overlay/TooltipImpl.cpp), so View/
// UIContext/RootView/TooltipView are only forward-declared here (the TooltipView is held as a RefPtr).
// Beef `new/delete` of the TooltipView -> RefPtr (RAII); the dtor still nulls the view's Parent/Context
// because the PopupLayer may outlive this manager (the view is shown ownsView:false).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:tooltip_manager;

import draconic.foundation;
import :view_id;
import :tooltip_placement;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;
    class UIContext;
    class TooltipView;

    /// Manages tooltip display timing. Owns a single reusable TooltipView. Ticked by UIContext each frame.
    class TooltipManager
    {
    public:
        explicit TooltipManager(UIContext* context);
        ~TooltipManager();

        TooltipManager(const TooltipManager&) = delete;
        TooltipManager& operator=(const TooltipManager&) = delete;

        /// Seconds before tooltip appears after hover starts.
        f32 ShowDelay = 0.5f;
        /// Seconds before tooltip auto-hides.
        f32 AutoHideDelay = 5.0f;

        /// Called when the hover target changes.
        void OnHoverChanged(View* newTarget);

        /// Called when the mouse is pressed - hide tooltip unless interactive and click is on the tooltip.
        void OnMouseDown();

        /// Tick each frame. Shows the tooltip after the delay, auto-hides after the timeout.
        void Update(f32 deltaTime);

        void OnViewDeleted(View* view);

    private:
        void Show(View* target);
        void Hide();
        [[nodiscard]] bool IsTooltipOrDescendant(View* view) const;

        static Float2 PositionTooltip(TooltipPlacement placement, f32 targetX, f32 targetY,
                                      f32 targetW, f32 targetH, Float2 popupSize, Rectangle screen);

        UIContext* m_context = nullptr;
        RefPtr<TooltipView> m_tooltipView;
        ViewId m_hoverTarget = ViewId::Invalid;
        f32 m_hoverTime = 0.0f;
        f32 m_showTime = 0.0f;
        bool m_showing = false;
        bool m_interactive = false;
    };
}
