// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Navigation - the `foundation.navigation` module.
//
// The navmesh core over vendored recastnavigation: the Recast bake (triangle soup and an agent
// profile in, a serialized tiled navmesh out) and the Detour runtime (navmesh load, path query,
// agent crowd and local avoidance). Recast and Detour are used directly, with no abstraction
// layer, but rc* and dt* types never cross the public surface. The bake is a pure function (no
// scene, no geometry module, no device), so the whole module is testable headless.

export module foundation.navigation;

export import :builder;
export import :mesh;
