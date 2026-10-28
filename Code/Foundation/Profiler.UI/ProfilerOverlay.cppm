// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :overlay partition.
///
/// The in-game overlay (Specs/profiler.md, P4): the frame time graph, the frame statistics, the
/// top CPU scopes and GPU passes and the counters, stacked on one translucent rounded panel in a
/// corner of whatever it fills. It never takes the pointer, so the game under it plays on. The
/// host shows it, arms the profiler while it shows, and refreshes it each frame.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:overlay;

import foundation.core;
import foundation.vg;
import foundation.ui;
import foundation.profiler;
import :view;
import :frame_time_graph;
import :frame_stats;
import :top_list;
import :counter_strip;

using namespace foundation::core;

export namespace foundation::profiler::ui
{
    enum class OverlayCorner : u8
    {
        TopRight,
        TopLeft,
        BottomRight,
        BottomLeft,
    };

    class ProfilerOverlay final : public foundation::ui::ViewGroup
    {
    public:
        static constexpr f32 kPanelWidth = 360.0f;
        static constexpr f32 kPadding = 8.0f;
        static constexpr f32 kGap = 6.0f;
        static constexpr f32 kMargin = 8.0f;

        explicit ProfilerOverlay(IAllocator& allocator) : m_views(allocator)
        {
            IsHitTestVisible = false;
            m_graph = MakeRef<FrameTimeGraph>(allocator, allocator);
            m_stats = MakeRef<FrameStats>(allocator, allocator);
            m_scopes = MakeRef<TopList>(allocator, allocator, TopLane::Scopes, 6);
            m_passes = MakeRef<TopList>(allocator, allocator, TopLane::GpuPasses, 5);
            m_counters = MakeRef<CounterStrip>(allocator, allocator);
            ProfilerView* views[] = {m_graph.Get(), m_stats.Get(), m_scopes.Get(), m_passes.Get(), m_counters.Get()};
            for (ProfilerView* view : views)
            {
                m_views.PushBack(view);
                AddView(view);
            }
        }

        /// What every view reads (borrowed; null shows nothing).
        void SetSource(const IProfileSource* source)
        {
            for (ProfilerView* view : m_views)
            {
                view->SetSource(source);
            }
        }

        /// A new frame: every view recomputes from the source.
        void Refresh()
        {
            for (ProfilerView* view : m_views)
            {
                view->Refresh();
            }
        }

        void SetCorner(OverlayCorner corner)
        {
            m_corner = corner;
            Invalidate();
        }
        [[nodiscard]] OverlayCorner Corner() const noexcept { return m_corner; }

        /// The panel's bounds in the overlay, from the last layout.
        [[nodiscard]] Rectangle Panel() const noexcept { return m_panel; }

        [[nodiscard]] FrameTimeGraph& Graph() noexcept { return *m_graph; }
        [[nodiscard]] FrameStats& Stats() noexcept { return *m_stats; }
        [[nodiscard]] TopList& Scopes() noexcept { return *m_scopes; }
        [[nodiscard]] TopList& Passes() noexcept { return *m_passes; }
        [[nodiscard]] CounterStrip& Counters() noexcept { return *m_counters; }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            ctx.VG().FillRoundedRect(m_panel, 6.0f, m_graph->palette.panel);
            DrawChildren(ctx);
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            // It fills what it is given; the panel inside is its views' height.
            const f32 inner = kPanelWidth - 2 * kPadding;
            for (ProfilerView* view : m_views)
            {
                view->Measure(foundation::ui::BoxConstraints{inner, inner, 0.0f, kFloatMax}); // the panel's width
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth < kFloatMax ? constraints.MaxWidth : kPanelWidth),
                                  constraints.ConstrainHeight(constraints.MaxHeight < kFloatMax ? constraints.MaxHeight : PanelHeight())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 panelWidth = Min(kPanelWidth, Max(width - 2 * kMargin, 0.0f));
            const f32 panelHeight = PanelHeight();
            const bool right = m_corner == OverlayCorner::TopRight || m_corner == OverlayCorner::BottomRight;
            const bool bottom = m_corner == OverlayCorner::BottomRight || m_corner == OverlayCorner::BottomLeft;
            m_panel = Rectangle{right ? Max(width - kMargin - panelWidth, 0.0f) : kMargin,
                                bottom ? Max(height - kMargin - panelHeight, 0.0f) : kMargin, panelWidth, panelHeight};
            f32 y = m_panel.y + kPadding;
            for (ProfilerView* view : m_views)
            {
                view->Layout(m_panel.x + kPadding, y, panelWidth - 2 * kPadding, view->MeasuredSize.y);
                y += view->MeasuredSize.y + kGap;
            }
        }

    private:
        [[nodiscard]] f32 PanelHeight() const noexcept
        {
            f32 height = 2 * kPadding;
            for (const ProfilerView* view : m_views)
            {
                height += view->MeasuredSize.y;
            }
            return height + kGap * static_cast<f32>(m_views.Size() - 1);
        }

        RefPtr<FrameTimeGraph> m_graph;
        RefPtr<FrameStats> m_stats;
        RefPtr<TopList> m_scopes;
        RefPtr<TopList> m_passes;
        RefPtr<CounterStrip> m_counters;
        Array<ProfilerView*> m_views; // the five above, in panel order
        OverlayCorner m_corner = OverlayCorner::TopRight;
        Rectangle m_panel{};
    };
}
