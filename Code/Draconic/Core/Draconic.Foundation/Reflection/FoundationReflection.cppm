// Draconic Foundation - :foundation_reflection partition
//
// Reflects Foundation's value types (vectors, color, quaternion, transform, geometry
// primitives, matrices, Guid) so they can be introspected and bound to scripting.
// Call RegisterFoundationTypes() once at startup; it patches each type's TypeOf<T>() in
// place and registers them in the GlobalTypeRegistry, plus namespace-level math
// constants in the GlobalConstantRegistry.
//
// This is the interface: it declares only the entry point. The reflection bodies
// (DRACONIC_REFLECT_* macro expansions) live in FoundationReflectionImpl.cpp, kept out of
// the interface so GCC does not emit a gcm cluster for consumers
// (see gcc-module-interface-hygiene).

export module draconic.foundation:foundation_reflection;

export namespace draconic::foundation
{
    // Registers all Foundation value types for reflection (patches each TypeOf<T>()) and
    // adds them to the GlobalTypeRegistry. Idempotent; call once at startup.
    void RegisterFoundationTypes();
}
