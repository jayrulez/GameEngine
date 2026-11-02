// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :waveform_view partition.
//
// WaveformView: a clip's peak waveform, an editor widget on the GUI. It draws symmetric bars per
// bucket about the middle line, and a playhead at a fraction of the clip while it plays. It can
// draw the clip as an edit would make it: its bars scaled by a gain (clamped to full scale), and
// the part from a fraction on shaded as cut. Its colours come from the style sheet (tag `waveform`,
// its own box for the background, and parts `wave`, `playhead` and `cut`), so it follows the theme.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.audio:waveform_view;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor
{
    namespace gui = foundation::gui;

    class WaveformView final : public gui::UIWidget
    {
        RTTI_OBJECT(WaveformView, gui::UIWidget)
    public:
        WaveformView()
        {
            SetTag(StringView(u8"waveform"));
            AddClass(u8"waveform");
        }

        /// Each bucket's peak, 0 to 1, left to right; with no peaks, only the background draws.
        void SetPeaks(Array<f32> peaks)
        {
            m_peaks = foundation::core::Move(peaks); // not the widget's own Move
            Invalidate();
        }
        [[nodiscard]] Span<const f32> Peaks() const noexcept { return Span<const f32>(m_peaks.Data(), m_peaks.Size()); }
        /// Where the playhead is along the clip, 0 to 1; below 0 hides it (stopped).
        void SetPlayhead(f32 fraction)
        {
            if (fraction == m_playhead)
                return;
            m_playhead = fraction;
            Invalidate();
        }
        [[nodiscard]] f32 Playhead() const noexcept { return m_playhead; }
        /// Draws each bar times `gain` (1: as it is), none taller than full scale.
        void SetGain(f32 gain)
        {
            if (gain == m_gain)
                return;
            m_gain = gain;
            Invalidate();
        }
        [[nodiscard]] f32 Gain() const noexcept { return m_gain; }
        /// Shades the clip from `fraction` (0 to 1) to its end as cut; 1 or more shades nothing.
        void SetCutFrom(f32 fraction)
        {
            if (fraction == m_cutFrom)
                return;
            m_cutFrom = fraction;
            Invalidate();
        }
        [[nodiscard]] f32 CutFrom() const noexcept { return m_cutFrom; }

        void CollectStyleParts(Array<StringView>& out) const override
        {
            out.PushBack(StringView(u8"wave"));
            out.PushBack(StringView(u8"playhead"));
            out.PushBack(StringView(u8"cut"));
        }
        [[nodiscard]] Float2 MeasureContent(Float2) override { return Float2{240.0f, 120.0f}; }

    protected:
        void OnDraw(gui::DrawContext& ctx, const gui::Rect& bounds) override
        {
            if (m_peaks.IsEmpty() || bounds.width <= 2.0f || bounds.height <= 2.0f)
                return;
            const f32 mid = bounds.y + bounds.height * 0.5f;
            const f32 bar = bounds.width / static_cast<f32>(m_peaks.Size());
            const gui::PartStyle& wave = GetPartStyle(u8"wave");
            const Color waveColor{0.25f, 0.78f, 0.75f, 0.9f}; // used when no sheet sets the colour
            for (usize i = 0; i < m_peaks.Size(); ++i)
            {
                const f32 half = Max(1.0f, Min(m_peaks[i] * m_gain, 1.0f) * (bounds.height * 0.5f - 2.0f));
                wave.Draw(ctx, gui::Rect{bounds.x + static_cast<f32>(i) * bar, mid - half, Max(1.0f, bar - 1.0f), half * 2.0f}, gui::FilledBox(waveColor));
            }
            if (m_cutFrom < 1.0f)
            {
                const f32 from = bounds.x + Max(m_cutFrom, 0.0f) * bounds.width;
                GetPartStyle(u8"cut").Draw(ctx, gui::Rect{from, bounds.y, bounds.x + bounds.width - from, bounds.height},
                                           gui::FilledBox(Color{0.0f, 0.0f, 0.0f, 0.45f}));
            }
            if (m_playhead >= 0.0f && m_playhead <= 1.0f)
                GetPartStyle(u8"playhead").Draw(ctx, gui::Rect{bounds.x + m_playhead * bounds.width - 1.0f, bounds.y, 2.0f, bounds.height},
                                                gui::FilledBox(Color{0.95f, 0.85f, 0.4f, 1.0f}));
        }

    private:
        Array<f32> m_peaks;
        f32 m_playhead = -1.0f;
        f32 m_gain = 1.0f;
        f32 m_cutFrom = 1.0f;
    };
}
