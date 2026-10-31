// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials - `foundation.materials`, the data-driven material model.
///
/// Aggregates the partitions: value types (:types), render state and vertex layouts
/// (:pipeline), the shared Material template (:material), the fluent MaterialBuilder
/// (:builder), per-use MaterialInstance (:instance), and the GPU-resource-owning
/// MaterialSystem (:system) that infers bind-group layouts from declared properties.

export module foundation.materials;

export import :types;
export import :pipeline;
export import :material;
export import :builder;
export import :instance;
export import :system;
