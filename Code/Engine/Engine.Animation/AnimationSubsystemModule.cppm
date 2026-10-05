// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - `engine.animation`, the scene side of skeletal animation.
///
/// SkeletalAnimationComponent (single clip) + AnimationGraphComponent (state machine / blend trees),
/// their managers (the animation<->render seam), and the AnimationSubsystem that injects the managers
/// into scenes. Depends on foundation.animation (foundation) + foundation.scene + engine.render
/// (the MeshComponent feed target); none of those depend back on it.

export module engine.animation;

export import :components;
export import :propertyanimator;
export import :ik;
export import :subsystem;
