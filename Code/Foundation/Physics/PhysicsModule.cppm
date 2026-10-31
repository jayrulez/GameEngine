// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Physics - the `foundation.physics` module.
//
// The rigid-body physics core over the vendored Jolt: world, bodies, primitive and compound
// shapes, the fixed layer matrix, queries, and buffered contact events. Jolt is the only backend
// and there is no abstraction layer, but JPH types never cross the public surface. Scene
// integration (components, transform sync, interpolation, settings) lives in engine.physics.

export module foundation.physics;

export import :world;
