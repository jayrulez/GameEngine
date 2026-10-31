// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_manager partition
//
// IScriptManager: the VM. Reflected types are registered with it (it walks each
// TypeInfo to build the backend's bindings), and it creates execution contexts.
// Each backend (AngelScript, Luau) implements it in its own library.

module;
#include "Core/Prelude.h"

export module foundation.script:script_manager;

import foundation.core;
import :script_context;
import :script_introspection; // DescribeBoundApi surface
import :script_debug;         // IScriptDebugger, IScriptProfiler, IScriptBlob

namespace core = foundation::core;

export namespace foundation::script
{
    /// Optional backend features, declared per backend. A consumer checks the flag and
    /// degrades cleanly: a backend without Coroutines still runs behaviors, it just has no
    /// coroutine scheduler.
    enum class ScriptCapabilities : core::u32
    {
        None = 0,
        Coroutines = 1 << 0, // cooperative coroutines (wait/waitUntil scheduler), run host-side
                             // per backend on that backend's own primitive
        Debugger = 1 << 1,   // step debugger (IScriptDebugger)
        Profiler = 1 << 2,   // VM-level profiling hooks (no backend implements them yet)
        Delegates = 1 << 3,  // script functions as native callbacks (IScriptDelegate)
        Bytecode = 1 << 4,   // cook-to-bytecode blob (IScriptBlob)
    };
    inline constexpr ScriptCapabilities operator|(ScriptCapabilities a, ScriptCapabilities b)
    {
        return static_cast<ScriptCapabilities>(static_cast<core::u32>(a) |
                                               static_cast<core::u32>(b));
    }
    inline constexpr bool HasScriptCapability(ScriptCapabilities value, ScriptCapabilities flag)
    {
        return (static_cast<core::u32>(value) & static_cast<core::u32>(flag)) != 0;
    }

    class IScriptManager : public core::Object
    {
    public:
        // Expose a reflected type to scripts. The backend introspects the
        // TypeInfo (properties, methods, constructors, constants) and installs
        // the corresponding bindings.
        virtual void RegisterType(const core::TypeInfo& type) = 0;

        /// Called once after all RegisterType calls, before the first CreateContext
        /// (RegisterReflectedTypes drives it). Backends that need two-phase emission defer
        /// it to here: AngelScript must declare every object type before any member of any
        /// type is registered, or everything would have to arrive in strict dependency
        /// order, so it declares all types and then binds all members. Backends with lazy
        /// emitters do nothing.
        virtual void FinalizeTypes() {}

        // Create a fresh, isolated execution context. Contexts see the classes
        // registered up to their creation.
        [[nodiscard]] virtual core::RefPtr<IScriptContext> CreateContext() = 0;

        // Compile `source` (named `chunkName` in errors) against the registered surface and
        // report its compile errors to `errors`, without running any of it: no top-level code,
        // no global initializers, no main. For checking code as it is typed, where running it
        // would run whatever is half written. Returns whether it compiled.
        [[nodiscard]] virtual bool Check(core::StringView source, core::StringView chunkName,
                                         IScriptErrorHandler& errors) = 0;

        // Single-step garbage collection, for backends that need it kept small in
        // real-time loops. Default: no-op.
        virtual void CollectGarbage() {}

        /// Resume every coroutine whose wait has elapsed, accumulating `deltaSeconds`;
        /// drop the ones that complete or fault. The scheduler lives host-side inside
        /// the backend (each backend on its own primitive); the subsystem calls this
        /// once per simulated frame. A backend without ScriptCapabilities::Coroutines
        /// leaves this a no-op. The conformance battery certifies the behavior; the flag
        /// only advertises it.
        virtual void AdvanceCoroutines(core::f64 deltaSeconds) { (void)deltaSeconds; }

        /// Stop and drop every coroutine owned by `instance` (its behavior is being
        /// disabled or destroyed). No-op on a backend without the capability.
        virtual void CancelCoroutinesFor(ScriptObject& instance) { (void)instance; }

        /// The backend's optional feature set. Required behavior is certified by the
        /// conformance battery instead and is not flagged here.
        [[nodiscard]] virtual ScriptCapabilities Capabilities() const
        {
            return ScriptCapabilities::None;
        }

        /// The script-callable API this backend actually bound, spelled in the backend's own
        /// language (names and signatures differ per backend, and a backend may not bind every
        /// reflected type). It is the accurate source for autocomplete and for the conformance
        /// diff that catches a type the backend silently failed to bind. It is not a
        /// capability: every backend implements it. The empty default is the right answer for
        /// a backend that has bound nothing, and the diff then flags it.
        [[nodiscard]] virtual core::Array<ScriptApiType> DescribeBoundApi() const
        {
            return core::Array<ScriptApiType>{};
        }

        // ---- debugger, profiler and bytecode (optional capabilities) ----

        /// A step debugger for this VM, or null (the default) when
        /// ScriptCapabilities::Debugger is absent.
        [[nodiscard]] virtual core::UniquePtr<IScriptDebugger> CreateDebugger()
        {
            return core::UniquePtr<IScriptDebugger>{};
        }

        /// A VM profiler, or null when ScriptCapabilities::Profiler is absent (the default).
        [[nodiscard]] virtual core::UniquePtr<IScriptProfiler> CreateProfiler()
        {
            return core::UniquePtr<IScriptProfiler>{};
        }

        /// Compile source to an opaque bytecode blob (the cook side of the Bytecode
        /// capability). The default returns NotSupported, for a source-only backend without
        /// ScriptCapabilities::Bytecode; a backend with a stable bytecode overrides it.
        [[nodiscard]] virtual core::Result<core::RefPtr<IScriptBlob>>
        CompileToBlob(core::StringView source, core::StringView chunkName)
        {
            (void)source;
            (void)chunkName;
            return core::Err(core::ErrorCode::NotSupported);
        }

        /// Create an empty bytecode blob to deserialize a stored one into (the load side of
        /// the Bytecode capability). IScriptBlob is opaque and never a registry type, so the
        /// runtime cannot construct it by type name: the backend that produced it makes the
        /// empty blob, the caller Serializes into it, then hands it to IScriptContext::LoadBlob.
        /// The default returns null (ScriptCapabilities::Bytecode absent).
        [[nodiscard]] virtual core::RefPtr<IScriptBlob> CreateBlob()
        {
            return core::RefPtr<IScriptBlob>{};
        }

        /// Assemble the behavior module's source from the loaded class sources. The run
        /// host owns the language-neutral generation and state bookkeeping and compiles the
        /// result; the language-specific framing lives here, per backend, so no language
        /// syntax leaks into the neutral libraries. A backend whose reflected types live in
        /// a separate module prepends its own facade-import prelude and any coroutine base;
        /// a backend with globally visible types needs none. The default is a plain
        /// newline-joined concatenation, for a backend that needs no framing.
        [[nodiscard]] virtual core::String
        AssembleBehaviorModuleSource(core::Span<const core::StringView> classSources) const
        {
            core::String moduleSource;
            for (const core::StringView& source : classSources)
            {
                moduleSource += source;
                moduleSource += u8"\n";
            }
            return moduleSource;
        }
    };
}
