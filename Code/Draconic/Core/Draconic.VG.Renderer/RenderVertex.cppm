// Draconic::VG::Renderer - :vertex partition.
//
// VGRenderVertex: the GPU vertex layout (float2 pos, float2 uv, float4 color,
// float coverage) the vg shader expects. Built from the CPU VGVertex; the color
// passes through UNCONVERTED - vertex colors are authored sRGB and the vg vertex
// shader performs the single sRGB->linear decode (decoding here too would double-
// decode). Ported from Sedulous.VG.Renderer/VGRenderVertex.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg.renderer:vertex;

import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;

export namespace draconic::vg::renderer
{
    struct VGRenderVertex
    {
        f32 position[2];
        f32 texCoord[2];
        f32 color[4];
        f32 coverage;

        VGRenderVertex() = default;

        explicit VGRenderVertex(const draconic::vg::VGVertex& v)
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
        }
    };

    static_assert(sizeof(VGRenderVertex) == 36,
                  "VGRenderVertex must be 36 bytes (8+8+16+4) for the shader layout");
}
