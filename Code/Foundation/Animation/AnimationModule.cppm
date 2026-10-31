// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - `foundation.animation`, the engine's skeletal-animation foundation.
///
/// It has no rendering dependency (the renderer consumes evaluated skinning matrices) and depends
/// only on Core (math and containers). It aggregates the partitions: easing, the skeleton
/// hierarchy, the pose view, clips, the sampler, root motion, pose modifiers, inverse kinematics,
/// the clip player and the animation graph.

export module foundation.animation;

export import :easing;
export import :skeleton;
export import :pose;
export import :clip;
export import :sampler;
export import :rootmotion;
export import :modifier;
export import :ik;
export import :player;
export import :graph;
