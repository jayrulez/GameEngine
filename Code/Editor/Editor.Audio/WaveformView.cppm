// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :waveform_view partition.
//
// WaveformView: a clip's peak waveform, a widget of the editor's on the GUI (Documentation/Specs/
// editor-gui.md, 5.4, layer 5): symmetric bars per bucket about the middle line, and a playhead at
// a fraction of the clip while it plays. Its colours are the sheet's (`waveform` the tag, its
// background its own box, its parts `wave` and `playhead`), so it follows the theme.

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

        /// Each bucket's peak, 0 to 1, left to right; none draws only the background.
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

        void CollectStyleParts(Array<StringView>& out) const override
        {
            out.PushBack(StringView(u8"wave"));
            out.PushBack(StringView(u8"playhead"));
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
            const Color waveColor{0.25f, 0.78f, 0.75f, 0.9f}; // without a sheet
            for (usize i = 0; i < m_peaks.Size(); ++i)
            {
                const f32 half = Max(1.0f, m_peaks[i] * (bounds.height * 0.5f - 2.0f));
                wave.Draw(ctx, gui::Rect{bounds.x + static_cast<f32>(i) * bar, mid - half, Max(1.0f, bar - 1.0f), half * 2.0f}, gui::FilledBox(waveColor));
            }
            if (m_playhead >= 0.0f && m_playhead <= 1.0f)
                GetPartStyle(u8"playhead").Draw(ctx, gui::Rect{bounds.x + m_playhead * bounds.width - 1.0f, bounds.y, 2.0f, bounds.height},
                                                gui::FilledBox(Color{0.95f, 0.85f, 0.4f, 1.0f}));
        }

    private:
        Array<f32> m_peaks;
        f32 m_playhead = -1.0f;
    };
}
