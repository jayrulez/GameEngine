// draconic.engine.particles:renderdata - the render-data the particle billboard path produces.
//
// A ParticleBillboardRenderData is a BATCH: one item per (system, texture, blend) carrying a
// borrowed pointer to `count` packed billboard instances. This is the key difference from the
// sprite path (one item per sprite) - particle counts would flood the draw-list sort otherwise.
// The ParticleRenderer packs the batch into its instance ring and emits one instanced draw.

module;
#include "Draconic.Foundation/Prelude.h"
#include <type_traits>

export module draconic.engine.particles:renderdata;

import draconic.foundation;
import draconic.rhi;
import draconic.render;    // RenderData base + RenderCategories
import draconic.particles; // ParticleBlendMode

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::particles
{
    // One packed billboard instance (80 bytes = 5x float4, matches the ParticleRenderer VS inputs).
    struct ParticleBillboardInstance
    {
        Float4 positionSize; // xyz world center, w width
        Float4 sizeRotMode;  // x height, y rotation (radians), z orientation mode, w unused
        Float4 color;        // rgba (linear, premultiply/scale done in sim)
        Float4 uvRect;       // xy uv min, zw uv size (flipbook)
        Float4 velocity;     // xyz world velocity, w stretch scale (0 = plain billboard)
    };
    static_assert(sizeof(ParticleBillboardInstance) == 80);

    // Both particle render-data kinds ride the one particle rendererId; this tells them apart in Resolve.
    struct ParticleRenderDataBase : draconic::render::RenderData
    {
        u8 particleKind = 0; // 0 = billboard batch, 1 = trail ribbon
    };

    // Batched billboard draw for one system (or texture/blend group). `instances` is borrowed and
    // valid for the frame (owned by the component manager's scratch). Rides the Transparent category.
    struct ParticleBillboardRenderData : ParticleRenderDataBase
    {
        const ParticleBillboardInstance* instances = nullptr;
        u32 count = 0;
        rhi::TextureView* texture = nullptr;
        ParticleBlendMode blend = ParticleBlendMode::Alpha; // selects the renderer's blend PSO
    };
    static_assert(std::is_trivially_destructible_v<ParticleBillboardRenderData>);

    // One ribbon vertex - world position + uv + color, pre-built camera-facing by the extractor. The
    // trail path draws these as a plain triangle list (no instancing, geometry already oriented).
    struct TrailVertex
    {
        Float3 position{0.0f, 0.0f, 0.0f};
        Float2 texCoord{0.0f, 0.0f};
        Float4 color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    // Batched trail ribbon for one system: a borrowed triangle-list vertex span (owned by the manager's
    // scratch for the frame).
    struct ParticleTrailRenderData : ParticleRenderDataBase
    {
        ParticleTrailRenderData() noexcept { particleKind = 1; }
        const TrailVertex* vertices = nullptr;
        u32 vertexCount = 0;
        rhi::TextureView* texture = nullptr;
        ParticleBlendMode blend = ParticleBlendMode::Alpha;
    };
    static_assert(std::is_trivially_destructible_v<ParticleTrailRenderData>);
}
