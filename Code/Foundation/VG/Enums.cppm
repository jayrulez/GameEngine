// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :enums partition.
//
// Small enumerations for vector-graphics draw state.

module;
#include "Core/Prelude.h"

export module foundation.vg:enums;

import foundation.core;

export namespace foundation::vg
{
    using foundation::core::u8;

    /// Determines how the interior of a path is calculated.
    enum class FillRule
    {
        EvenOdd, ///< Inside if a ray crosses an odd number of path edges.
        NonZero, ///< Inside if the winding number is non-zero.
    };

    /// Blend mode for draw commands.
    enum class VGBlendMode
    {
        Normal,   ///< Standard alpha blending.
        Additive, ///< Additive blending.
        Multiply, ///< Multiply blending.
        Screen,   ///< Screen blending.
    };

    /// The values a target's blending works on.
    enum class VGBlendSpace
    {
        /// Linear light: authored colours are decoded and the target encodes on write, so half
        /// black over white is 73% grey. Right for panels that sit in a lit scene.
        Linear,
        /// sRGB-encoded values, as browsers and design tools blend: half black over white is
        /// 50% grey, and gradients and fades go as authored. The renderer draws into a UNORM
        /// view of the target (its sRGB twin's), and colours stay as entered.
        Encoded,
    };

    /// Clipping mode for draw commands.
    enum class VGClipMode
    {
        None,    ///< No clipping.
        Scissor, ///< Scissor rectangle clipping.
        Stencil, ///< Stencil-based path clipping.
    };

    /// Line cap style for stroke endpoints.
    enum class VGLineCap
    {
        Butt,   ///< Flat end at the exact endpoint.
        Round,  ///< Rounded end extending by half the stroke width.
        Square, ///< Square end extending by half the stroke width.
    };

    /// Line join style for stroke corners.
    enum class VGLineJoin
    {
        Miter, ///< Sharp corner (clamped by miter limit).
        Round, ///< Rounded corner.
        Bevel, ///< Beveled (flat cut) corner.
    };

    /// How a command's texels are shaded: straight sampling, or MSDF distance-field decode
    /// (crisp text at any scale). The renderer switches pipelines on this.
    enum class VGDrawMode
    {
        Default,        ///< Sample the texture / vertex color directly.
        DistanceField,  ///< Decode an MSDF atlas (median-of-3 + screen-space AA).
        GradientRadial, ///< Per-pixel radial gradient: t = length(texcoord); samples the ramp LUT.
        GradientConic,  ///< Per-pixel conic gradient: t = angle(texcoord)/2pi; samples the ramp LUT.
        /// A blurred rounded-rectangle (the UI box-shadow): the vertex texcoord carries the
        /// rounded-box SDF operand in blur-sigma units and coverage the corner radius (negative =
        /// inset); the shader integrates a Gaussian over the signed distance. One shader, no
        /// texture, any number of shadows per command.
        BoxShadow,
        /// A layer's composite (VGContext::PopLayer): a quad sampling the layer's offscreen target,
        /// which holds premultiplied colour, scaled by the quad's alpha (the layer's opacity). With
        /// no compositeLayer, the command's texture instead (VGContext::DrawImagePremultiplied).
        Layer,
        /// An analytic box (VGContext::DrawBox): the command's vertices are VGBatch::boxVertices
        /// (startIndex, indexCount count them, six to a box), drawn unindexed through the box
        /// pipeline; fill and border come from one signed distance in the fragment shader.
        Box,
    };

    /// Stencil-then-cover fill phases. Direct commands draw color immediately (the
    /// tessellated path). A complex fill (holes, self-intersection, even-odd) instead emits
    /// a StencilWrite command (contour fans, color-masked; the stencil accumulates winding by
    /// incr/decr-wrap for NonZero or invert for EvenOdd) followed by a StencilCover command
    /// (a bounding quad that draws color where the stencil is non-zero or odd and zeroes the
    /// stencil behind itself). Requires a host-provided stencil attachment; see
    /// VGContext::SetStencilFills and the VGRenderer target config.
    /// Stencil bit planes (fills and path clipping share one 8-bit stencil):
    /// bit 7 (0x80) is the clip mask, bits 0..6 accumulate fill winding. Fill covers
    /// zero only the winding bits; clipped covers restore the clip bit with Replace.
    enum class VGFillPhase : u8
    {
        Direct,       ///< Ordinary draw (no stencil interaction).
        StencilWrite, ///< Winding pass: color-masked fans accumulate into the stencil.
        StencilCover, ///< Cover pass: draw where stencil says inside, clearing it after.
        ClipApply,    ///< Convert accumulated clip winding to the 0x80 clip mask (color-masked).
        ClipClear,    ///< Zero the clip mask over the clip bounds (color-masked).
    };

    /// How a gradient maps parameters outside [0,1] (the SVG and CSS spread methods).
    /// Rendered through the LUT sampler's address mode: Pad clamps (the default), Repeat
    /// wraps, Reflect mirrors. Conic gradients wrap inherently and ignore this.
    enum class VGGradientSpread : u8
    {
        Pad,
        Repeat,
        Reflect,
    };

    /// How a gradient fill emits per-vertex data during tessellation (paired with the draw mode).
    /// Linear is exact as an affine per-vertex parameter; radial and conic need per-pixel coordinates
    /// so a dedicated shader can compute the (non-affine) parameter without Gouraud approximation.
    enum class VGGradientTess
    {
        Gouraud,     ///< Per-vertex fill.GetColorAt color, interpolated across triangles.
        LinearLut,   ///< texcoord = LUT u from the affine linear parameter; ramp sampled per pixel.
        RadialCoord, ///< texcoord = fill.GradientCoord (radial); the shader derives t per pixel.
        ConicCoord,  ///< texcoord = fill.GradientCoord (conic); the shader derives t per pixel.
    };
}
