// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin partition
//
// IRuntimePlugin: a unit of engine functionality delivered separately from the
// core executable. A plugin registers its subsystems and services into the SubsystemManager
// on load and removes them on unload. Plugins may be linked statically (handed
// to PluginHost::Add) or loaded from a shared library at runtime
// (PluginHost::Load). Plugins own the subsystems they register: they register
// them non-owningly via SubsystemManager::RegisterSubsystem and tear them down in
// OnUnload, before the backing library is closed.

module;
#include "Core/Prelude.h"

export module foundation.runtime:plugin;

import foundation.core;
import :subsystem_manager;

namespace core = foundation::core;

export namespace foundation::runtime
{
    class IRuntimePlugin
    {
    public:
        virtual ~IRuntimePlugin() = default;

        // Human-readable identifier (for logging/diagnostics).
        [[nodiscard]] virtual core::StringView Name() const noexcept = 0;

        // Register the plugin's subsystems/services into the SubsystemManager.
        virtual void OnLoad(SubsystemManager& manager) = 0;

        // Remove what OnLoad added. Called before the backing library is closed,
        // so it is safe to touch plugin-defined types and destroy subsystems here.
        virtual void OnUnload(SubsystemManager& manager) = 0;
    };

    // Signature of the factory a dynamically-loaded plugin library must export
    // under the C name in CreatePluginSymbol. It returns a plugin instance owned
    // by the library (typically a function-local static); the host never frees
    // it, and closes the library after OnUnload.
    using CreatePluginFn = IRuntimePlugin* (*)();

    // The exported symbol name PluginHost::Load resolves in a plugin library.
    inline constexpr core::StringView CreatePluginSymbol = u8"CreatePlugin";
}
