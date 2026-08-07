// Draconic UI Toolkit - :dock_zone_indicator partition
//
// Overlay showing dock drop zones during drag operations. Hit-test transparent; drawn manually by
// DockManager. Ported from Sedulous.UI.Toolkit/src/Docking/DockZoneIndicator.bf. Beef `List<DockTarget>`
// -> Array<DockTarget>; Beef nullable `DockTarget? HoveredTarget` -> Optional<DockTarget> HoveredTarget();
// `RectangleF` -> Rectangle; byte `Color(...)` -> private static Rgb().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:dock_zone_indicator;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :dock_position;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// A dock target zone with position and bounds.
    struct DockTarget
    {
        DockPosition Position = DockPosition::Center;
        Rectangle Rect{};
        View* RelativeTo = nullptr; // the view this zone docks relative to
    };

    /// Overlay that shows dock drop zones during drag operations.
    class DockZoneIndicator : public View
    {
        DRACONIC_OBJECT(DockZoneIndicator, View)
    public:
        /// Accent color for the drop zones. Set by the owning DockManager from its resolved theme
        /// AccentColor (the indicator is drawn manually and never in the styled tree, so it can't
        /// resolve styles itself). Defaults to the classic blue.
        Color Accent{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f};

        DockZoneIndicator() { IsHitTestVisible = false; }

        [[nodiscard]] i32 TargetCount() const { return static_cast<i32>(m_targets.Size()); }
        [[nodiscard]] i32 HoveredIndex() const { return m_hoveredIndex; }

        /// Clear all targets.
        void ClearTargets()
        {
            m_targets.Clear();
            m_hoveredIndex = -1;
        }

        /// Add a dock zone target.
        void AddTarget(DockPosition position, Rectangle rect, View* relativeTo)
        {
            DockTarget t;
            t.Position = position;
            t.Rect = rect;
            t.RelativeTo = relativeTo;
            m_targets.PushBack(t);
        }

        /// Update hover state from screen coordinates.
        void UpdateHover(f32 x, f32 y)
        {
            m_hoveredIndex = -1;
            for (i32 i = 0; i < static_cast<i32>(m_targets.Size()); ++i)
            {
                const Rectangle r = m_targets[static_cast<usize>(i)].Rect;
                if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                {
                    m_hoveredIndex = i;
                    return;
                }
            }
        }

        /// Get the hovered target, or empty.
        [[nodiscard]] Optional<DockTarget> HoveredTarget() const
        {
            if (m_hoveredIndex >= 0 && m_hoveredIndex < static_cast<i32>(m_targets.Size()))
            {
                return Optional<DockTarget>(m_targets[static_cast<usize>(m_hoveredIndex)]);
            }
            return Optional<DockTarget>{};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            // Use the accent handed down by the DockManager (this overlay isn't in the styled tree),
            // so the drop zones follow the active theme instead of being hardcoded blue.
            const Color zoneColor = Color{Accent.r, Accent.g, Accent.b, 80.0f / 255.0f};
            const Color zoneBorder = Color{Accent.r, Accent.g, Accent.b, 200.0f / 255.0f};
            const Color hoverColor = Color{zoneColor.r, zoneColor.g, zoneColor.b,
                                           Min(1.0f, zoneColor.a + 60.0f / 255.0f)};

            for (i32 i = 0; i < static_cast<i32>(m_targets.Size()); ++i)
            {
                const DockTarget target = m_targets[static_cast<usize>(i)];
                const bool isHovered = (i == m_hoveredIndex);
                const Color fill = isHovered ? hoverColor : zoneColor;

                ctx.VG().FillRoundedRect(target.Rect, 4, fill);
                ctx.VG().StrokeRoundedRect(target.Rect, 4, zoneBorder, 1);

                // Draw directional arrow.
                const f32 cx = target.Rect.x + target.Rect.width * 0.5f;
                const f32 cy = target.Rect.y + target.Rect.height * 0.5f;
                const Color arrowColor = Rgb(255, 255, 255, isHovered ? 220 : 150);
                const f32 sz = 6.0f;

                ctx.VG().BeginPath();
                switch (target.Position)
                {
                case DockPosition::Top:
                    ctx.VG().MoveTo(cx - sz, cy + sz * 0.3f);
                    ctx.VG().LineTo(cx + sz, cy + sz * 0.3f);
                    ctx.VG().LineTo(cx, cy - sz * 0.5f);
                    break;
                case DockPosition::Bottom:
                    ctx.VG().MoveTo(cx - sz, cy - sz * 0.3f);
                    ctx.VG().LineTo(cx + sz, cy - sz * 0.3f);
                    ctx.VG().LineTo(cx, cy + sz * 0.5f);
                    break;
                case DockPosition::Left:
                    ctx.VG().MoveTo(cx + sz * 0.3f, cy - sz);
                    ctx.VG().LineTo(cx + sz * 0.3f, cy + sz);
                    ctx.VG().LineTo(cx - sz * 0.5f, cy);
                    break;
                case DockPosition::Right:
                    ctx.VG().MoveTo(cx - sz * 0.3f, cy - sz);
                    ctx.VG().LineTo(cx - sz * 0.3f, cy + sz);
                    ctx.VG().LineTo(cx + sz * 0.5f, cy);
                    break;
                case DockPosition::Center:
                    ctx.VG().FillRect(Rectangle{cx - sz, cy - sz, sz * 2, sz * 2}, arrowColor);
                    break;
                case DockPosition::Float:
                    break;
                }
                ctx.VG().ClosePath();
                ctx.VG().Fill(arrowColor);
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        Array<DockTarget> m_targets;
        i32 m_hoveredIndex = -1;
    };

    DRACONIC_DEFINE_OBJECT(DockZoneIndicator, "draconic::ui::toolkit")
}
