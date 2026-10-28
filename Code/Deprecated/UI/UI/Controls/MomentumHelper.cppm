// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :momentum_helper partition
//
// Physics-based kinetic scrolling helper. A plain value struct embedded in ScrollView: call Update()
// each frame and apply the returned displacement to the scroll offset. Ported from Sedulous.UI/src/
// Controls/MomentumHelper.bf. Self-contained (no View). Beef's `mut` methods -> plain non-const methods;
// the `(float dx, float dy)` tuple return -> Float2.

module;
#include "Core/Prelude.h"

export module foundation.ui:momentum_helper;

import foundation.core; // Float2, Abs, Min

using namespace foundation::core;

export namespace foundation::ui
{
    struct MomentumHelper
    {
        /// Current velocity in pixels/second.
        f32 VelocityX = 0.0f;
        f32 VelocityY = 0.0f;

        /// Friction coefficient (higher = faster deceleration).
        f32 Friction = 6.0f;

        /// Velocity below this is snapped to zero.
        f32 StopThreshold = 0.5f;

        /// Whether momentum is active.
        [[nodiscard]] bool IsActive() const noexcept
        {
            return Abs(VelocityX) > StopThreshold || Abs(VelocityY) > StopThreshold;
        }

        /// Advance physics by deltaTime. Returns the displacement (dx, dy) to apply to the scroll offset.
        Float2 Update(f32 deltaTime)
        {
            if (!IsActive())
            {
                return Float2{0.0f, 0.0f};
            }

            const f32 decay = 1.0f - Min(Friction * deltaTime, 1.0f);
            const f32 dx = VelocityX * deltaTime;
            const f32 dy = VelocityY * deltaTime;

            VelocityX *= decay;
            VelocityY *= decay;

            if (Abs(VelocityX) < StopThreshold)
            {
                VelocityX = 0.0f;
            }
            if (Abs(VelocityY) < StopThreshold)
            {
                VelocityY = 0.0f;
            }

            return Float2{dx, dy};
        }

        /// Stop all momentum immediately.
        void Stop() noexcept
        {
            VelocityX = 0.0f;
            VelocityY = 0.0f;
        }
    };
}
