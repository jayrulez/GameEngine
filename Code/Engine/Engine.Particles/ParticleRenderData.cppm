// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.particles:renderdata - the render-data the particle billboard path produces.
//
// A ParticleBillboardRenderData is a BATCH: one item per (system, texture, blend) carrying a
// borrowed pointer to `count` packed billboard instances. This is the key difference from the
// sprite path (one item per sprite) - particle counts would flood the draw-list sort otherwise.
// The ParticleRenderer packs the batch into its instance ring and emits one instanced draw.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof
#include <type_traits>

export module engine.particles:renderdata;

import foundation.core;
import foundation.rhi;
import foundation.render;    // RenderData base + RenderCategories
import foundation.particles; // ParticleBlendMode

using namespace foundation::core;
using namespace foundation::particles;
namespace rhi = foundation::rhi;

export namespace engine::particles
{
    // A particle's colour as simulated (authored sRGB: the effect's colours, curves and trail
    // colour, interpolated as entered) decoded to linear for the renderer. Applied where the
    // colour leaves the simulation: billboards, trails, mesh tints and particle lights.
    [[nodiscard]] inline Float4 ParticleColorToLinear(Float4 c) noexcept
    {
        const Color l = ToLinear(Color{c.x, c.y, c.z, c.w});
        return Float4{l.r, l.g, l.b, l.a};
    }

    // One packed billboard instance (80 bytes). The vertex layout reads it as five float4s at
    // locations 0 to 4 (ParticleRenderer's billboard pipeline); particle.vs.hlsl unpacks each into
    // the fields named here, so the offsets below are the shader's contract.
    struct ParticleBillboardInstance
    {
        Float3 position;  // world centre
        f32 width;
        f32 height;
        f32 rotation;     // radians, the camera-facing roll
        f32 mode;         // the orientation mode's ordinal, as a float (the shader rounds it)
        f32 softDistance; // the soft-particle fade band (0 = off)
        Float4 color;     // rgba, linear (decoded from the simulated sRGB colour)
        Float4 uvRect;    // xy uv min, zw uv size (flipbook)
        Float3 velocity;  // world velocity
        f32 stretch;      // the stretch scale (0 = plain billboard)
    };
    static_assert(sizeof(ParticleBillboardInstance) == 80);
    static_assert(offsetof(ParticleBillboardInstance, position) == 0);
    static_assert(offsetof(ParticleBillboardInstance, width) == 12);
    static_assert(offsetof(ParticleBillboardInstance, height) == 16);
    static_assert(offsetof(ParticleBillboardInstance, rotation) == 20);
    static_assert(offsetof(ParticleBillboardInstance, mode) == 24);
    static_assert(offsetof(ParticleBillboardInstance, softDistance) == 28);
    static_assert(offsetof(ParticleBillboardInstance, color) == 32);
    static_assert(offsetof(ParticleBillboardInstance, uvRect) == 48);
    static_assert(offsetof(ParticleBillboardInstance, velocity) == 64);
    static_assert(offsetof(ParticleBillboardInstance, stretch) == 76);

    // Both particle render-data kinds ride the one particle rendererId; this tells them apart in Resolve.
    struct ParticleRenderDataBase : foundation::render::RenderData
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
