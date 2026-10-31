// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles - the CPU particle runtime, aggregating its partitions: the SoA stream
// container, the initializer and behavior modules, the effect, system, emitter and instance
// object model, and the CPU simulator. The cooked resource, the scene and render integration,
// and editor authoring are separate layers on top.

export module foundation.particles;

export import :types;   // enums, RangeValue, ParticleCurve, EmissionShape, events, update context
export import :streams; // ParticleStreamId, the SoA stream container (ParticleStream, CPUStream)
export import :modules; // initializer, behavior and simulator bases, concrete modules, the CPU simulator
export import :effect; // ParticleEmitter, ParticleSystem, ParticleEffect, ParticleEffectInstance
