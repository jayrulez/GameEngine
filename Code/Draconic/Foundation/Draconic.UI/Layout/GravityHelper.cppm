// Draconic UI - :gravity_helper partition
//
// Applies Gravity flags to position a child within a container. Ported from
// Sedulous.UI/src/Layout/GravityHelper.bf (Beef static class -> struct with a static method).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:gravity_helper;

import draconic.foundation; // Rectangle, Max
import :thickness;
import :gravity;

using namespace draconic::foundation;

export namespace draconic::ui
{
    struct GravityHelper
    {
        /// Positions a child of (childW, childH) inside a container of (containerW, containerH) with the
        /// given margin. Returns the child's (x, y, w, h).
        [[nodiscard]] static Rectangle Apply(Gravity gravity, f32 containerW, f32 containerH,
                                             f32 childW, f32 childH, Thickness margin) noexcept
        {
            const f32 availW = containerW - margin.Left - margin.Right;
            const f32 availH = containerH - margin.Top - margin.Bottom;
            f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

            // Horizontal
            if (HasFlag(gravity, Gravity::FillH))
            {
                x = margin.Left;
                w = availW;
            }
            else if (HasFlag(gravity, Gravity::Right))
            {
                x = containerW - margin.Right - childW;
                w = childW;
            }
            else if (HasFlag(gravity, Gravity::CenterH))
            {
                x = margin.Left + (availW - childW) * 0.5f;
                w = childW;
            }
            else
            {
                x = margin.Left;
                w = childW;
            } // Left or None

            // Vertical
            if (HasFlag(gravity, Gravity::FillV))
            {
                y = margin.Top;
                h = availH;
            }
            else if (HasFlag(gravity, Gravity::Bottom))
            {
                y = containerH - margin.Bottom - childH;
                h = childH;
            }
            else if (HasFlag(gravity, Gravity::CenterV))
            {
                y = margin.Top + (availH - childH) * 0.5f;
                h = childH;
            }
            else
            {
                y = margin.Top;
                h = childH;
            } // Top or None

            return Rectangle{x, y, Max(0.0f, w), Max(0.0f, h)};
        }
    };
}
