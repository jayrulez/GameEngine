// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :gravity_helper partition
//
// Applies Gravity flags to position a child within a container. Ported from
// Sedulous.UI/src/Layout/GravityHelper.bf (Beef static class -> struct with a static method).

module;
#include "Core/Prelude.h"

export module foundation.ui:gravity_helper;

import foundation.core; // Rectangle, Max
import :thickness;
import :gravity;

using namespace foundation::core;

export namespace foundation::ui
{
    struct GravityHelper
    {
        /// Positions a child's MARGIN BOX of (boxW, boxH) inside a container of (containerW,
        /// containerH). Returns the margin-box (x, y, w, h) - View::Layout insets to the border
        /// box, so gravity math does not need the margin itself (a margin-aware form would
        /// produce identical border-box results).
        [[nodiscard]] static Rectangle Apply(Gravity gravity, f32 containerW, f32 containerH,
                                             f32 boxW, f32 boxH) noexcept
        {
            f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

            // Horizontal
            if (HasFlag(gravity, Gravity::FillH))
            {
                x = 0.0f;
                w = containerW;
            }
            else if (HasFlag(gravity, Gravity::Right))
            {
                x = containerW - boxW;
                w = boxW;
            }
            else if (HasFlag(gravity, Gravity::CenterH))
            {
                x = (containerW - boxW) * 0.5f;
                w = boxW;
            }
            else
            {
                x = 0.0f;
                w = boxW;
            } // Left or None

            // Vertical
            if (HasFlag(gravity, Gravity::FillV))
            {
                y = 0.0f;
                h = containerH;
            }
            else if (HasFlag(gravity, Gravity::Bottom))
            {
                y = containerH - boxH;
                h = boxH;
            }
            else if (HasFlag(gravity, Gravity::CenterV))
            {
                y = (containerH - boxH) * 0.5f;
                h = boxH;
            }
            else
            {
                y = 0.0f;
                h = boxH;
            } // Top or None

            return Rectangle{x, y, Max(0.0f, w), Max(0.0f, h)};
        }
    };
}
