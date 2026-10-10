// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :zoom_readout partition.
//
// The scene view's measurement overlay while zooming: a wheel dolly shows, for a moment, how far
// the camera is from its focus, what a grid cell measures, and a scale bar of a round length at
// the focus, then fades once the zoom stops. The pure parts live here (when it shows, the bar's
// length, the words for a length) so they are testable without a viewport; the page draws them.

module;
#include "Core/Prelude.h"

export module editor.scene:zoom_readout;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    /// A round length at the focus and the pixels it spans on screen.
    struct ScaleBar
    {
        f32 metres = 0.0f;
        f32 pixels = 0.0f;
    };

    /// The longest round length (1, 2 or 5 times a power of ten) that spans at most
    /// `targetPixels` at `distance` in front of a perspective camera of vertical field of view
    /// `fovY` (radians) drawn `viewportHeight` pixels tall. Zero for a degenerate view.
    [[nodiscard]] inline ScaleBar ScaleBarAt(f32 distance, f32 fovY, f32 viewportHeight,
                                             f32 targetPixels = 120.0f)
    {
        if (distance <= 0.0f || fovY <= 0.0f || viewportHeight <= 0.0f || targetPixels <= 0.0f)
        {
            return {};
        }
        // The height of the view at the focus, spread over the viewport's pixels.
        const f32 metresPerPixel = 2.0f * distance * Tan(0.5f * fovY) / viewportHeight;
        const f32 target = targetPixels * metresPerPixel;
        f32 decade = 1.0f;
        while (decade * 10.0f <= target)
        {
            decade *= 10.0f;
        }
        while (decade > target)
        {
            decade *= 0.1f;
        }
        f32 metres = decade;
        if (decade * 5.0f <= target)
        {
            metres = decade * 5.0f;
        }
        else if (decade * 2.0f <= target)
        {
            metres = decade * 2.0f;
        }
        return ScaleBar{metres, metres / metresPerPixel};
    }

    /// A length in words: millimetres and centimetres below a metre, metres (one decimal below
    /// ten) below a kilometre, kilometres (one decimal) above. "5 cm", "2.5 m", "12 m", "1.2 km".
    [[nodiscard]] inline String FormatMetres(f32 metres)
    {
        if (metres < 0.01f)
        {
            return Format(u8"{} mm", static_cast<i64>(metres * 1000.0f + 0.5f));
        }
        if (metres < 1.0f)
        {
            return Format(u8"{} cm", static_cast<i64>(metres * 100.0f + 0.5f));
        }
        const auto oneDecimal = [](f32 value, StringView unit)
        {
            const i64 tenths = static_cast<i64>(value * 10.0f + 0.5f);
            return tenths % 10 == 0 ? Format(u8"{} {}", tenths / 10, unit)
                                    : Format(u8"{}.{} {}", tenths / 10, tenths % 10, unit);
        };
        if (metres < 10.0f)
        {
            return oneDecimal(metres, u8"m");
        }
        if (metres < 1000.0f)
        {
            return Format(u8"{} m", static_cast<i64>(metres + 0.5f));
        }
        return oneDecimal(metres * 0.001f, u8"km");
    }

    /// When the readout shows: fully for a moment after the last zoom, then fading out.
    class ZoomReadout
    {
    public:
        static constexpr f32 kHoldSeconds = 1.0f; // shown whole after the last notch
        static constexpr f32 kFadeSeconds = 0.5f; // then gone over this long

        /// A zoom happened this frame: show it (again) from the start.
        void Zoomed() noexcept
        {
            m_shown = true;
            m_sinceZoom = 0.0f;
        }

        void Advance(f32 dt) noexcept
        {
            if (m_shown)
            {
                m_sinceZoom += dt;
                m_shown = m_sinceZoom < kHoldSeconds + kFadeSeconds;
            }
        }

        /// 1 while held, falling to 0 over the fade, 0 when hidden.
        [[nodiscard]] f32 Opacity() const noexcept
        {
            if (!m_shown)
            {
                return 0.0f;
            }
            if (m_sinceZoom <= kHoldSeconds)
            {
                return 1.0f;
            }
            return Clamp(1.0f - (m_sinceZoom - kHoldSeconds) / kFadeSeconds, 0.0f, 1.0f);
        }

    private:
        bool m_shown = false;
        f32 m_sinceZoom = 0.0f;
    };
}
