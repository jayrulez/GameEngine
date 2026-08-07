// Draconic UI - :debug_settings partition
//
// Flags controlling which debug overlays are drawn after the normal render pass.
// Zero overhead when all flags are false (AnyEnabled() gates the work). Ported from
// Sedulous.UI/src/Debug/UIDebugDrawSettings.bf.

export module draconic.ui:debug_settings;

export namespace draconic::ui
{
    struct UIDebugDrawSettings
    {
        /// Red outline around every view's bounds.
        bool ShowBounds = false;
        /// Green fill for ViewGroup padding regions.
        bool ShowPadding = false;
        /// Orange fill for LayoutParams margin regions.
        bool ShowMargin = false;
        /// Numbered overlay showing draw order within parent.
        bool ShowZOrder = false;
        /// Yellow highlight on the view under the cursor.
        bool ShowHitTarget = false;
        /// Blue outline on focused view and ancestors with focus-within.
        bool ShowFocusPath = false;
        /// Numbered focus arrows showing tab order.
        bool ShowTabOrder = false;

        /// True if any debug flag is enabled.
        [[nodiscard]] constexpr bool AnyEnabled() const noexcept
        {
            return ShowBounds || ShowPadding || ShowMargin || ShowZOrder || ShowHitTarget ||
                   ShowFocusPath || ShowTabOrder;
        }
    };
}
