/// Draconic::Animation - `draconic.animation`, the engine's skeletal-animation foundation.
///
/// A faithful port of Sedulous.Animation (foundation only - no rendering dependency; the renderer
/// consumes evaluated skinning matrices). Depends solely on Foundation (math + containers). Aggregates the
/// partitions: easing (:easing), the skeleton hierarchy (:skeleton), and the pose view (:pose).
/// The clip/sampler/player and the animation graph land in later partitions.

export module draconic.animation;

export import :easing;
export import :skeleton;
export import :pose;
export import :clip;
export import :sampler;
export import :player;
export import :graph;
