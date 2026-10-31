// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :core_reflection partition
//
// Reflects Core's value types (vectors, color, quaternion, transform, geometry
// primitives, matrices, Guid) so they can be introspected and bound to scripting.
// Call RegisterCoreTypes() once at startup; it patches each type's TypeOf<T>() in
// place and registers them in the GlobalTypeRegistry, plus namespace-level math
// constants in the GlobalConstantRegistry.
//
// This is the interface: it declares only the entry point. The reflection bodies
// (REFLECT_* macro expansions) live in CoreReflectionImpl.cpp, kept out of
// the interface so GCC does not emit a gcm cluster for consumers.

export module foundation.core:core_reflection;

export namespace foundation::core
{
    // Registers all Core value types for reflection (patches each TypeOf<T>()) and
    // adds them to the GlobalTypeRegistry. Idempotent; call once at startup.
    void RegisterCoreTypes();
}
