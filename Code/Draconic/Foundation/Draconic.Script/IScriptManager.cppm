// Draconic Script - :script_manager partition
//
// IScriptManager: the VM. Reflected types are registered with it (it walks each
// TypeInfo to build the backend's bindings), and it creates execution contexts.
// One backend (Lua, ...) implements this per VM; backends are plugins.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script:script_manager;

import draconic.foundation;
import :script_context;
import :script_introspection; // DescribeBoundApi surface
import :script_debug;         // IScriptDebugger / IScriptProfiler / IScriptBlob seams

namespace foundation = draconic::foundation;

export namespace draconic::script
{
    /// Optional backend features (scripting.md B4), declared per backend and consumed
    /// contract-first: a consumer CHECKS the flag and degrades cleanly - a backend
    /// without Coroutines still runs behaviors, it just has no coroutine scheduler.
    enum class ScriptCapabilities : foundation::u32
    {
        None = 0,
        Coroutines = 1 << 0, // cooperative coroutines (wait/waitUntil scheduler) - implemented
                             // host-side per backend (each backend uses its own primitive)
        Debugger = 1 << 1,   // step-debug seam (interface committed; none implemented yet)
        Profiler = 1 << 2,   // VM-level profiling hooks (interface committed; none implemented yet)
        Delegates = 1 << 3,  // script functions as native callbacks (IScriptDelegate) - implemented
        Bytecode = 1 << 4,   // cook-to-bytecode blob (interface committed; none implemented yet)
    };
    inline constexpr ScriptCapabilities operator|(ScriptCapabilities a, ScriptCapabilities b)
    {
        return static_cast<ScriptCapabilities>(static_cast<foundation::u32>(a) |
                                               static_cast<foundation::u32>(b));
    }
    inline constexpr bool HasScriptCapability(ScriptCapabilities value, ScriptCapabilities flag)
    {
        return (static_cast<foundation::u32>(value) & static_cast<foundation::u32>(flag)) != 0;
    }

    class IScriptManager : public foundation::Object
    {
    public:
        // Expose a reflected type to scripts. The backend introspects the
        // TypeInfo (properties / methods / constructors / constants) and installs
        // the corresponding bindings.
        virtual void RegisterType(const foundation::TypeInfo& type) = 0;

        /// Called once after ALL RegisterType calls, before the first CreateContext
        /// (RegisterReflectedTypes drives it). Backends that need two-phase emission -
        /// AngelScript must DECLARE every object type before any member of any type is
        /// registered, or everything has to arrive in strict dependency order - defer
        /// their emission to here: declare-all-types, then bind-all-members. Backends
        /// with lazy emitters (Wren materializes at context creation) no-op.
        virtual void FinalizeTypes() {}

        // Create a fresh, isolated execution context. Contexts see the classes
        // registered up to their creation.
        [[nodiscard]] virtual foundation::RefPtr<IScriptContext> CreateContext() = 0;

        // Single-step garbage collection, for backends that need it kept small in
        // real-time loops. Default: no-op.
        virtual void CollectGarbage() {}

        /// Resume every coroutine whose wait has elapsed, accumulating `deltaSeconds`;
        /// drop the ones that complete or fault. The scheduler lives host-side inside
        /// the backend (each backend on its own primitive); the subsystem calls this
        /// ONCE per simulated frame. A backend without ScriptCapabilities::Coroutines
        /// leaves this a no-op - the battery certifies the behavior, the flag only
        /// advertises it.
        virtual void AdvanceCoroutines(foundation::f64 deltaSeconds) { (void)deltaSeconds; }

        /// Stop and drop every coroutine owned by `instance` (its behavior is being
        /// disabled or destroyed). No-op on a backend without the capability.
        virtual void CancelCoroutinesFor(ScriptObject& instance) { (void)instance; }

        /// The backend's OPTIONAL feature set. Required behavior is certified by the
        /// conformance battery instead - never flagged here.
        [[nodiscard]] virtual ScriptCapabilities Capabilities() const
        {
            return ScriptCapabilities::None;
        }

        /// The ACTUAL script-callable API this backend bound, spelled in the backend's own
        /// language (names/signatures differ per backend, and a backend may not bind every
        /// reflected type). The accurate source for a future ScriptClassesView/autocomplete
        /// and for the conformance diff that catches a type the backend silently failed to
        /// bind. NOT a capability - every backend implements it; the default (empty) is the
        /// honest answer for a backend that has bound nothing, and the diff then flags it.
        [[nodiscard]] virtual foundation::Array<ScriptApiType> DescribeBoundApi() const
        {
            return foundation::Array<ScriptApiType>{};
        }

        // ---- committed seams (no backend implements these yet) ----

        /// A step debugger for this VM, or null when ScriptCapabilities::Debugger is absent
        /// (the default - the seam is committed, the impl is a later track).
        [[nodiscard]] virtual foundation::UniquePtr<IScriptDebugger> CreateDebugger()
        {
            return foundation::UniquePtr<IScriptDebugger>{};
        }

        /// A VM profiler, or null when ScriptCapabilities::Profiler is absent (the default).
        [[nodiscard]] virtual foundation::UniquePtr<IScriptProfiler> CreateProfiler()
        {
            return foundation::UniquePtr<IScriptProfiler>{};
        }

        /// Compile source to an opaque bytecode blob (the cook side of the Bytecode seam).
        /// Default: NotSupported (ScriptCapabilities::Bytecode absent). A backend with a
        /// stable bytecode (AngelScript SaveByteCode) fills this later; a source-only
        /// backend (Wren) stays unsupported - exactly the split the capability model exists
        /// for.
        [[nodiscard]] virtual foundation::Result<foundation::RefPtr<IScriptBlob>>
        CompileToBlob(foundation::StringView source, foundation::StringView chunkName)
        {
            (void)source;
            (void)chunkName;
            return foundation::Err(foundation::ErrorCode::NotSupported);
        }

        /// Assemble the ONE behavior module's source from the loaded class SOURCES. The
        /// run host owns the neutral generation/state bookkeeping and compiles the result;
        /// the LANGUAGE-SPECIFIC framing lives HERE, per backend, so no language syntax
        /// leaks into the neutral libraries (scripting.md §7.5). A backend whose reflected
        /// types live in a separate module prepends its own facade-import prelude + any
        /// coroutine base; a backend with globally-visible types needs none. The default
        /// is a plain newline-joined concatenation - the safe behavior for a backend that
        /// needs no framing at all.
        [[nodiscard]] virtual foundation::String
        AssembleBehaviorModuleSource(foundation::Span<const foundation::StringView> classSources) const
        {
            foundation::String moduleSource;
            for (const foundation::StringView& source : classSources)
            {
                moduleSource += source;
                moduleSource += u8"\n";
            }
            return moduleSource;
        }
    };
}
