// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.particles - scene and render integration for the particle system: the ECS component and
// manager (which ticks the CPU simulation and provides billboard render data), the dedicated
// ParticleRenderer, and the application-level ParticleSubsystem that wires them into
// RenderSubsystem through its generic register-renderer and register-provider calls. Depends on
// render.subsystem, scene and foundation.particles; foundation.render stays ignorant of particles.

export module engine.particles;

export import :renderdata; // ParticleBillboardInstance, ParticleBillboardRenderData
export import :renderer;   // ParticleRenderer (dedicated billboard Renderer)
export import :components; // ParticleEffectComponent and manager (sim tick, IRenderDataProvider)
export import :subsystem;  // ParticleSubsystem (injects manager, registers renderer and provider)
