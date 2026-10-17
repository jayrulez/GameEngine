// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :progress_bar partition
//
// ProgressBar: a display-only [0,1] fill bar (track + fill). A lean native control
// modeled on eepp's UIProgressBar (role only). No interaction; drive it with SetProgress.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:progress_bar;

import foundation.core; // Color, Max, Min
import foundation.vg;   // CornerRadii
import :reactive; // Observable (what bindings follow)
import :rect;
import :draw_context;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class ProgressBar : public UIWidget
    {
        RTTI_OBJECT(ProgressBar, UIWidget)
    public:
        ProgressBar() { SetTag(core::StringView(u8"progressbar")); }

        /// The progress (a read a binding records).
        [[nodiscard]] f32 GetProgress() const
        {
            m_progressChanged.Track();
            return m_progress;
        }
        void SetProgress(f32 progress)
        {
            progress = core::Max(0.0f, core::Min(1.0f, progress));
            if (progress == m_progress)
                return;
            m_progress = progress;
            Invalidate();
            m_progressChanged.Notify();
        }

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

        // Theming parts: progressbar::track / ::fill.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"track"));
            out.PushBack(core::StringView(u8"fill"));
        }

        /// A bar's length and thickness when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{160.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const f32 radius = b.height * 0.5f;
            GetPartStyle(u8"track").Draw(ctx, b, FilledBox(m_trackColor, vg::CornerRadii(radius)));
            if (m_progress > 0.0f)
                GetPartStyle(u8"fill").Draw(ctx, Rect{b.x, b.y, b.width * m_progress, b.height}, FilledBox(m_fillColor, vg::CornerRadii(radius)));
        }

        f32 m_progress = 0.0f;
        Observable m_progressChanged; // what bindings reading the progress follow
        Color m_trackColor{0.24f, 0.26f, 0.30f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
    };
}
