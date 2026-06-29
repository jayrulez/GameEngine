// Raptor::VG — :enums partition.
//
// Small enumerations for vector-graphics draw state. Ported from
// Sedulous.VG (FillRule/VGBlendMode/VGClipMode/VGLineCap/VGLineJoin).

module;
#include "Core/Prelude.h"

export module raptor.vg:enums;

export namespace raptor::vg
{
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

    /// Selects which rendering pipeline a command uses (internal to VG).
    enum class VGDrawMode
    {
        Default,        ///< Standard alpha-coverage path.
        DistanceField,  ///< Multi-channel signed distance field.
    };

    /// Line join style for stroke corners.
    enum class VGLineJoin
    {
        Miter, ///< Sharp corner (clamped by miter limit).
        Round, ///< Rounded corner.
        Bevel, ///< Beveled (flat cut) corner.
    };
}
