// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script - the `foundation.script` module.
//
// A thin, VM-agnostic scripting abstraction built on Core's reflection: an
// IScriptManager (the VM) creates IScriptContexts, reflected types are
// registered with it, and all values cross the boundary as Variant. Concrete
// VM backends (AngelScript, Luau) are libraries implementing these interfaces.

export module foundation.script;

export import :script_delegate;
export import :script_introspection;
export import :script_debug;
export import :script_context;
export import :script_manager;
export import :script_register;
export import :backend_registry;
