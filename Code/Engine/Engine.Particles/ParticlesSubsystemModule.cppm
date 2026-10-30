// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.particles - scene/render integration for the particle system: the ECS
// component + manager (which ticks the CPU sim and provides billboard render-data), the dedicated
// ParticleRenderer, and the application-level ParticleSubsystem that wires them into RenderSubsystem via
// its generic register-renderer / register-provider seam. Depends on render.subsystem + scene +
// foundation.particles; foundation.render stays ignorant of particles.

export module engine.particles;

export import :renderdata; // ParticleBillboardInstance + ParticleBillboardRenderData
export import :renderer;   // ParticleRenderer (dedicated billboard Renderer)
export import :components; // ParticleEffectComponent + manager (sim tick + IRenderDataProvider)
export import :subsystem;  // ParticleSubsystem (injects manager, registers renderer + provider)
