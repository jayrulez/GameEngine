// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RHI - the `foundation.rhi:depth` partition: the engine's depth convention expressed
// in RHI terms. core::projection says which way depth runs (reverse-Z: near = 1, far = 0); this
// says what that means for a pipeline: which compare function is "nearer", what a depth clear
// is, and which way a rasterizer bias pushes. Every depth-stencil state, sampler compare and
// clear in the engine names one of these rather than a literal, so the convention lives in one
// place (three, with the shader twin Data/Engine/Shaders/depth.hlsli).
export module foundation.rhi:depth;

import foundation.core;
import :enums;

using namespace foundation::core;

export namespace foundation::rhi::depth
{
    // Pass when the fragment is strictly nearer than the stored depth (a depth prepass, an
    // opaque pass with no prepass).
    [[nodiscard]] constexpr CompareFunction Nearer() noexcept
    {
        return projection::kReverseZ ? CompareFunction::Greater : CompareFunction::Less;
    }
    // Pass when the fragment is nearer than or at the stored depth: the forward pass after a prepass
    // (equal depth = the same surface), transparents and overlays against opaque depth, the sky
    // at the far plane against a cleared background, and a shadow sampler's "lit when the
    // receiver is at or nearer than the occluder".
    [[nodiscard]] constexpr CompareFunction NearerOrEqual() noexcept
    {
        return projection::kReverseZ ? CompareFunction::GreaterEqual : CompareFunction::LessEqual;
    }
    // Pass when the fragment is strictly farther (rare; the inverse of Nearer).
    [[nodiscard]] constexpr CompareFunction Farther() noexcept
    {
        return projection::kReverseZ ? CompareFunction::Less : CompareFunction::Greater;
    }

    // What a depth attachment clears to: the far plane, so the first fragment anywhere wins.
    [[nodiscard]] constexpr f32 ClearValue() noexcept { return projection::kNdcDepthFar; }

    // Rasterizer depth bias that pushes a fragment away from the viewer (a shadow caster away
    // from the light, so the receiver reads as nearer and acne goes). The hardware adds the bias
    // to the depth value, so under reverse-Z "away" is a smaller value: the sign flips.
    [[nodiscard]] constexpr i32 BiasAwayFromViewer(i32 units) noexcept
    {
        return projection::kReverseZ ? -units : units;
    }
    [[nodiscard]] constexpr f32 SlopeBiasAwayFromViewer(f32 scale) noexcept
    {
        return projection::kReverseZ ? -scale : scale;
    }
}
