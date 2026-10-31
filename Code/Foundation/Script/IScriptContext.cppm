// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_context partition
//
// IScriptContext: an isolated script execution environment. Everything crossing
// the boundary uses Core's reflection currency - Variant for values/objects,
// TypeInfo for types - so the interface is VM-agnostic. Backends (Lua, ...) are
// plugins implementing this.

module;
#include "Core/Prelude.h"

export module foundation.script:script_context;

import foundation.core;
import :script_debug; // IScriptBlob (the LoadBlob seam)

namespace core = foundation::core;

export namespace foundation::script
{
    /// What every backend says when a script uses a handle whose object is gone (an element
    /// destroyed, a component removed from its entity): any use but `isValid()`, which every object
    /// handle answers, is this error, raised where it happens.
    inline constexpr const char* kObjectGoneMessage =
        "the object is gone (destroyed, or a component no longer on its entity)";

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
        core::StringView module; // may be empty
        core::i32 line;          // 1-based source line, or -1 if unknown
        core::i32 column;        // 1-based column on that line, or -1 if unknown
        core::StringView message;
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
    class ScriptObject : public core::Object
    {
    public:
        // Invoke a method on this object with reflected args. Returns its result
        // (empty Variant for a void method), or an error if the method is missing
        // or the script faults.
        [[nodiscard]] virtual core::Result<core::Variant>
        Invoke(core::StringView method, core::Span<core::Variant> args) = 0;
        // Reads the instance's property (a member field) named `name`: a game script's score or
        // lives count, for a playtest that probes it. NotFound when the object has no such
        // property; methods are not properties.
        [[nodiscard]] virtual core::Result<core::Variant> GetProperty(core::StringView name) = 0;
    };

    // One class in a structured behavior-module load. `name` is the class's SOURCE FILE
    // identity (ScriptClass::sourceName - e.g. "Mover.as"): a debug-capable backend makes
    // it the class's own script section, so GetLineNumber reports (sourceFile, sourceLine)
    // and an editor breakpoint keyed on the file lines up. `source` is the class body.
    struct BehaviorModuleClass
    {
        core::StringView name;   // the source-file section identity (may be empty)
        core::StringView source; // the class source text
        // Optional precompiled bytecode for this class (ScriptClass::bytecode). When present and
        // the backend has a stable per-class bytecode (ScriptCapabilities::Bytecode), the backend
        // loads THIS instead of compiling `source` - the player path (no compiler needed). Empty
        // means source-only. The default source-concatenating LoadBehaviorModule ignores it.
        core::Span<const core::byte> bytecode;
    };

    class IScriptContext : public core::Object
    {
    public:
        // Sets (or clears, with nullptr) the error sink. When unset, backends
        // report errors to the console.
        virtual void SetErrorHandler(IScriptErrorHandler* handler) = 0;

        // Compile and run a chunk of script source. NotSupported if the backend
        // has no compiler (e.g. it only loads precompiled blobs).
        virtual core::Status Load(core::StringView source, core::StringView chunkName) = 0;

        // Load the run's behavior module from its per-class sources, PRESERVING per-class
        // section identity. The run host owns the generation bookkeeping and passes
        // `moduleName` (e.g. "behaviors#3") for the module; each class carries its own
        // `name` (its sourceName). A debug-capable backend adds each class as its OWN
        // script section named `name`, so GetLineNumber reports the source file (not the
        // concatenated module) - the identity an editor breakpoint keys on. The default
        // reproduces the frameless single-chunk behavior (newline-joined sources loaded as
        // `moduleName`); a backend that needs language framing (a facade import prelude, a
        // coroutine base) overrides to add it - see IScriptManager::AssembleBehaviorModuleSource.
        virtual core::Status LoadBehaviorModule(core::Span<const BehaviorModuleClass> classes,
                                                core::StringView moduleName)
        {
            core::String moduleSource;
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
        virtual core::Status LoadBlob(IScriptBlob& blob)
        {
            (void)blob;
            return core::Status{core::ErrorCode::NotSupported};
        }

        // Globals are exchanged as Variants (values or objects).
        virtual void SetGlobal(core::StringView name, const core::Variant& value) = 0;
        [[nodiscard]] virtual core::Variant GetGlobal(core::StringView name) = 0;

        // True if a callable global by that name exists.
        [[nodiscard]] virtual bool HasFunction(core::StringView name) const = 0;

        // Call a global function with reflected args; returns its result (void
        // -> empty Variant), or an error if missing / on a script fault.
        [[nodiscard]] virtual core::Result<core::Variant> Call(core::StringView function,
                                                               core::Span<core::Variant> args) = 0;

        // Instantiate a script-defined class by name, passing reflected
        // constructor args. Returns null if the class is unknown or construction
        // faults. The returned object outlives this call and retains the context.
        [[nodiscard]] virtual core::RefPtr<ScriptObject>
        CreateInstance(core::StringView className, core::Span<core::Variant> args) = 0;

        // ---- host services (per-context) ----
        // Name-keyed host objects native facades resolve DURING a scripted call (via
        // CurrentScriptContext below). This is how engine singletons reach scripts
        // without process globals: each context binds its OWN service set - two
        // contexts can see two different input runtimes (players, editor vs game).
        void SetService(core::StringView name, void* service)
        {
            m_services.InsertOrAssign(core::String(name), service);
        }
        [[nodiscard]] void* GetService(core::StringView name) const
        {
            void* const* found = m_services.Find(core::String(name));
            return found != nullptr ? *found : nullptr;
        }

    private:
        core::HashMap<core::String, void*> m_services;
    };

    // The context whose script code is EXECUTING right now on this thread (null outside
    // scripted calls). Backends push it around every reflected dispatch; native facades
    // (the Input class) resolve their per-context services through it. Nesting-safe
    // (script -> native -> script restores the previous).
    //
    // The slot accessor is NON-inline, defined in ScriptContextImpl.cpp: the backend
    // pushing (Script.AngelScript/.Luau) and the facade reading (Engine.*) live in
    // different libraries, and a vague-linkage thread_local would duplicate per shared
    // library - the facade would read null and lose its per-context services
    // (shared-libraries.md rendezvous rule).
    namespace detail
    {
        [[nodiscard]] IScriptContext*& CurrentScriptContextSlot() noexcept;
    }
    [[nodiscard]] inline IScriptContext* CurrentScriptContext() noexcept
    {
        return detail::CurrentScriptContextSlot();
    }
    class ScriptCallScope
    {
    public:
        explicit ScriptCallScope(IScriptContext* scriptContext) noexcept
            : m_previous(detail::CurrentScriptContextSlot())
        {
            detail::CurrentScriptContextSlot() = scriptContext;
        }
        ~ScriptCallScope() { detail::CurrentScriptContextSlot() = m_previous; }
        ScriptCallScope(const ScriptCallScope&) = delete;
        ScriptCallScope& operator=(const ScriptCallScope&) = delete;

    private:
        IScriptContext* m_previous;
    };
}
