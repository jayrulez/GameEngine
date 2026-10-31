// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :vertex partition.
//
// VGVertex: the GPU vertex for vector graphics with analytical-AA coverage.
// Color is stored as a full float Color (RGBA). The renderer's GPU color attribute is
// Float32x4 (see VGRenderVertex), so packing to a byte Color32 here would only quantize
// colors to 8 bits, which shows as banding across gradients and AA fringes.

module;
#include <cstddef> // offsetof
#include "Core/Prelude.h"

export module foundation.vg:vertex;

import foundation.core;

using namespace foundation::core;

export namespace foundation::vg
{
    /// Vertex structure for vector graphics with analytical AA support.
    ///
    /// One layout serves every pipeline but the analytic box (VGBoxVertex), and two pipelines
    /// read some fields for another value; each has a named constructor so the write site says
    /// which (and the shader names its input the same way):
    /// - the box shadow (BoxShadow, vg_shadow.ps): `texCoord` is the rounded-box SDF operand q
    ///   and `coverage` the corner radius in blur-sigma units, negative for an inset shadow;
    /// - a distance-field glyph's effect pass (Glyph, vg_df.ps): `effect` is the edge shift and
    ///   the ramp width;
    /// - a layer composite (LayerComposite, vg_layer.ps): `effect` is the colour filter as
    ///   offsets from no filter, (1 - saturation, brightness - 1).
    // ENGINE_EXPORT_DATA for SizeInBytes: a static data member read from another
    // library needs the annotation.
    struct ENGINE_EXPORT_DATA VGVertex
    {
        Float2 position;     ///< Position in screen/world coordinates.
        Float2 texCoord;     ///< Texture coordinates (UV); the SDF operand for a box shadow.
        Color color;         ///< Vertex color in full float RGBA (no 8-bit quantization).
        /// Analytical-AA coverage (0 = transparent fringe, 1 = opaque); the signed corner radius
        /// for a box shadow.
        f32 coverage = 1.0f;
        /// Zero but for two pipelines. A distance-field glyph's effect pass (TextEffects), in the
        /// field's units (0.5 is the edge): x moves the edge outward, y widens it into a soft
        /// ramp; either nonzero reads the field's true distance (alpha) instead of the
        /// multi-channel median. A layer composite: (1 - saturation, brightness - 1).
        Float2 effect{};

        /// Size in bytes of this vertex structure.
        static constexpr i32 SizeInBytes = 44; // 8 + 8 + 16 + 4 + 8

        /// Fixed UV for solid-color drawing.
        static constexpr f32 SolidUV = 0.5f;

        constexpr VGVertex() noexcept = default;

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

        /// A box shadow's vertex (VGDrawMode::BoxShadow): `shadowOperand` is the rounded-box SDF
        /// operand q = |p - c| - (halfSize - r) in blur-sigma units, carried in `texCoord`;
        /// `signedCornerRadius` is the corner radius r in sigma units, negative for an inset
        /// shadow, carried in `coverage`.
        [[nodiscard]] static constexpr VGVertex BoxShadow(Float2 position, Float2 shadowOperand, Color color,
                                                          f32 signedCornerRadius) noexcept
        {
            return VGVertex(position, shadowOperand, color, signedCornerRadius);
        }

        /// A distance-field glyph's vertex: `edgeShiftRamp`, carried in `effect`, is x the edge's
        /// move outward and y its ramp width, in field units (zeros for the plain glyph pass).
        [[nodiscard]] static constexpr VGVertex Glyph(Float2 position, Float2 uv, Color color,
                                                      Float2 edgeShiftRamp) noexcept
        {
            VGVertex vertex(position, uv, color, 1.0f);
            vertex.effect = edgeShiftRamp;
            return vertex;
        }

        /// A layer composite's vertex (VGDrawMode::Layer): the colour filter rides `effect` as
        /// offsets from no filter, (1 - saturation, brightness - 1), so a plain composite carries
        /// zeros.
        [[nodiscard]] static constexpr VGVertex LayerComposite(Float2 position, Float2 uv, Color tint,
                                                               f32 saturation, f32 brightness) noexcept
        {
            VGVertex vertex(position, uv, tint);
            vertex.effect = Float2{1.0f - saturation, brightness - 1.0f};
            return vertex;
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
        // The border's colour on each side, drawn over the fill.
        Color borderTop;
        Color borderRight;
        Color borderBottom;
        Color borderLeft;

        static constexpr i32 SizeInBytes = 136; // 8 + 8 + 8 + 16 + 16 + 16 + 4 * 16
    };

    static_assert(sizeof(VGBoxVertex) == VGBoxVertex::SizeInBytes, "VGBoxVertex must match the box pipeline's layout");

    static_assert(sizeof(VGVertex) == VGVertex::SizeInBytes,
                  "VGVertex must match the renderer layout (VGRenderVertex)");
    static_assert(offsetof(VGVertex, position) == 0);
    static_assert(offsetof(VGVertex, texCoord) == 8);
    static_assert(offsetof(VGVertex, color) == 16);
    static_assert(offsetof(VGVertex, coverage) == 32);
    static_assert(offsetof(VGVertex, effect) == 36);
}
