// Draconic UI - :popup_positioner partition
//
// Static positioning helpers for popups, tooltips, and menus - pure calculations, no state. Ported from
// Sedulous.UI/src/Overlay/PopupPositioner.bf. Beef `static class` -> a struct of static methods; the
// `(float x, float y)` tuple returns -> Float2; RectangleF -> foundation::Rectangle; Vector2 -> Float2.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:popup_positioner;

import draconic.foundation; // Rectangle, Float2

using namespace draconic::foundation;

export namespace draconic::ui
{
    struct PopupPositioner
    {
        /// Position below the anchor; flip above if it clips the bottom; clamp to screen.
        [[nodiscard]] static Float2 BestFit(Rectangle anchor, Float2 popupSize, Rectangle screen)
        {
            f32 x = anchor.x;
            f32 y = anchor.y + anchor.height;
            if (y + popupSize.y > screen.y + screen.height)
            {
                y = anchor.y - popupSize.y;
            } // flip above
            if (x + popupSize.x > screen.x + screen.width)
            {
                x = screen.x + screen.width - popupSize.x;
            }
            if (x < screen.x)
            {
                x = screen.x;
            }
            if (y < screen.y)
            {
                y = screen.y;
            }
            return Float2{x, y};
        }

        /// Position directly below the anchor, clamped horizontally.
        [[nodiscard]] static Float2 Below(Rectangle anchor, Float2 popupSize, Rectangle screen)
        {
            f32 x = anchor.x;
            const f32 y = anchor.y + anchor.height;
            if (x + popupSize.x > screen.x + screen.width)
            {
                x = screen.x + screen.width - popupSize.x;
            }
            if (x < screen.x)
            {
                x = screen.x;
            }
            return Float2{x, y};
        }

        /// Position directly above the anchor, clamped to screen.
        [[nodiscard]] static Float2 Above(Rectangle anchor, Float2 popupSize, Rectangle screen)
        {
            f32 x = anchor.x;
            f32 y = anchor.y - popupSize.y;
            if (x + popupSize.x > screen.x + screen.width)
            {
                x = screen.x + screen.width - popupSize.x;
            }
            if (x < screen.x)
            {
                x = screen.x;
            }
            if (y < screen.y)
            {
                y = screen.y;
            }
            return Float2{x, y};
        }

        /// Position to the right of a parent menu; flip left if it clips the right edge.
        [[nodiscard]] static Float2 Submenu(Rectangle parent, Float2 popupSize, Rectangle screen)
        {
            const f32 gap = 2.0f; // clear the parent menu's border instead of overlapping it
            f32 x = parent.x + parent.width + gap;
            f32 y = parent.y;
            if (x + popupSize.x > screen.x + screen.width)
            {
                x = parent.x - popupSize.x - gap;
            }
            if (y + popupSize.y > screen.y + screen.height)
            {
                y = screen.y + screen.height - popupSize.y;
            }
            if (y < screen.y)
            {
                y = screen.y;
            }
            return Float2{x, y};
        }

        /// Center the popup within the screen.
        [[nodiscard]] static Float2 Center(Float2 popupSize, Rectangle screen)
        {
            return Float2{screen.x + (screen.width - popupSize.x) * 0.5f,
                          screen.y + (screen.height - popupSize.y) * 0.5f};
        }
    };
}
