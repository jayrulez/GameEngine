// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - the `foundation.rendergraph` module.
//
// A render graph over the RHI: passes declare resource accesses, the graph
// resolves dependencies, allocates/aliases transient resources, and inserts the
// right barriers automatically. One named module composed of partitions,
// re-exported here.

export module foundation.rendergraph;

export import :types;
export import :callbacks;
export import :descriptors;
export import :persistent_resource;
export import :resource;
export import :pass;
export import :state_tracker;
export import :barrier_solver;
export import :pass_builder;
export import :transient_pool;
export import :graph;
export import :validator;
export import :debug;
export import :profiler;
