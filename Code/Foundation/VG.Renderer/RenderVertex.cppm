// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.Renderer::Renderer - :vertex partition.
//
// VGRenderVertex: the GPU vertex layout (float2 pos, float2 uv, float4 color,
// float coverage, float2 effect) the vg shader expects. Built from the CPU VGVertex; the color
// passes through UNCONVERTED - vertex colors are authored sRGB and the vg vertex
// shader performs the single sRGB->linear decode (decoding here too would double-
// decode). Ported from Sedulous.VG.Renderer/VGRenderVertex.bf.

module;
#include "Core/Prelude.h"

export module foundation.vg.renderer:vertex;

import foundation.core;
import foundation.vg;

using namespace foundation::core;

export namespace foundation::vg::renderer
{
    struct VGRenderVertex
    {
        f32 position[2];
        f32 texCoord[2];
        f32 color[4];
        f32 coverage;
        f32 effect[2];

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
}
