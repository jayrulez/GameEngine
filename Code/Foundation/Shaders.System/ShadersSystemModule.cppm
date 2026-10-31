// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Shaders.System - the `foundation.shaders.system` module.
///
/// The variant compile-on-demand cache (:shader_system) and the dev file-backed
/// source provider (:file_provider) that serves engine built-in shaders from the
/// engine shader root.

export module foundation.shaders.system;

export import :shader_system;
export import :file_provider;
export import :host;
