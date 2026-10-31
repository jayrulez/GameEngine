// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - `engine.animation`, the scene side of skeletal animation.
///
/// SkeletalAnimationComponent (single clip) and AnimationGraphComponent (state machine and blend
/// trees), their managers, which feed the animation into the render meshes, and the
/// AnimationSubsystem that injects the managers into scenes. Depends on foundation.animation,
/// foundation.scene and engine.render (the MeshComponent feed target); none of those depend back
/// on it.

export module engine.animation;

export import :components;
export import :propertyanimator;
export import :ik;
export import :attachment;
export import :subsystem;
