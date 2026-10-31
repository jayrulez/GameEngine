// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :progress_bar partition
//
// ProgressBar: a display-only fill bar over a range. It is a RangeWidget (min, max, step and value;
// no page), and the fill covers the value's fraction of the range. An indeterminate bar, for work
// of unknown length, instead sweeps a segment along the track on the GUI's clock.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:progress_bar;

import foundation.core; // Color, Max, Min, MakeRef, Duration
import foundation.vg;   // CornerRadii
import :rect;
import :draw_context;
import :ui_widget;
import :actions; // TickAction, for the indeterminate sweep
import :range_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class ProgressBar : public RangeWidget
    {
        RTTI_OBJECT(ProgressBar, RangeWidget)
    public:
        ProgressBar() { SetTag(core::StringView(u8"progressbar")); }

        /// Marks the work as of unknown length, so a segment sweeps along the track.
        void SetIndeterminate(bool indeterminate)
        {
            if (indeterminate == m_indeterminate)
                return;
            m_indeterminate = indeterminate;
            m_phase = 0.0f;
            Invalidate();
        }
        [[nodiscard]] bool IsIndeterminate() const noexcept { return m_indeterminate; }
        /// Returns the sweep's position from 0 to 1; tests read it.
        [[nodiscard]] f32 GetSweepPhase() const noexcept { return m_phase; }

        void SetTrackColor(Color color)
        {
            m_trackColor = color;
            Invalidate();
        }
        void SetFillColor(Color color)
        {
            m_fillColor = color;
            Invalidate();
        }

        // Style parts: progressbar::track and ::fill.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"track"));
            out.PushBack(core::StringView(u8"fill"));
        }

        /// Returns the bar's length and thickness when nothing else sizes it.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{160.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const bool rtl = IsRightToLeft(); // the bar fills from the inline start
            const f32 radius = b.height * 0.5f;
            GetPartStyle(u8"track").Draw(ctx, b, FilledBox(m_trackColor, vg::CornerRadii(radius)));
            if (m_indeterminate)
            {
                Sweep();
                // A segment a third of the track long enters at the inline start and leaves at the
                // end, clipped to the track.
                const f32 segment = b.width / 3.0f;
                const f32 x = b.x - segment + (b.width + segment) * m_phase;
                const f32 left = core::Max(b.x, x);
                const f32 right = core::Min(b.x + b.width, x + segment);
                if (right > left)
                    GetPartStyle(u8"fill").Draw(ctx, MirroredIn(Rect{left, b.y, right - left, b.height}, b, rtl),
                                                FilledBox(m_fillColor, vg::CornerRadii(radius)));
                return;
            }
            const f32 ratio = static_cast<f32>(GetRatio());
            if (ratio > 0.0f)
                GetPartStyle(u8"fill").Draw(ctx, MirroredIn(Rect{b.x, b.y, b.width * ratio, b.height}, b, rtl),
                                            FilledBox(m_fillColor, vg::CornerRadii(radius)));
        }

    private:
        // Runs the sweep on the GUI's clock while the bar is indeterminate and shown.
        void Sweep()
        {
            if (m_sweeping || GetActionManager() == nullptr)
                return;
            m_sweeping = true;
            ProgressBar* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
            {
                if (!self->m_indeterminate)
                {
                    self->m_sweeping = false;
                    return false;
                }
                self->m_phase += static_cast<f32>(elapsed.AsSecondsF()) / kSweepSeconds;
                while (self->m_phase >= 1.0f)
                    self->m_phase -= 1.0f;
                self->Invalidate();
                return true;
            }));
        }

        bool m_indeterminate = false;
        bool m_sweeping = false;
        f32 m_phase = 0.0f;
        Color m_trackColor{0.24f, 0.26f, 0.30f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
        static constexpr f32 kSweepSeconds = 1.4f;
    };
}
