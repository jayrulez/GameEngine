// draconic.particles - the CPU particle runtime, aggregating its partitions. A faithful
// port of SedulousEngine's Sedulous.Particles (CPU side): the SoA stream container, the
// initializer/behavior module taxonomy, the effect/system/emitter/instance object model,
// and the CPU simulator. The cooked resource, GPU-compute simulator, ECS/render
// integration, and editor authoring are separate layers on top. See docs/design/particles.md.

export module draconic.particles;

export import :types;   // enums, RangeValue, ParticleCurve, EmissionShape, events, update context
export import :streams; // ParticleStreamId + SoA stream container (ParticleStream / CPUStream)
export import :modules; // initializer/behavior/simulator bases + concrete modules + CPU simulator
export import :effect; // ParticleEmitter / ParticleSystem / ParticleEffect / ParticleEffectInstance
