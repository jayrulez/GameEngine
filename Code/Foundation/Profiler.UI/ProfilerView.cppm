// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :view partition.
///
/// What every profiler view shares: the source it reads (the live store or a capture), how many
/// of the newest frames it looks at, the palette, and text drawn when the context has fonts (a
/// headless test's has none, and the view draws its shapes alone). A view computes what it shows
/// in Refresh, from the source, and draws that; tests read the same model.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:view;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;
import foundation.profiler;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::profiler::ui
{
    /// The colours every view draws with: the CPU in blue, the GPU in amber, on a dark panel.
    struct ProfilerPalette
    {
        Color panel{0.05f, 0.06f, 0.08f, 0.82f};
        Color plot{1.0f, 1.0f, 1.0f, 0.05f};
        Color text{0.94f, 0.95f, 0.97f, 1.0f};
        Color dim{0.66f, 0.69f, 0.74f, 1.0f};
        Color cpu{0.36f, 0.70f, 1.0f, 1.0f};
        Color gpu{1.0f, 0.64f, 0.26f, 1.0f};
        Color budget{1.0f, 1.0f, 1.0f, 0.28f};
        Color selection{0.36f, 0.70f, 1.0f, 0.18f};
        Color highlight{1.0f, 1.0f, 1.0f, 0.9f};
        Color grid{1.0f, 1.0f, 1.0f, 0.10f};
    };

    /// A source with no frames: what a view reads before it is given one.
    class EmptyProfileSource final : public IProfileSource
    {
    public:
        u64 TickFrequency() const noexcept override { return 1; }
        bool HasFrames() const noexcept override { return false; }
        u64 OldestFrame() const noexcept override { return 0; }
        u64 NewestFrame() const noexcept override { return 0; }
        const FrameRecord* Frame(u64) const noexcept override { return nullptr; }
        StringView NameOf(NameId) const noexcept override { return {}; }
        u32 NameCount() const noexcept override { return 0; }
        Span<const ThreadInfo> Threads() const noexcept override { return {}; }
        Span<const CounterInfo> Counters() const noexcept override { return {}; }
    };

    class ProfilerView : public foundation::ui::View
    {
    public:
        static constexpr f32 kFontSize = 11.0f;
        static constexpr f32 kLineHeight = 15.0f;

        /// What the view reads; null shows nothing. Borrowed: the source outlives the view.
        void SetSource(const IProfileSource* source) noexcept
        {
            m_source = source;
            Refresh();
        }
        [[nodiscard]] const IProfileSource* Source() const noexcept { return m_source; }

        /// How many of the newest frames it looks at.
        void SetFrameCount(u32 count) noexcept
        {
            m_frameCount = Max<u32>(count, 1);
            Refresh();
        }
        [[nodiscard]] u32 FrameCount() const noexcept { return m_frameCount; }

        /// Look at exactly these frames (the panel's selection) instead of the newest FrameCount;
        /// an empty range goes back to the newest.
        void SetRange(FrameRange range)
        {
            m_range = range;
            Refresh();
        }

        /// Recompute what it shows from the source (a new frame arrived) and ask for a draw.
        void Refresh()
        {
            Compute();
            InvalidateVisual();
        }

        ProfilerPalette palette;

    protected:
        explicit ProfilerView(IAllocator& allocator) noexcept : m_allocator(&allocator)
        {
            IsHitTestVisible = false; // it shows, never takes the pointer from the game
        }

        /// Fill the view's model from the source; called with a null source too.
        virtual void Compute() = 0;

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        /// The source, or one with no frames, so a view reads the same either way.
        [[nodiscard]] const IProfileSource& SourceOrEmpty() const noexcept
        {
            static const EmptyProfileSource kEmpty;
            return m_source != nullptr ? *m_source : kEmpty;
        }

        [[nodiscard]] FrameRange Range() const noexcept
        {
            if (!m_range.empty)
            {
                return m_range;
            }
            return m_source != nullptr ? LastFrames(*m_source, m_frameCount) : FrameRange{};
        }

        static void DrawLabel(foundation::ui::UIDrawContext& ctx, StringView text, Rectangle bounds, Color color,
                              fonts::TextAlignment align = fonts::TextAlignment::Left)
        {
            if (ctx.FontService() == nullptr || text.IsEmpty())
            {
                return;
            }
            if (fonts::CachedFont* font = ctx.FontService()->GetFont(kFontSize))
            {
                ctx.VG().DrawText(text, font, bounds, align, fonts::VerticalAlignment::Middle, color);
            }
        }

        const IProfileSource* m_source = nullptr;
        u32 m_frameCount = 120;
        FrameRange m_range; // set: exactly these frames

    private:
        IAllocator* m_allocator;
    };
}
