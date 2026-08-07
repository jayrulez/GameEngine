// Draconic::Physics - the `draconic.physics` module.
//
// The rigid-body physics core over the vendored Jolt (docs/design/physics.md): world,
// bodies, primitive/compound shapes, the fixed layer matrix, queries, and buffered
// contact events. Jolt is the committed backend with NO abstraction layer, but JPH
// types never cross the public surface. Scene integration (components, transform sync,
// interpolation, settings) lives in draconic.engine.physics.

export module draconic.physics;

export import :world;
