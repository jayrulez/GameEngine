// Draconic::VG - :vertex partition.
//
// VGVertex: the GPU vertex for vector graphics with analytical-AA coverage.
// Ported from Sedulous.VG/VGVertex.bf. Color is stored as a full float Color
// (RGBA). The renderer's GPU color attribute is Float32x4 (see VGRenderVertex),
// so packing to a byte Color32 here only quantized colors to 8-bit for no GPU
// benefit - it re-expanded to float immediately, and the quantization showed up
// as visible banding across gradients and AA fringes. Keeping float precision
// end to end removes that banding (see [[vg-quality-track]]).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:vertex;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::vg
{
    /// Vertex structure for vector graphics with analytical AA support.
    struct VGVertex
    {
        Float2 position;     ///< Position in screen/world coordinates.
        Float2 texCoord;     ///< Texture coordinates (UV).
        Color color;         ///< Vertex color in full float RGBA (no 8-bit quantization).
        f32 coverage = 1.0f; ///< Analytical-AA coverage (0 = transparent fringe, 1 = opaque).

        /// Size in bytes of this vertex structure.
        static constexpr i32 SizeInBytes = 36; // 8 + 8 + 16 + 4

        /// Fixed UV for solid-color drawing.
        static constexpr f32 SolidUV = 0.5f;

        constexpr VGVertex() noexcept = default;

        // Constructors take float Color and pack to Color32 at emission.
        constexpr VGVertex(Float2 inPosition, Float2 inTexCoord, Color inColor,
                           f32 inCoverage = 1.0f) noexcept
            : position(inPosition), texCoord(inTexCoord), color(inColor), coverage(inCoverage)
        {
        }

        constexpr VGVertex(f32 x, f32 y, f32 u, f32 v, Color inColor,
                           f32 inCoverage = 1.0f) noexcept
            : position(x, y), texCoord(u, v), color(inColor), coverage(inCoverage)
        {
        }

        /// Create a solid-color vertex (no texture).
        [[nodiscard]] static constexpr VGVertex Solid(Float2 position, Color color,
                                                      f32 coverage = 1.0f) noexcept
        {
            return VGVertex(position, Float2{SolidUV, SolidUV}, color, coverage);
        }

        /// Create a solid-color vertex (no texture).
        [[nodiscard]] static constexpr VGVertex Solid(f32 x, f32 y, Color color,
                                                      f32 coverage = 1.0f) noexcept
        {
            return VGVertex(x, y, SolidUV, SolidUV, color, coverage);
        }
    };

    static_assert(sizeof(VGVertex) == VGVertex::SizeInBytes,
                  "VGVertex must stay 24 bytes for the renderer layout");
}
