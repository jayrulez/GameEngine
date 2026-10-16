// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scroll_bar partition
//
// ScrollBar: a draggable indicator of a scroll position in [0,1], with a thumb whose length
// reflects the visible proportion (viewport / content). Modeled on eepp's UIScrollBar (role,
// not a line-for-line port). Horizontal or vertical. Reuses the Slider's drag discipline: an
// own m_dragging flag (set in OnMouseDown, cleared in an OnMouseUp override, gated in
// OnMouseMove) so a captured drag that leaves the widget - firing OnMouseLeave, which clears
// m_pressed - keeps tracking. Clicking the track jumps the thumb to the cursor.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:scroll_bar;

import foundation.core; // Color, Function, Move, Max, Min, Float2, Rectangle, RefPtr, MakeRef
import foundation.vg;   // CornerRadii
import :rect;
import :event;
import :draw_context;
import :rectangle_drawable;
import :ui_widget;
import :linear_layout; // Orientation

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class ScrollBar : public UIWidget
    {
        RTTI_OBJECT(ScrollBar, UIWidget)
    public:
        ScrollBar()
        {
            SetTag(core::StringView(u8"scrollbar"));
            // The track is the node background so the theme's background-color styles it.
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_trackColor));
        }

        void SetOrientation(Orientation orientation)
        {
            m_orientation = orientation;
            Invalidate();
        }
        [[nodiscard]] Orientation GetOrientation() const noexcept { return m_orientation; }

        [[nodiscard]] f32 GetValue() const noexcept { return m_value; }
        void SetValue(f32 value)
        {
            value = core::Max(0.0f, core::Min(1.0f, value));
            if (value == m_value)
                return;
            m_value = value;
            Invalidate();
            if (m_onChanged)
                m_onChanged(m_value);
        }
        void SetOnValueChanged(core::Function<void(f32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        // Fraction of the track the thumb fills = viewport / content (clamped to [0,1]).
        void SetThumbProportion(f32 proportion)
        {
            proportion = core::Max(0.0f, core::Min(1.0f, proportion));
            if (proportion == m_proportion)
                return;
            m_proportion = proportion;
            Invalidate();
        }
        [[nodiscard]] f32 GetThumbProportion() const noexcept { return m_proportion; }

        void SetTrackColor(Color color)
        {
            m_trackColor = color;
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), color));
        }
        void SetThumbColor(Color color)
        {
            m_thumbColor = color;
            Invalidate();
        }

        // Theming: track = background-color (node background); thumb = scrollbar::thumb part.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"thumb"));
        }
        void SetThemePartColor(core::StringView part, Color color) override
        {
            if (part == core::StringView(u8"thumb"))
                SetThumbColor(color);
        }

    protected:
        // Press on the thumb starts a grab-drag (the thumb keeps its offset under the cursor,
        // so it doesn't jump); press on the track above/below the thumb pages toward the click
        // by one visible proportion (eepp-style), no drag.
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left)
                return; // only the left button drags/pages
            event.Handled = true;
            const Rect b = GetContentBounds();
            const f32 track = TrackLength(b);
            const f32 thumbLen = ThumbLength(track);
            const f32 travel = core::Max(0.0f, track - thumbLen);
            const f32 thumbStart = travel * m_value;
            const f32 along = AlongAxis(event, b);

            if (along >= thumbStart && along <= thumbStart + thumbLen)
            {
                m_dragging = true;
                m_grabOffset = along - thumbStart; // keep the grab point fixed under the cursor
            }
            else
            {
                const f32 page = (m_proportion > 0.0f) ? m_proportion : 0.1f;
                SetValue((along < thumbStart) ? (m_value - page) : (m_value + page));
            }
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_dragging)
                return;
            const Rect b = GetContentBounds();
            const f32 track = TrackLength(b);
            const f32 thumbLen = ThumbLength(track);
            const f32 travel = core::Max(0.0f, track - thumbLen);
            if (travel <= 0.0f)
            {
                SetValue(0.0f);
                return;
            }
            SetValue((AlongAxis(event, b) - m_grabOffset) / travel);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            m_dragging = false;
        }
        void OnPointerCancel(const MouseEvent&) override { m_dragging = false; }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const f32 track = TrackLength(b);
            const f32 thumbLen = ThumbLength(track);
            const f32 travel = core::Max(0.0f, track - thumbLen);
            const f32 pos = travel * m_value;

            // The track is painted by the node background (themable via background-color); we
            // draw only the thumb here.
            const Rect thumb = (m_orientation == Orientation::Vertical)
                                   ? Rect{b.x, b.y + pos, b.width, thumbLen}
                                   : Rect{b.x + pos, b.y, thumbLen, b.height};
            ctx.VG().FillRoundedRect(thumb.ToRectangle(), Radii(thumb), m_thumbColor);
        }

    private:
        [[nodiscard]] f32 TrackLength(const Rect& b) const
        {
            return (m_orientation == Orientation::Vertical) ? b.height : b.width;
        }
        [[nodiscard]] f32 ThumbLength(f32 track) const
        {
            return core::Max(kMinThumb, track * m_proportion);
        }
        [[nodiscard]] static vg::CornerRadii Radii(const Rect& r)
        {
            return vg::CornerRadii(core::Min(r.width, r.height) * 0.5f);
        }

        // Cursor position along the bar's axis, relative to the track start.
        [[nodiscard]] f32 AlongAxis(const MouseEvent& event, const Rect& b) const
        {
            const core::Float2 local = ConvertToNodeSpace(event.Position);
            return (m_orientation == Orientation::Vertical) ? (local.y - b.y) : (local.x - b.x);
        }

        Orientation m_orientation = Orientation::Vertical;
        f32 m_value = 0.0f;
        f32 m_proportion = 0.3f;
        bool m_dragging = false;
        f32 m_grabOffset = 0.0f;
        Color m_trackColor{0.18f, 0.19f, 0.23f, 1.0f};
        Color m_thumbColor{0.42f, 0.45f, 0.52f, 1.0f};
        core::Function<void(f32)> m_onChanged;

        static constexpr f32 kMinThumb = 16.0f;
    };

    RTTI_DEFINE_OBJECT(ScrollBar, "rtti::gui")
}
