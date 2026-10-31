// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.Renderer::Renderer - :vertex partition.
//
// VGRenderVertex: the GPU vertex layout (float2 pos, float2 uv, float4 color,
// float coverage, float2 effect) the vg shader expects. Built from the CPU VGVertex; the color
// passes through unconverted: vertex colors are authored sRGB and the vg vertex shader
// performs the single sRGB-to-linear decode (decoding here too would decode twice).

module;
#include <cstddef> // offsetof
#include "Core/Prelude.h"

export module foundation.vg.renderer:vertex;

import foundation.core;
import foundation.vg;

using namespace foundation::core;

export namespace foundation::vg::renderer
{
    // The fields keep VGVertex's names; two pipelines read texCoord, coverage and effect for
    // other values (VGVertex's comment and named constructors list them, and each fragment
    // shader names its inputs for its own meaning).
    struct VGRenderVertex
    {
        f32 position[2];
        f32 texCoord[2]; // the SDF operand for a box shadow
        f32 color[4];
        f32 coverage;    // the signed corner radius for a box shadow
        f32 effect[2];   // a glyph's edge shift and ramp; a layer's colour filter

        VGRenderVertex() = default;

        explicit VGRenderVertex(const foundation::vg::VGVertex& v)
        {
            position[0] = v.position.x;
            position[1] = v.position.y;
            texCoord[0] = v.texCoord.x;
            texCoord[1] = v.texCoord.y;
            color[0] = v.color.r;
            color[1] = v.color.g;
            color[2] = v.color.b;
            color[3] = v.color.a;
            coverage = v.coverage;
            effect[0] = v.effect.x;
            effect[1] = v.effect.y;
        }
    };

    static_assert(sizeof(VGRenderVertex) == 44,
                  "VGRenderVertex must be 44 bytes (8+8+16+4+8) for the shader layout");
    // The offsets the renderer's vertex layout names (Renderer.cppm, CreatePipelineVariant).
    static_assert(offsetof(VGRenderVertex, position) == 0);
    static_assert(offsetof(VGRenderVertex, texCoord) == 8);
    static_assert(offsetof(VGRenderVertex, color) == 16);
    static_assert(offsetof(VGRenderVertex, coverage) == 32);
    static_assert(offsetof(VGRenderVertex, effect) == 36);
}
