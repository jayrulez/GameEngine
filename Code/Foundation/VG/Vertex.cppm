// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :vertex partition.
//
// VGVertex: the GPU vertex for vector graphics with analytical-AA coverage.
// Ported from Sedulous.VG/VGVertex.bf. Color is stored as a full float Color
// (RGBA). The renderer's GPU color attribute is Float32x4 (see VGRenderVertex),
// so packing to a byte Color32 here only quantized colors to 8-bit for no GPU
// benefit - it re-expanded to float immediately, and the quantization showed up
// as visible banding across gradients and AA fringes. Keeping float precision
// end to end removes that banding (see [[vg-quality-track]]).

module;
#include "Core/Prelude.h"

export module foundation.vg:vertex;

import foundation.core;

using namespace foundation::core;

export namespace foundation::vg
{
    /// Vertex structure for vector graphics with analytical AA support.
    // ENGINE_EXPORT_DATA for SizeInBytes: a static data member read from another
    // library needs the annotation (shared-libraries.md P5/W1).
    struct ENGINE_EXPORT_DATA VGVertex
    {
        Float2 position;     ///< Position in screen/world coordinates.
        Float2 texCoord;     ///< Texture coordinates (UV).
        Color color;         ///< Vertex color in full float RGBA (no 8-bit quantization).
        f32 coverage = 1.0f; ///< Analytical-AA coverage (0 = transparent fringe, 1 = opaque).
        /// A distance-field glyph's effect pass (TextEffects), in the field's units (0.5 is the
        /// edge): x moves the edge outward, y widens it into a soft ramp. Either nonzero reads
        /// the field's true distance (alpha) instead of the multi-channel median. Zero elsewhere.
        Float2 effect{};

        /// Size in bytes of this vertex structure.
        static constexpr i32 SizeInBytes = 44; // 8 + 8 + 16 + 4 + 8

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

    /// One corner of an analytic box (VGContext::DrawBox, VGDrawMode::Box): every vertex carries
    /// the whole box, so the fragment shader evaluates its fill and border from one signed
    /// distance with no tessellation. `local` is the point relative to the box's centre in the
    /// box's own units (interpolated, it stays exact under any affine transform); the rest is the
    /// same at every corner.
    struct ENGINE_EXPORT_DATA VGBoxVertex
    {
        Float2 position;     ///< Device position (the box's quad, grown for the AA ramp).
        Float2 local;        ///< Relative to the box's centre, in box units.
        Float2 halfSize;     ///< Half the box's size, in box units.
        Float4 radii;        ///< Corner radii: top left, top right, bottom right, bottom left.
        Float4 borders;      ///< Border widths: top, right, bottom, left.
        Color fill;          ///< The fill (authored sRGB, opacity applied).
        Color borderColor;   ///< The border's colour, drawn over the fill.

        static constexpr i32 SizeInBytes = 88; // 8 + 8 + 8 + 16 + 16 + 16 + 16
    };

    static_assert(sizeof(VGBoxVertex) == VGBoxVertex::SizeInBytes, "VGBoxVertex must stay 88 bytes");

    static_assert(sizeof(VGVertex) == VGVertex::SizeInBytes,
                  "VGVertex must match the renderer layout (VGRenderVertex)");
}
