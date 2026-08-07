/// Draconic::Materials - `draconic.materials`, the data-driven material model.
///
/// Aggregates the partitions: value types (:types), render-state + vertex layouts
/// (:pipeline), the shared Material template (:material), the fluent MaterialBuilder
/// (:builder), per-use MaterialInstance (:instance), and the GPU-resource-owning
/// MaterialSystem (:system) that infers bind-group layouts from declared properties.

export module draconic.materials;

export import :types;
export import :pipeline;
export import :material;
export import :builder;
export import :instance;
export import :system;
