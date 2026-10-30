// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Runtime - the `foundation.runtime` module.
//
// The engine runtime: a SubsystemManager owns Subsystems and drives their lifecycle and
// per-frame phases. The application loop and plugin host build on this.

export module foundation.runtime;

export import :subsystem;
export import :subsystem_manager;
export import :plugin;
export import :pluginhost;
