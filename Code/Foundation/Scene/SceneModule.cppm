// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Scene - `foundation.scene`, the scene / ECS foundation.
///
/// The engine's world model: scenes of entities with a transform hierarchy and
/// per-scene systems (component managers), driven by a SceneSubsystem.

export module foundation.scene;

export import :entity;
export import :phase;
export import :system;
export import :component;
export import foundation.messaging; // EventBus lives in its own leaf module
export import :scene;
export import :manager;
export import :frame_time;
export import :composition;
