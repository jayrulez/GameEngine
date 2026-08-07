// Draconic Script - :script_context partition
//
// IScriptContext: an isolated script execution environment. Everything crossing
// the boundary uses Foundation's reflection currency - Variant for values/objects,
// TypeInfo for types - so the interface is VM-agnostic. Backends (Lua, ...) are
// plugins implementing this; see Documentation/Planning.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script:script_context;

import draconic.foundation;
import :script_debug; // IScriptBlob (the LoadBlob seam)

namespace foundation = draconic::foundation;

export namespace draconic::script
{
    enum class ScriptErrorKind
    {
        Compile, // failed to compile source
        Runtime, // threw / aborted while running
    };

    // A script error. The string views are valid only for the duration of the
    // OnError callback (copy out if you need to keep them).
    struct ScriptError
    {
        ScriptErrorKind kind;
        foundation::StringView module; // may be empty
        foundation::i32 line;          // 1-based source line, or -1 if unknown
        foundation::StringView message;
    };

    // Host-provided sink for script errors (compile + runtime). Non-owning: the
    // host manages its lifetime and outlives the context it's attached to.
    class IScriptErrorHandler
    {
    public:
        virtual ~IScriptErrorHandler() = default;
        virtual void OnError(const ScriptError& error) = 0;
    };

    // A handle to a live script-side object - typically an instance of a
    // script-defined class. Reference-counted; it keeps its owning context alive
    // for as long as it exists. This is the shared primitive script integration
    // builds on: a global "driver" object the runtime ticks (tier 1), or - later,
    // when an ECS exists - per-entity script components an ECS system ticks.
    class ScriptObject : public foundation::Object
    {
    public:
        // Invoke a method on this object with reflected args. Returns its result
        // (empty Variant for a void method), or an error if the method is missing
        // or the script faults.
        [[nodiscard]] virtual foundation::Result<foundation::Variant>
        Invoke(foundation::StringView method, foundation::Span<foundation::Variant> args) = 0;
    };

    // One class in a structured behavior-module load. `name` is the class's SOURCE FILE
    // identity (ScriptClass::sourceName - e.g. "Mover.as"): a debug-capable backend makes
    // it the class's own script section, so GetLineNumber reports (sourceFile, sourceLine)
    // and an editor breakpoint keyed on the file lines up. `source` is the class body.
    struct BehaviorModuleClass
    {
        foundation::StringView name;   // the source-file section identity (may be empty)
        foundation::StringView source; // the class source text
    };

    class IScriptContext : public foundation::Object
    {
    public:
        // Sets (or clears, with nullptr) the error sink. When unset, backends
        // report errors to the console.
        virtual void SetErrorHandler(IScriptErrorHandler* handler) = 0;

        // Compile and run a chunk of script source. NotSupported if the backend
        // has no compiler (e.g. it only loads precompiled blobs).
        virtual foundation::Status Load(foundation::StringView source, foundation::StringView chunkName) = 0;

        // Load the run's behavior module from its per-class sources, PRESERVING per-class
        // section identity. The run host owns the generation bookkeeping and passes
        // `moduleName` (e.g. "behaviors#3") for the module; each class carries its own
        // `name` (its sourceName). A debug-capable backend adds each class as its OWN
        // script section named `name`, so GetLineNumber reports the source file (not the
        // concatenated module) - the identity an editor breakpoint keys on. The default
        // reproduces the frameless single-chunk behavior (newline-joined sources loaded as
        // `moduleName`); a backend that needs language framing (a facade import prelude, a
        // coroutine base) overrides to add it - see IScriptManager::AssembleBehaviorModuleSource.
        virtual foundation::Status LoadBehaviorModule(foundation::Span<const BehaviorModuleClass> classes,
                                                foundation::StringView moduleName)
        {
            foundation::String moduleSource;
            for (const BehaviorModuleClass& entry : classes)
            {
                moduleSource += entry.source;
                moduleSource += u8"\n";
            }
            return Load(moduleSource.AsView(), moduleName);
        }

        // Load a precompiled bytecode blob (the counterpart to
        // IScriptManager::CompileToBlob). COMMITTED SEAM: the default is NotSupported;
        // no backend implements it yet (ScriptCapabilities::Bytecode is absent).
        virtual foundation::Status LoadBlob(IScriptBlob& blob)
        {
            (void)blob;
            return foundation::Status{foundation::ErrorCode::NotSupported};
        }

        // Globals are exchanged as Variants (values or objects).
        virtual void SetGlobal(foundation::StringView name, const foundation::Variant& value) = 0;
        [[nodiscard]] virtual foundation::Variant GetGlobal(foundation::StringView name) = 0;

        // True if a callable global by that name exists.
        [[nodiscard]] virtual bool HasFunction(foundation::StringView name) const = 0;

        // Call a global function with reflected args; returns its result (void
        // -> empty Variant), or an error if missing / on a script fault.
        [[nodiscard]] virtual foundation::Result<foundation::Variant> Call(foundation::StringView function,
                                                               foundation::Span<foundation::Variant> args) = 0;

        // Instantiate a script-defined class by name, passing reflected
        // constructor args. Returns null if the class is unknown or construction
        // faults. The returned object outlives this call and retains the context.
        [[nodiscard]] virtual foundation::RefPtr<ScriptObject>
        CreateInstance(foundation::StringView className, foundation::Span<foundation::Variant> args) = 0;

        // ---- host services (per-context) ----
        // Name-keyed host objects native facades resolve DURING a scripted call (via
        // CurrentScriptContext below). This is how engine singletons reach scripts
        // without process globals: each context binds its OWN service set - two
        // contexts can see two different input runtimes (players, editor vs game).
        void SetService(foundation::StringView name, void* service)
        {
            m_services.InsertOrAssign(foundation::String(name), service);
        }
        [[nodiscard]] void* GetService(foundation::StringView name) const
        {
            void* const* found = m_services.Find(foundation::String(name));
            return found != nullptr ? *found : nullptr;
        }

    private:
        foundation::HashMap<foundation::String, void*> m_services;
    };

    // The context whose script code is EXECUTING right now on this thread (null outside
    // scripted calls). Backends push it around every reflected dispatch; native facades
    // (the Input class) resolve their per-context services through it. Nesting-safe
    // (script -> native -> script restores the previous).
    namespace detail
    {
        inline thread_local IScriptContext* g_currentContext = nullptr;
    }
    [[nodiscard]] inline IScriptContext* CurrentScriptContext() noexcept
    {
        return detail::g_currentContext;
    }
    class ScriptCallScope
    {
    public:
        explicit ScriptCallScope(IScriptContext* context) noexcept
            : m_previous(detail::g_currentContext)
        {
            detail::g_currentContext = context;
        }
        ~ScriptCallScope() { detail::g_currentContext = m_previous; }
        ScriptCallScope(const ScriptCallScope&) = delete;
        ScriptCallScope& operator=(const ScriptCallScope&) = delete;

    private:
        IScriptContext* m_previous;
    };
}
