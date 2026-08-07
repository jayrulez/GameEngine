/// Draconic::Scene - `draconic.scene`, the scene / ECS foundation.
///
/// The engine's world model: scenes of entities with a transform hierarchy and
/// per-scene systems (component managers), driven by a SceneSubsystem. This phase
/// aggregates the entity layer; later partitions add the transform hierarchy,
/// components + per-scene systems, and the SceneSubsystem.

export module draconic.scene;

export import :entity;
export import :phase;
export import :system;
export import :component;
export import :aware;
export import :events;
export import :scene;
export import :manager;
