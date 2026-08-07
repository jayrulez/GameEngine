// Draconic::VG - :enums partition.
//
// Small enumerations for vector-graphics draw state. Ported from
// Sedulous.VG (FillRule/VGBlendMode/VGClipMode/VGLineCap/VGLineJoin).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:enums;

import draconic.foundation;

export namespace draconic::vg
{
    using draconic::foundation::u8;

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
    };

    /// Stencil-then-cover fill phases. Direct commands draw color immediately (the
    /// tessellated path). A complex fill (holes, self-intersection, even-odd) instead emits
    /// a StencilWrite command (contour fans, COLOR MASKED, stencil accumulates winding via
    /// incr/decr-wrap for NonZero or invert for EvenOdd) followed by a StencilCover command
    /// (a bounding quad that draws color where the stencil is non-zero/odd and zeroes the
    /// stencil behind itself). Requires a host-provided stencil attachment - see
    /// VGContext::SetStencilFills / VGRenderer target config.
    /// Stencil bit-plane contract (fills + path clipping share ONE 8-bit stencil):
    /// bit 7 (0x80) is the CLIP mask, bits 0..6 accumulate fill winding. Fill covers
    /// zero only the winding bits; clipped covers RESTORE the clip bit via Replace.
    enum class VGFillPhase : u8
    {
        Direct,       ///< Ordinary draw (no stencil interaction).
        StencilWrite, ///< Winding pass: color-masked fans accumulate into the stencil.
        StencilCover, ///< Cover pass: draw where stencil says inside, clearing it after.
        ClipApply,    ///< Convert accumulated clip winding to the 0x80 clip mask (color-masked).
        ClipClear,    ///< Zero the clip mask over the clip bounds (color-masked).
    };

    /// How a gradient maps parameters outside [0,1] (the SVG/CSS spread methods).
    /// Rendered via the LUT sampler's address mode: Pad = clamp (default), Repeat =
    /// wrap, Reflect = mirror. Conic gradients wrap inherently and ignore this.
    enum class VGGradientSpread : u8
    {
        Pad,
        Repeat,
        Reflect,
    };

    /// How a gradient fill emits per-vertex data during tessellation (paired with the draw mode).
    /// Linear is exact as an affine per-vertex parameter; radial/conic need per-pixel coordinates
    /// so a dedicated shader can compute the (non-affine) parameter without Gouraud approximation.
    enum class VGGradientTess
    {
        Gouraud,     ///< Legacy: per-vertex fill.GetColorAt color, interpolated across triangles.
        LinearLut,   ///< texcoord = LUT u from the affine linear parameter; ramp sampled per pixel.
        RadialCoord, ///< texcoord = fill.GradientCoord (radial); the shader derives t per pixel.
        ConicCoord,  ///< texcoord = fill.GradientCoord (conic); the shader derives t per pixel.
    };
}
