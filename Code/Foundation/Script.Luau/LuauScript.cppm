// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.Luau - the Luau VM backend for Foundation::Script.
//
// Implements IScriptManager / IScriptContext / ScriptObject / IScriptDelegate over the
// vendored Luau VM + Compiler (ThirdParty/luau, interpreter-only). One isolated
// lua_State per context; reflected engine types are emitted into each state at context
// creation as a metatable (property get/set + method dispatch through the reflection
// currency: Variant in a userdata, ToInstance for the `this`). Coroutines run on a
// host-side scheduler over lua threads (startCoroutine / waitSeconds / waitUntil - the
// same wait model the other backends certify); delegates wrap a registry-ref'd Lua
// function with the Detach-on-close protocol the conformance battery requires.
//
// Error handling: the VM is built with LUA_USE_LONGJMP=1 (the engine is
// -fno-exceptions), and every entry into the VM goes through lua_pcall/lua_resume so a
// script fault lands back here as a status, never unwinding our frames.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include "Core/Reflection/Reflect.h"
#include "Script.Luau/LuauInclude.h"

export module foundation.script.luau;

import foundation.core;
import foundation.script;
import foundation.script.facades; // CollectEmittableTypes - the shared reachability closure

namespace core = foundation::core;
using namespace foundation::core;

namespace foundation::script
{
    class LuauScriptManager;
    class LuauScriptContext;
    class LuauDebugger;

    namespace
    {
        [[nodiscard]] const char* CStr(StringView view, String& storage)
        {
            storage = String(view);
            return reinterpret_cast<const char*>(storage.CStr());
        }

        [[nodiscard]] StringView ViewOf(const char* text)
        {
            return (text != nullptr) ? StringView(reinterpret_cast<const utf8char*>(text))
                                     : StringView{};
        }

        // Luau prefixes compile/runtime error messages with "<chunk>:<line>: <text>". Extract the
        // line number (the first ":<digits>:" run), or -1 when the message has no such prefix.
        [[nodiscard]] i32 ParseLuauErrorLine(StringView message)
        {
            for (usize i = 0; i + 1 < message.Size(); ++i)
            {
                if (message[i] != utf8char(':') || message[i + 1] < utf8char('0') ||
                    message[i + 1] > utf8char('9'))
                {
                    continue;
                }
                i32 line = 0;
                usize j = i + 1;
                while (j < message.Size() && message[j] >= utf8char('0') &&
                       message[j] <= utf8char('9'))
                {
                    line = line * 10 + static_cast<i32>(message[j] - utf8char('0'));
                    ++j;
                }
                if (j < message.Size() && message[j] == utf8char(':'))
                {
                    return line;
                }
            }
            return -1;
        }

        // A reflected type -> its Luau TYPE annotation for a .d.luau declaration. Primitives map
        // to Luau's built-ins; Float3 is the native `vector`; enums surface as numbers; a Variant
        // payload is `any`; a reflected CLASS resolves to its own declared name ONLY when we also
        // declare it (else `any`, so a declaration never references an undeclared type). nullptr =
        // a void return, spelled `()`.
        [[nodiscard]] String LuauTypeName(const TypeInfo* type, Span<const TypeInfo* const> declared)
        {
            if (type == nullptr)
            {
                return String(u8"()");
            }
            if (type == &TypeOf<Float3>())
            {
                return String(u8"vector");
            }
            if (type == &TypeOf<String>())
            {
                return String(u8"string");
            }
            if (type == &TypeOf<bool>())
            {
                return String(u8"boolean");
            }
            if (type == &TypeOf<f32>() || type == &TypeOf<f64>() || type == &TypeOf<i8>() ||
                type == &TypeOf<i16>() || type == &TypeOf<i32>() || type == &TypeOf<i64>() ||
                type == &TypeOf<u8>() || type == &TypeOf<u16>() || type == &TypeOf<u32>() ||
                type == &TypeOf<u64>())
            {
                return String(u8"number");
            }
            if (type == &TypeOf<Variant>() || type->enumeratorCount > 0)
            {
                // A Variant sink is `any`; an enum surfaces as its underlying number (the enum's
                // named table is declared separately as a value, not a type).
                return type->enumeratorCount > 0 ? String(u8"number") : String(u8"any");
            }
            for (const TypeInfo* known : declared)
            {
                if (known == type)
                {
                    return String(ViewOf(ScriptTypeName(*type))); // the alias (typed-decl spelling)
                }
            }
            return String(u8"any");
        }

        // Appends a Luau parameter list `(p0: T0, p1: T1, ...)` from a method/constructor's params,
        // using the A6 param names (or `arg<N>` when a name is absent). `skipSelf` drops nothing
        // here - instance methods get an explicit `self` prepended by the caller.
        void AppendLuauParamList(String& out, const ParamInfo* params, u32 paramCount,
                                 Span<const TypeInfo* const> declared)
        {
            out += u8"(";
            for (u32 p = 0; p < paramCount; ++p)
            {
                if (p > 0)
                {
                    out += u8", ";
                }
                const ParamInfo& param = params[p];
                if (param.name != nullptr && param.name[0] != '\0')
                {
                    out += ViewOf(param.name);
                }
                else
                {
                    out += Format(u8"arg{}", p);
                }
                out += u8": ";
                out += LuauTypeName(param.type != nullptr ? param.type() : nullptr, declared);
            }
            out += u8")";
        }

    }

    // =====================================================================
    // Bytecode blob: a compiled Luau chunk (the Bytecode capability).
    // IScriptManager::CompileToBlob produces one at COOK; IScriptContext::LoadBlob
    // feeds it back to luau_load in the PLAYER, so the cooked pack carries bytecode
    // and no compiler runs at load (the sandbox win). Bytecode is version-locked: the
    // cook fingerprint carries the vendored Luau version so a vendor bump recooks.
    // Opaque per IScriptBlob's contract - only this
    // backend produces/consumes it, so LoadBlob downcasts by static_cast.
    // =====================================================================
    class LuauScriptBlob final : public IScriptBlob
    {
    public:
        [[nodiscard]] Span<const byte> Bytecode() const noexcept
        {
            return Span<const byte>{m_bytecode.Data(), m_bytecode.Size()};
        }
        void SetBytecode(const byte* data, usize size)
        {
            m_bytecode.Resize(size);
            if (size > 0)
            {
                std::memcpy(m_bytecode.Data(), data, size);
            }
        }

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "bytecode", m_bytecode);
        }

    private:
        Array<byte> m_bytecode;
    };

    // =====================================================================
    // Delegate: a registry-ref'd Lua function. Holding the RefPtr keeps the
    // function GC-alive (lua_ref pins it); Detach() is called by the owning
    // context when the lua_State closes, after which Invoke fails cleanly -
    // the "error if the owning context is gone" contract.
    // =====================================================================
    class LuauScriptDelegate final : public IScriptDelegate
    {
    public:
        LuauScriptDelegate(LuauScriptContext* context, lua_State* state, int functionRef)
            : m_context(context), m_state(state), m_functionRef(functionRef)
        {
        }
        ~LuauScriptDelegate() override;

        void Detach() noexcept
        {
            m_context = nullptr;
            m_state = nullptr;
            m_functionRef = LUA_NOREF;
        }

        [[nodiscard]] Result<Variant> Invoke(Span<Variant> args) override;

    private:
        LuauScriptContext* m_context; // null after Detach
        lua_State* m_state;
        int m_functionRef;
    };

    // =====================================================================
    // ScriptObject: a registry-ref'd Lua table (an instance of a script
    // class). Retains the context.
    // =====================================================================
    class LuauScriptObject final : public ScriptObject
    {
    public:
        LuauScriptObject(RefPtr<LuauScriptContext> context, int tableRef)
            : m_context(Move(context)), m_tableRef(tableRef)
        {
        }
        ~LuauScriptObject() override;

        [[nodiscard]] Result<Variant> Invoke(StringView method, Span<Variant> args) override;
        [[nodiscard]] Result<Variant> GetProperty(StringView name) override;

        // Identity of the instance table (coroutine ownership matching).
        [[nodiscard]] const void* TablePointer() const;

    private:
        RefPtr<LuauScriptContext> m_context;
        int m_tableRef;
    };

    // =====================================================================
    // Context: one isolated lua_State.
    // =====================================================================
    class LuauScriptContext final : public IScriptContext
    {
    public:
        explicit LuauScriptContext(RefPtr<LuauScriptManager> manager);
        ~LuauScriptContext() override;

        void SetErrorHandler(IScriptErrorHandler* handler) override { m_errors = handler; }
        Status Load(StringView source, StringView chunkName) override;
        Status LoadBlob(IScriptBlob& blob) override;
        Status LoadBehaviorModule(Span<const BehaviorModuleClass> classes,
                                  StringView moduleName) override;
        void SetGlobal(StringView name, const Variant& value) override;
        [[nodiscard]] Variant GetGlobal(StringView name) override;
        [[nodiscard]] bool HasFunction(StringView name) const override;
        [[nodiscard]] Result<Variant> Call(StringView function, Span<Variant> args) override;
        [[nodiscard]] RefPtr<ScriptObject> CreateInstance(StringView className,
                                                          Span<Variant> args) override;

        [[nodiscard]] lua_State* State() noexcept { return m_state; }
        [[nodiscard]] LuauScriptManager* Manager() noexcept { return m_manager.Get(); }

        // The debugger installs the VM-global debugstep callback here: it fires per
        // Lua line while a thread has singlestep armed. Singlestep itself is armed per-thread at
        // resume time (RunCallable / ResumeCoroutine) only while a debugger is attached.
        void InstallDebugHooks(LuauDebugger* debugger);
        void RemoveDebugHooks();

        void ReportError(ScriptErrorKind kind, StringView module, StringView message);
        // The message a failed pcall/load left on top of the stack; pops it.
        void ReportTopOfStack(ScriptErrorKind kind, StringView module);

        // luau_load already-compiled bytecode as chunk `=chunkName`, then run its top level.
        // Shared by source Load (after luau_compile), the per-class bytecode path in
        // LoadBehaviorModule, and LoadBlob - one place owns the chunkname + error identity.
        Status LoadCompiledChunk(const byte* bytecode, usize size, StringView chunkName);

        // Reconstruct a cooked blob from its SERIALIZED bytes (the pack's ScriptClass::bytecode -
        // CompileToBlob -> Serialize) and load its bytecode as chunk `=chunkName`. The per-class
        // bytecode path in LoadBehaviorModule (the player: no luau_compile, per-class identity).
        Status LoadSerializedBlobChunk(Span<const byte> serialized, StringView chunkName);

        void TrackDelegate(LuauScriptDelegate* delegate) { m_delegates.PushBack(delegate); }
        void UntrackDelegate(LuauScriptDelegate* delegate)
        {
            for (usize i = 0; i < m_delegates.Size(); ++i)
            {
                if (m_delegates[i] == delegate)
                {
                    m_delegates.RemoveAt(i);
                    return;
                }
            }
        }

        // ---- marshalling (public: the emitter's C closures use these) ----
        void PushVariant(lua_State* state, const Variant& value);
        [[nodiscard]] Variant ToVariant(lua_State* state, int index);
        // Like ToVariant, but honours the reflected parameter/property TYPE the value is bound to:
        // an enum-typed target takes a Lua number carried as i64 (the neutral dispatch casts it to
        // the enum). Use at every native call/setter site where the expected type is known.
        [[nodiscard]] Variant ToVariantForParam(lua_State* state, int index, const TypeInfo* expected);

        // ---- the resumable-thread executor ----
        // Every script CALL (Call / CreateInstance / ScriptObject::Invoke) runs on a POOLED lua
        // thread via lua_resume, NOT lua_pcall on the main state - one executor for the production
        // AND the debugged program (no divergence), and the only shape lua_break (the debugger's
        // suspend) can suspend + resume. AcquireThread reuses a recycled thread or makes one
        // (registry-pinned for GC); a nested call (a facade that dispatches script) takes a
        // second thread, so pool depth = nesting depth. RunCallable moves the callable+args from
        // m_state to a thread, resumes, and (on success) brings the single result back to m_state.
        [[nodiscard]] lua_State* AcquireThread();
        void ReleaseThread(lua_State* thread);
        // `total` = the callable + its args currently on top of m_state. Returns the lua_resume
        // status: LUA_OK leaves the result on m_state; an error leaves the message on m_state.
        [[nodiscard]] int RunCallable(int total);

    private:
        void EmitRegisteredTypes();
        void EmitType(const TypeInfo& type);
        // A reflected enum as a named constant table: _G[EnumName] = { ValueName = <int>, ... }.
        void EmitEnum(const TypeInfo& type);
        void InstallCoroutineApi();

        RefPtr<LuauScriptManager> m_manager;
        lua_State* m_state = nullptr;
        IScriptErrorHandler* m_errors = nullptr;
        Array<LuauScriptDelegate*> m_delegates; // borrowed; Detach()ed at close
        Array<lua_State*> m_freeThreads; // recyclable resumable threads (all registry-pinned)
    };

    // =====================================================================
    // Manager.
    // =====================================================================
    class LuauScriptManager final : public IScriptManager
    {
    public:
        ~LuauScriptManager() override
        {
            DIAGNOSTIC_ASSERT(m_contexts.IsEmpty()); // contexts retain the manager
        }

        void RegisterType(const TypeInfo& type) override
        {
            for (const TypeInfo* existing : m_types)
            {
                if (existing == &type)
                {
                    return;
                }
            }
            m_types.PushBack(&type);
        }

        void FinalizeTypes() override
        {
            // The overload contract: fail loudly if any registered type binds two methods to the
            // same script name (see foundation.script ValidateScriptMethodNames).
            ValidateScriptMethodNames(RegisteredTypes());
            // ...and fail loudly if two types bind to the same script-facing name (class name or
            // scriptName alias) - the alias mechanism's collision guard.
            ValidateScriptTypeNames(RegisteredTypes());
        }

        [[nodiscard]] RefPtr<IScriptContext> CreateContext() override;

        void CollectGarbage() override
        {
            for (LuauScriptContext* context : m_contexts)
            {
                lua_gc(context->State(), LUA_GCCOLLECT, 0);
            }
        }

        [[nodiscard]] ScriptCapabilities Capabilities() const override
        {
            return ScriptCapabilities::Coroutines | ScriptCapabilities::Delegates |
                   ScriptCapabilities::Bytecode | ScriptCapabilities::Debugger;
        }

        // A step debugger over this VM's contexts: singlestep + (short_src,line) break
        // on the pooled resumable threads, held via lua_break. One at a time; it registers as the
        // active debugger on construction (RunCallable/AdvanceCoroutines consult it) and
        // unregisters on destruction. Defined after the context class.
        [[nodiscard]] UniquePtr<IScriptDebugger> CreateDebugger() override;

        void SetActiveDebugger(LuauDebugger* debugger) noexcept { m_debugger = debugger; }
        [[nodiscard]] LuauDebugger* ActiveDebugger() const noexcept { return m_debugger; }
        [[nodiscard]] Span<LuauScriptContext* const> Contexts() const noexcept
        {
            return Span<LuauScriptContext* const>{m_contexts.Data(), m_contexts.Size()};
        }

        // Compile source to a Luau bytecode blob (the cook side of the Bytecode seam). The
        // Luau compiler embeds compile errors in the bytecode - they surface at luau_load -
        // so this validates the result in a throwaway state and fails the COOK rather than
        // shipping a chunk that faults on the player's LoadBlob.
        [[nodiscard]] Result<RefPtr<IScriptBlob>> CompileToBlob(StringView source,
                                                               StringView chunkName) override;

        [[nodiscard]] RefPtr<IScriptBlob> CreateBlob() override
        {
            return RefPtr<IScriptBlob>(MakeRef<LuauScriptBlob>(MemoryAllocator()).Get());
        }

        [[nodiscard]] Array<ScriptApiType> DescribeBoundApi() const override;

        void AdvanceCoroutines(f64 deltaSeconds) override;
        void CancelCoroutinesFor(ScriptObject& instance) override;

        // ---- backend internals ----
        [[nodiscard]] Span<const TypeInfo* const> RegisteredTypes() const noexcept
        {
            return Span<const TypeInfo* const>{m_types.Data(), m_types.Size()};
        }
        // A context created WHILE a debugger is attached must get its debug hooks too (the
        // debugger only hooked the contexts that existed at CreateDebugger time). Out-of-line so it
        // can reach into the context type.
        void TrackContext(LuauScriptContext* context);
        void UntrackContext(LuauScriptContext* context)
        {
            DropCoroutinesOf(context);
            for (usize i = 0; i < m_contexts.Size(); ++i)
            {
                if (m_contexts[i] == context)
                {
                    m_contexts.RemoveAt(i);
                    return;
                }
            }
        }

        // One scheduled coroutine: a registry-ref'd lua thread waiting on a timer or a
        // predicate. `owner` is the instance table's pointer identity (CancelCoroutinesFor).
        struct Coroutine
        {
            LuauScriptContext* context = nullptr;
            lua_State* thread = nullptr;
            int threadRef = LUA_NOREF;
            const void* owner = nullptr;
            f64 remainingSeconds = 0.0;
            int predicateRef = LUA_NOREF; // LUA_NOREF = timed wait
            bool started = false;         // first resume pending?
        };
        Array<Coroutine> coroutines;

        // The entry whose thread is `state` (the wait primitives run ON the thread).
        [[nodiscard]] Coroutine* FindCoroutine(lua_State* state)
        {
            for (Coroutine& entry : coroutines)
            {
                if (entry.thread == state)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // The debugger's Continue on a broken COROUTINE thread routes through the same
        // ProcessCoroutineResume as the scheduler, then drops the entry if it finished (Fable Q6).
        void ContinueCoroutine(Coroutine& entry, int status);

    private:
        void DropCoroutinesOf(LuauScriptContext* context);
        void DropCoroutineAt(usize index);
        // Resume one entry; true when it should be REMOVED (finished or faulted).
        [[nodiscard]] bool ResumeCoroutine(Coroutine& entry);

        // The ONE post-resume router shared by the coroutine scheduler and the debugger's Continue
        // (Fable Q6) so they never disagree: BREAK -> the debugger holds the thread (false, kept),
        // YIELD -> stays scheduled (false), OK/error -> drop (true). ClassifyResume is its lone map.
        enum class ResumeDisposition
        {
            Completed,
            Rescheduled,
            Broke,
            Faulted
        };
        [[nodiscard]] static ResumeDisposition ClassifyResume(int status) noexcept
        {
            switch (status)
            {
            case LUA_OK:
                return ResumeDisposition::Completed;
            case LUA_YIELD:
                return ResumeDisposition::Rescheduled;
            case LUA_BREAK:
                return ResumeDisposition::Broke;
            default:
                return ResumeDisposition::Faulted;
            }
        }
        [[nodiscard]] bool ProcessCoroutineResume(Coroutine& entry, int status);

        Array<const TypeInfo*> m_types;
        Array<LuauScriptContext*> m_contexts; // borrowed; contexts retain us
        LuauDebugger* m_debugger = nullptr;   // borrowed; the active step debugger (self-registers)
    };

    // =====================================================================
    // Step debugger: singlestep + (short_src, line) breakpoints on the pooled
    // resumable threads. The debugstep callback lua_break()s the running thread on a hit; the
    // resume returns LUA_BREAK, the debugger HOLDS that thread, fires the paused state (the run
    // host freezes the game), and Continue/Step re-resume it. lua_break cannot cross a C-call
    // boundary (lua_isyieldable == 0 there), so a break wanted mid-facade is DEFERRED to the next
    // safe line. Script frames only - stepping never enters engine C.
    // =====================================================================
    class LuauDebugger final : public IScriptDebugger
    {
    public:
        explicit LuauDebugger(LuauScriptManager* manager) : m_manager(manager)
        {
            if (m_manager != nullptr)
            {
                m_manager->SetActiveDebugger(this);
                for (LuauScriptContext* context : m_manager->Contexts())
                {
                    context->InstallDebugHooks(this);
                }
            }
        }

        ~LuauDebugger() override
        {
            if (m_manager != nullptr)
            {
                for (LuauScriptContext* context : m_manager->Contexts())
                {
                    context->RemoveDebugHooks();
                }
                if (m_heldThread != nullptr && m_heldContext != nullptr)
                {
                    m_heldContext->ReleaseThread(m_heldThread); // no Abort; just stop holding it
                }
                m_manager->SetActiveDebugger(nullptr);
            }
        }

        LuauDebugger(const LuauDebugger&) = delete;
        LuauDebugger& operator=(const LuauDebugger&) = delete;

        // ---- IScriptDebugger ----
        void SetBreakpoint(StringView file, i32 line) override
        {
            for (const Breakpoint& bp : m_breakpoints)
            {
                if (bp.line == line && bp.file.AsView() == file)
                {
                    return;
                }
            }
            m_breakpoints.PushBack(Breakpoint{String(file), line});
        }
        void RemoveBreakpoint(StringView file, i32 line) override
        {
            for (usize i = 0; i < m_breakpoints.Size(); ++i)
            {
                if (m_breakpoints[i].line == line && m_breakpoints[i].file.AsView() == file)
                {
                    m_breakpoints.RemoveAt(i);
                    return;
                }
            }
        }
        void Break() override { m_breakNext = true; }
        void Continue() override { Resume(StepMode::None); }
        void StepInto() override { Resume(StepMode::Into); }
        void StepOver() override { Resume(StepMode::Over); }

        [[nodiscard]] Array<ScriptStackFrame> CaptureStackFrames() override
        {
            Array<ScriptStackFrame> frames;
            if (m_heldThread == nullptr)
            {
                return frames;
            }
            const int depth = lua_stackdepth(m_heldThread);
            for (int level = 0; level < depth; ++level) // 0 = innermost
            {
                lua_Debug info;
                if (lua_getinfo(m_heldThread, level, "sln", &info) == 0)
                {
                    continue;
                }
                ScriptStackFrame frame;
                // Frame 0 is the pause point (the line ABOUT to run); lua_getinfo after the break
                // reports the restored pc's line, off by one, so use the stored broken line there.
                frame.line = (level == 0) ? m_brokenLine : info.currentline;
                frame.file = String(ViewOf(info.short_src));
                frame.function =
                    String((info.name != nullptr) ? ViewOf(info.name) : StringView(u8"?"));
                frames.PushBack(Move(frame));
            }
            return frames;
        }

        [[nodiscard]] Array<ScriptVariable> CaptureLocals(u32 depth) override
        {
            Array<ScriptVariable> locals;
            if (m_heldThread == nullptr)
            {
                return locals;
            }
            for (int n = 1;; ++n)
            {
                const char* name = lua_getlocal(m_heldThread, static_cast<int>(depth), n);
                if (name == nullptr)
                {
                    break; // no more locals visible at this frame
                }
                // lua_getlocal pushed the value on the held thread; describe + pop it.
                ScriptVariable variable;
                variable.name = String(ViewOf(name));
                DescribeLuaValue(variable, m_heldThread, -1);
                lua_pop(m_heldThread, 1);
                if (name[0] != '(') // skip "(temporary)" internal slots
                {
                    locals.PushBack(Move(variable));
                }
            }
            return locals;
        }

        [[nodiscard]] Array<ScriptVariable> CaptureObject(u64 objectRef) override
        {
            Array<ScriptVariable> members;
            Variant* stored = FindObject(objectRef);
            if (stored == nullptr)
            {
                return members;
            }
            const TypeInfo* type = stored->Type();
            if (type == nullptr)
            {
                return members;
            }
            Instance instance = ToInstance(*stored);
            for (usize i = 0; i < PropertyCount(*type); ++i)
            {
                const PropertyInfo& property = PropertyAt(*type, i);
                if (IsNested(property))
                {
                    continue;
                }
                ScriptVariable variable;
                variable.name = String(ViewOf(property.name));
                variable.typeName = String(
                    (property.type != nullptr) ? ViewOf(property.type->name) : StringView(u8"?"));
                DescribeVariant(variable, GetProperty(property, instance));
                members.PushBack(Move(variable));
            }
            return members;
        }

        void SetListener(IScriptDebuggerListener* listener) override { m_listener = listener; }

        // ---- called by the executor + the debugstep callback ----

        // The debugstep callback (control is INSIDE the resumed thread). Decide whether THIS line
        // breaks; lua_break here when safe, else DEFER to the next safe line (C-call boundary).
        void OnStep(lua_State* thread, lua_Debug* ar)
        {
            if (m_heldThread != nullptr)
            {
                return; // one break at a time; a re-entrant call during a pause runs through
            }
            const int line = ar->currentline; // the pause point (the line about to execute)
            const int depth = lua_stackdepth(thread);
            // Re-arm the breakpoint on the line we resumed from once execution genuinely LEAVES it
            // (so a loop back re-breaks); until then skip it, so Continue/Step does not re-break
            // where it paused. A call on that line (which dips DEEPER and returns to the same line
            // for the store, e.g. `x = f()`) must NOT count as leaving - only a move to a different
            // line at the resume depth or shallower does.
            if (m_resumeLine >= 0 && depth <= m_resumeDepth && line != m_resumeLine)
            {
                m_resumeLine = -1;
            }
            const bool atResumePoint =
                (m_resumeLine >= 0 && line == m_resumeLine && depth == m_resumeDepth);

            lua_Debug info;
            const StringView src = (lua_getinfo(thread, 0, "s", &info) != 0) ? ViewOf(info.short_src)
                                                                            : StringView{};
            Cause cause = Cause::Breakpoint;
            bool want = false;
            if (m_breakNext)
            {
                want = true;
                cause = Cause::Step;
                m_breakNext = false;
            }
            else if (m_stepArmed)
            {
                const bool depthOk = (m_stepMode == StepMode::Into) || (depth <= m_stepFromDepth);
                const bool moved = (line != m_stepFromLine) || (depth != m_stepFromDepth);
                if (depthOk && moved)
                {
                    want = true;
                    cause = Cause::Step;
                }
            }
            if (!want && !atResumePoint && IsBreakpoint(src, line))
            {
                want = true;
                cause = Cause::Breakpoint;
            }
            if (!want && m_pending)
            {
                want = true;
                cause = m_pendingCause;
            }
            if (!want)
            {
                return;
            }
            if (lua_isyieldable(thread) == 0)
            {
                // Across a C-call boundary lua_break would runtime-error (Fable Q3): remember the
                // wanted break and take it at the next safe line (back on the resumable frame).
                m_pending = true;
                m_pendingCause = cause;
                return;
            }
            m_pending = false;
            m_stepArmed = false;
            m_resumeLine = -1;
            m_cause = cause;
            m_brokenLine = line;
            lua_break(thread);
        }

        // The executor calls this after a resume returned LUA_BREAK: OWN the broken thread + fire.
        void OnBroke(LuauScriptContext* context, lua_State* thread)
        {
            m_heldThread = thread;
            m_heldContext = context;
            FireState(m_cause == Cause::Step ? ScriptDebuggerState::Stepped
                                             : ScriptDebuggerState::Breakpoint);
        }

        [[nodiscard]] bool HasHeldThread() const noexcept { return m_heldThread != nullptr; }

    private:
        enum class StepMode
        {
            None,
            Into,
            Over
        };
        enum class Cause
        {
            Breakpoint,
            Step
        };
        struct Breakpoint
        {
            String file;
            i32 line = -1;
        };
        struct CapturedObject
        {
            u64 ref = 0;
            Variant value;
        };

        [[nodiscard]] bool IsBreakpoint(StringView src, int line) const
        {
            for (const Breakpoint& bp : m_breakpoints)
            {
                if (bp.line == line && bp.file.AsView() == src)
                {
                    return true;
                }
            }
            return false;
        }

        void FireState(ScriptDebuggerState state)
        {
            if (m_listener != nullptr)
            {
                m_listener->OnDebuggerStateChanged(state);
            }
        }

        // Re-resume the held thread. None = run to next break/completion; Into/Over arm a one-line
        // step from the current (line, depth). Completion returns the thread to its pool; another
        // break re-holds it.
        void Resume(StepMode mode)
        {
            if (m_heldThread == nullptr || m_heldContext == nullptr)
            {
                return;
            }
            lua_State* thread = m_heldThread;
            LuauScriptContext* context = m_heldContext;
            if (mode == StepMode::None)
            {
                m_stepArmed = false;
            }
            else
            {
                m_stepMode = mode;
                m_stepFromDepth = lua_stackdepth(thread);
                // Right after a break lua_getinfo reports the restored pc's line (off by one, same
                // reason CaptureStackFrames uses m_brokenLine for frame 0); the pause point IS the
                // line we resume from, so step from there or a StepOver would break on it again.
                m_stepFromLine = m_brokenLine;
                m_stepArmed = true;
            }
            // Skip the breakpoint on the line/frame we resume from until execution leaves it, so
            // Continue/Step does not immediately re-break where it was already paused.
            m_resumeLine = m_brokenLine;
            m_resumeDepth = lua_stackdepth(thread);
            // A broken thread is either a scheduled coroutine or a pooled executor thread; they
            // route differently after the resume (a coroutine may YIELD back to the scheduler or,
            // when done, be dropped from the schedule; a pooled thread returns to its pool). Decide
            // before the resume for the `from` argument, then route on the result.
            const bool isCoroutine = (m_manager->FindCoroutine(thread) != nullptr);
            m_heldThread = nullptr;
            m_heldContext = nullptr;
            m_objects.Clear(); // captured object refs are valid only for the current break
            FireState(ScriptDebuggerState::Running);

            int status;
            {
                ScriptCallScope scope(context);
                status = lua_resume(thread, isCoroutine ? nullptr : context->State(), 0);
            }
            if (isCoroutine)
            {
                // Re-find (a resumed body may have spawned coroutines and reallocated the array);
                // then route through the SAME ProcessCoroutineResume the scheduler uses (Fable Q6):
                // BREAK re-holds (OnBroke re-fires paused), YIELD stays scheduled (game runs on),
                // OK/error drops.
                if (LuauScriptManager::Coroutine* entry = m_manager->FindCoroutine(thread))
                {
                    m_manager->ContinueCoroutine(*entry, status);
                }
                // A finished coroutine ends this debugged run (Terminated); a yield leaves it
                // scheduled (stay Running); a break already re-fired Breakpoint/Stepped.
                if (status != LUA_BREAK && status != LUA_YIELD)
                {
                    FireState(ScriptDebuggerState::Terminated);
                }
                return;
            }
            if (status == LUA_BREAK)
            {
                OnBroke(context, thread); // hit the next break/step - hold + fire again
                return;
            }
            if (status != LUA_OK && status != LUA_YIELD)
            {
                context->ReportTopOfStack(ScriptErrorKind::Runtime, u8"debug");
            }
            context->ReleaseThread(thread); // completed pooled executor thread -> back to the pool
            FireState(ScriptDebuggerState::Terminated); // the debugged call ran to completion
        }

        // ---- value description ----
        [[nodiscard]] u64 StoreObject(const Variant& value)
        {
            const u64 ref = m_nextObjectRef++;
            m_objects.PushBack(CapturedObject{ref, value});
            return ref;
        }
        [[nodiscard]] Variant* FindObject(u64 ref)
        {
            for (CapturedObject& object : m_objects)
            {
                if (object.ref == ref)
                {
                    return &object.value;
                }
            }
            return nullptr;
        }

        // A reflected Variant -> display text + (if a reflected object with properties) an
        // expandable objectRef.
        void DescribeVariant(ScriptVariable& variable, const Variant& value)
        {
            const TypeInfo* type = value.Type();
            if (const f64* n = value.TryGet<f64>())
            {
                variable.value = Format(u8"{}", *n);
            }
            else if (const bool* b = value.TryGet<bool>())
            {
                variable.value = String(*b ? u8"true" : u8"false");
            }
            else if (const String* s = value.TryGet<String>())
            {
                variable.value = *s;
            }
            else if (const Float3* v = value.TryGet<Float3>())
            {
                variable.value = Format(u8"({}, {}, {})", v->x, v->y, v->z);
            }
            else if (type != nullptr && PropertyCount(*type) > 0)
            {
                variable.value = String(ViewOf(type->name));
                variable.objectRef = StoreObject(value);
            }
            else if (type != nullptr)
            {
                variable.value = String(ViewOf(type->name));
            }
            else
            {
                variable.value = String(u8"nil");
            }
        }

        // A raw Lua stack value -> ScriptVariable (avoids ToVariant's function->delegate side
        // effect; a userdata box is the reflected object, made expandable).
        void DescribeLuaValue(ScriptVariable& variable, lua_State* thread, int index)
        {
            switch (lua_type(thread, index))
            {
            case LUA_TNIL:
                variable.typeName = String(u8"nil");
                variable.value = String(u8"nil");
                break;
            case LUA_TBOOLEAN:
                variable.typeName = String(u8"boolean");
                variable.value = String(lua_toboolean(thread, index) != 0 ? u8"true" : u8"false");
                break;
            case LUA_TNUMBER:
                variable.typeName = String(u8"number");
                variable.value = Format(u8"{}", lua_tonumber(thread, index));
                break;
            case LUA_TVECTOR:
            {
                const float* v = lua_tovector(thread, index);
                variable.typeName = String(u8"vector");
                variable.value = (v != nullptr) ? Format(u8"({}, {}, {})", v[0], v[1], v[2])
                                                : String(u8"?");
                break;
            }
            case LUA_TSTRING:
            {
                size_t length = 0;
                const char* s = lua_tolstring(thread, index, &length);
                variable.typeName = String(u8"string");
                variable.value = String(StringView(reinterpret_cast<const utf8char*>(s), length));
                break;
            }
            case LUA_TFUNCTION:
                variable.typeName = String(u8"function");
                variable.value = String(u8"<function>");
                break;
            case LUA_TTABLE:
                variable.typeName = String(u8"table");
                variable.value = String(u8"<table>");
                break;
            case LUA_TUSERDATA:
            {
                // Every userdata in this backend is a Variant box (lua_newuserdatadtor + ~Variant).
                Variant* boxed = static_cast<Variant*>(lua_touserdata(thread, index));
                if (boxed != nullptr && boxed->Type() != nullptr)
                {
                    DescribeVariant(variable, *boxed);
                    variable.typeName = String(ViewOf(boxed->Type()->name));
                }
                else
                {
                    variable.typeName = String(u8"userdata");
                    variable.value = String(u8"<userdata>");
                }
                break;
            }
            default:
                variable.typeName = String(u8"value");
                variable.value = String(u8"?");
                break;
            }
        }

        LuauScriptManager* m_manager = nullptr;
        IScriptDebuggerListener* m_listener = nullptr;
        Array<Breakpoint> m_breakpoints;
        lua_State* m_heldThread = nullptr;
        LuauScriptContext* m_heldContext = nullptr;
        bool m_breakNext = false;
        bool m_stepArmed = false;
        StepMode m_stepMode = StepMode::None;
        int m_stepFromLine = -1;
        int m_stepFromDepth = 0;
        int m_brokenLine = -1; // the pause point line (frame-0 line; the callhook off-by-one target)
        int m_resumeLine = -1; // the line Continue/Step resumed from - skip its breakpoint until we leave it
        int m_resumeDepth = 0;
        bool m_pending = false; // a break deferred past a C-call boundary
        Cause m_pendingCause = Cause::Breakpoint;
        Cause m_cause = Cause::Breakpoint;
        Array<CapturedObject> m_objects;
        u64 m_nextObjectRef = 1;
    };

    // =====================================================================
    // Marshalling
    // =====================================================================
    namespace
    {
        // Full userdata payload: an in-place Variant (value or object mode). The
        // userdata destructor (lua_newuserdatadtor) runs ~Variant - releases any
        // RefPtr the object mode holds - with no __gc plumbing.
        void DestroyVariantUserdata(void* payload) { static_cast<Variant*>(payload)->~Variant(); }

        [[nodiscard]] Variant* VariantAt(lua_State* state, int index)
        {
            if (lua_type(state, index) != LUA_TUSERDATA)
            {
                return nullptr;
            }
            return static_cast<Variant*>(lua_touserdata(state, index));
        }
    }

    void LuauScriptContext::PushVariant(lua_State* state, const Variant& value)
    {
        if (value.IsEmpty())
        {
            lua_pushnil(state);
            return;
        }
        if (const bool* b = value.TryGet<bool>())
        {
            lua_pushboolean(state, *b ? 1 : 0);
            return;
        }
        if (const f64* d = value.TryGet<f64>())
        {
            lua_pushnumber(state, *d);
            return;
        }
        if (const f32* f = value.TryGet<f32>())
        {
            lua_pushnumber(state, static_cast<f64>(*f));
            return;
        }
        if (const i32* i = value.TryGet<i32>())
        {
            lua_pushnumber(state, static_cast<f64>(*i));
            return;
        }
        if (const i64* i = value.TryGet<i64>())
        {
            lua_pushnumber(state, static_cast<f64>(*i));
            return;
        }
        if (const u32* u = value.TryGet<u32>())
        {
            lua_pushnumber(state, static_cast<f64>(*u));
            return;
        }
        if (const String* s = value.TryGet<String>())
        {
            lua_pushlstring(state, reinterpret_cast<const char*>(s->CStr()), s->Size());
            return;
        }
        // Float3 maps to Luau's NATIVE vector type (LUA_VECTOR_SIZE == 3, exact match): scripts get
        // v.x/.y/.z and vector arithmetic with NO per-value userdata allocation - the Luau-specific
        // fast path that replaces the boxed reflected handle the other backends use.
        if (const Float3* v = value.TryGet<Float3>())
        {
            lua_pushvector(state, v->x, v->y, v->z);
            return;
        }
        // An enum crosses as its underlying number: Lua has no enum type, the emitter excludes
        // enums from binding, and an enum parameter converts the number back (mirrors
        // AngelScript). Must come before the boxed-Variant fallthrough (an enum Variant is not a
        // primitive TryGet match).
        if (const TypeInfo* enumType = value.Type();
            enumType != nullptr && enumType->enumeratorCount > 0)
        {
            lua_pushnumber(state, static_cast<f64>(value.AsEnumInt()));
            return;
        }
        // A reflected container (Array<T>) crosses as a native 1-indexed Lua table (script gets `#t`
        // and ipairs); each element is pushed recursively - numbers, strings, boxed reflected handles.
        // GC-managed, no manual refcount. The facade returns an engine Array<T>.
        // Must precede the boxed-Variant fallthrough.
        if (const TypeInfo* containerType = value.Type();
            containerType != nullptr && IsContainer(*containerType))
        {
            const ContainerInfo& ci = *containerType->container;
            Variant holder = value; // ToInstance needs a mutable lvalue; the copy is cheap
            const Instance inst = ToInstance(holder);
            const usize count = (inst.Pointer() != nullptr) ? ci.size(inst) : usize{0};
            lua_createtable(state, static_cast<int>(count), 0);
            for (usize i = 0; i < count; ++i)
            {
                PushVariant(state, ci.getAt(inst, i));
                lua_rawseti(state, -2, static_cast<int>(i) + 1);
            }
            return;
        }
        // Everything else (objects, reflected values) travels as a boxed Variant with the
        // dynamic type's dispatch metatable, so script sees properties/methods directly.
        void* payload = lua_newuserdatadtor(state, sizeof(Variant), DestroyVariantUserdata);
        new (payload) Variant(value);
        const TypeInfo* type = value.Type();
        if (type != nullptr)
        {
            // The per-type metatable was created at emission when the type is bound; a
            // NON-bound type still gets a plain metatable-free userdata (opaque handle).
            lua_pushlightuserdata(state, const_cast<TypeInfo*>(type));
            lua_rawget(state, LUA_REGISTRYINDEX);
            if (lua_istable(state, -1))
            {
                lua_setmetatable(state, -2);
            }
            else
            {
                lua_pop(state, 1);
            }
        }
    }

    Variant LuauScriptContext::ToVariant(lua_State* state, int index)
    {
        switch (lua_type(state, index))
        {
        case LUA_TNIL:
            return Variant{};
        case LUA_TBOOLEAN:
            return Variant::From<bool>(lua_toboolean(state, index) != 0);
        case LUA_TNUMBER:
            return Variant::From<f64>(lua_tonumber(state, index));
        case LUA_TVECTOR:
        {
            // A native Luau vector crosses back as a Float3 (exact 3x f32).
            const float* v = lua_tovector(state, index);
            return (v != nullptr) ? Variant::From<Float3>(Float3(v[0], v[1], v[2])) : Variant{};
        }
        case LUA_TSTRING:
        {
            size_t length = 0;
            const char* text = lua_tolstring(state, index, &length);
            String value;
            value.Append(reinterpret_cast<const utf8char*>(text), length);
            return Variant::From<String>(Move(value));
        }
        case LUA_TUSERDATA:
        {
            if (const Variant* boxed = VariantAt(state, index))
            {
                return *boxed;
            }
            return Variant{};
        }
        case LUA_TFUNCTION:
        {
            // A script function crossing into native = a delegate (the reflected param
            // side matches it by the IScriptDelegate object type).
            lua_pushvalue(state, index);
            const int ref = lua_ref(state, -1);
            lua_pop(state, 1);
            RefPtr<LuauScriptDelegate> delegate = MakeRef<LuauScriptDelegate>(
                MemoryAllocator(), this, m_state, ref);
            TrackDelegate(delegate.Get());
            return Variant::From<RefPtr<Object>>(RefPtr<Object>(delegate.Get()));
        }
        default:
            return Variant{};
        }
    }

    Variant LuauScriptContext::ToVariantForParam(lua_State* state, int index,
                                                 const TypeInfo* expected)
    {
        // A table for an Array<E> parameter: its sequence (1..n) as an Array<Variant>, each element
        // narrowed to E as a parameter of that type would be; the reflection call converts the list
        // into the parameter's Array<E>.
        if (expected != nullptr && IsContainer(*expected) && expected->container->elementType != nullptr &&
            lua_type(state, index) == LUA_TTABLE)
        {
            const int table = lua_absindex(state, index);
            const int count = lua_objlen(state, table);
            Array<Variant> list;
            list.Reserve(static_cast<usize>(count));
            for (int i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, table, i);
                list.PushBack(ToVariantForParam(state, -1, expected->container->elementType));
                lua_pop(state, 1);
            }
            return Variant::From<Array<Variant>>(Move(list));
        }
        // A Lua number narrows to the reflected param's EXACT numeric/enum type. Lua numbers are all
        // f64, but the neutral dispatch matches the declared param type, so an f32/i32/enum param
        // must receive that typed Variant - not a bare f64 (which it would reject). Mirrors
        // AngelScript's expected-type marshalling.
        if (expected != nullptr && lua_type(state, index) == LUA_TNUMBER)
        {
            const f64 n = lua_tonumber(state, index);
            if (expected->enumeratorCount > 0)
            {
                return Variant::From<i64>(static_cast<i64>(n)); // enum: its underlying int
            }
            if (expected == &TypeOf<f32>())
            {
                return Variant::From<f32>(static_cast<f32>(n));
            }
            if (expected == &TypeOf<i32>())
            {
                return Variant::From<i32>(static_cast<i32>(n));
            }
            if (expected == &TypeOf<i64>())
            {
                return Variant::From<i64>(static_cast<i64>(n));
            }
            if (expected == &TypeOf<u32>())
            {
                return Variant::From<u32>(static_cast<u32>(n));
            }
            if (expected == &TypeOf<u64>())
            {
                return Variant::From<u64>(static_cast<u64>(n));
            }
            // f64 (or an unrecognised numeric) falls through to the ordinary conversion.
        }
        return ToVariant(state, index);
    }

    // =====================================================================
    // Reflected-type emission: one metatable per TypeInfo per state.
    //
    // Metatable layout:
    //   registry[TypeInfo*] = mt { __index = C(__index), __newindex = C(__newindex) }
    //   mt's C closures carry: upvalue1 = TypeInfo* (light), upvalue2 = context* (light),
    //   upvalue3 = methods table { name -> C(method) with upvalues MethodInfo*/context* }.
    // Class access: _G[Name] = class table { new = C(ctor), <statics> } so scripts write
    // Name.new(...) and instance:Method(...).
    // =====================================================================
    namespace
    {
        struct DispatchUpvalues
        {
            static constexpr int kType = 1;
            static constexpr int kContext = 2;
            static constexpr int kMethods = 3;
        };

        // Null-safe C-string equality (method/script names are null-terminated ASCII).
        [[nodiscard]] bool NameEquals(const char* a, const char* b)
        {
            if (a == b)
            {
                return true;
            }
            if (a == nullptr || b == nullptr)
            {
                return false;
            }
            while (*a != '\0' && *a == *b)
            {
                ++a;
                ++b;
            }
            return *a == *b;
        }

        [[nodiscard]] const PropertyInfo* FindPropertyInChain(const TypeInfo* type,
                                                              const char* name)
        {
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                if (const PropertyInfo* property = FindProperty(*t, name))
                {
                    return property;
                }
            }
            return nullptr;
        }

        // Marshal self (for instance methods) + args off the Lua stack and invoke `method`, pushing
        // its result. Shared by MethodThunk (one captured method) and ArityMethodThunk (resolved by
        // argc). Returns the Lua result count.
        int InvokeReflected(lua_State* state, LuauScriptContext* context, const MethodInfo& method)
        {
            const int argCount = lua_gettop(state);
            // The bound self is dispatched by POINTER into its userdata box (like the property
            // get/set thunks below), NOT a copy - so a MUTATING reflected method (e.g.
            // JsonValue.Set) persists to the script's value, matching the reference semantics
            // AngelScript gives asOBJ_REF reflected types. A copy here silently dropped mutations.
            Variant* boxedSelf = nullptr;
            Array<Variant> args;
            int firstArg = 1;
            if (!method.isStatic)
            {
                boxedSelf = VariantAt(state, 1);
                if (boxedSelf == nullptr)
                {
                    lua_pushstring(state, "method called without a bound self (use ':')");
                    lua_error(state);
                }
                firstArg = 2;
            }
            for (int i = firstArg; i <= argCount; ++i)
            {
                // Honour the reflected param type for this arg (enum -> number carried as i64);
                // extra args beyond the signature fall back to plain conversion.
                const u32 paramIndex = static_cast<u32>(i - firstArg);
                const TypeInfo* expected =
                    (paramIndex < method.paramCount && method.params[paramIndex].type != nullptr)
                        ? method.params[paramIndex].type()
                        : nullptr;
                args.PushBack(context->ToVariantForParam(state, i, expected));
            }

            ScriptCallScope scope(context);
            Result<Variant> result =
                method.isStatic
                    ? InvokeStatic(method, Span<Variant>{args.Data(), args.Size()})
                    : InvokeMethod(method, ToInstance(*boxedSelf),
                                   Span<Variant>{args.Data(), args.Size()});
            if (!result.HasValue())
            {
                lua_pushstring(state, "reflected method invocation failed");
                lua_error(state);
            }
            if (result.Value().IsEmpty())
            {
                return 0;
            }
            context->PushVariant(state, result.Value());
            return 1;
        }

        // How many methods on `type`'s chain bind to (script name, static-ness) - the ARITY FAMILY
        // size. 1 = a plain method; >1 = an arity family dispatched by argument count.
        int ScriptNameFamilySize(const TypeInfo* type, const char* scriptName, bool isStatic)
        {
            int count = 0;
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                for (const MethodInfo& m : Methods(*t))
                {
                    if (m.name != nullptr && m.isStatic == isStatic &&
                        NameEquals(ScriptMethodName(m), scriptName))
                    {
                        ++count;
                    }
                }
            }
            return count;
        }

        // The family member matching a given argument count (paramCount == argc), or null.
        const MethodInfo* ResolveArityMethod(const TypeInfo* type, const char* scriptName,
                                             bool isStatic, int argc)
        {
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                for (const MethodInfo& m : Methods(*t))
                {
                    if (m.name != nullptr && m.isStatic == isStatic &&
                        m.paramCount == static_cast<u32>(argc) &&
                        NameEquals(ScriptMethodName(m), scriptName))
                    {
                        return &m;
                    }
                }
            }
            return nullptr;
        }

        // Whether a reflected operator's method binds as a Luau metamethod: a static whose first
        // parameter is the type (the left operand), one parameter for Negate, two otherwise.
        [[nodiscard]] bool IsBindableOperator(const TypeInfo& type, const MethodInfo& method)
        {
            const bool unary = method.op == MethodOperator::Negate;
            return method.op != MethodOperator::None && method.isStatic &&
                   method.paramCount == (unary ? 1u : 2u) && method.params[0].type != nullptr &&
                   method.params[0].type() == &type;
        }

        // The metamethod a reflected operator binds as; null for None.
        [[nodiscard]] const char* OperatorMetamethod(MethodOperator op)
        {
            switch (op)
            {
            case MethodOperator::Add:
                return "__add";
            case MethodOperator::Subtract:
                return "__sub";
            case MethodOperator::Multiply:
                return "__mul";
            case MethodOperator::Divide:
                return "__div";
            case MethodOperator::Negate:
                return "__unm";
            case MethodOperator::Equals:
                return "__eq";
            case MethodOperator::None:
                break;
            }
            return nullptr;
        }

        // A reflected operator's metamethod (__add ...): of the type's statics marked with the
        // operator, the one whose right operand fits the call's (a number, or a value of the
        // parameter's type), invoked with the left operand first. Luau's `a += b` is `a = a + b`.
        // Upvalues: type* (light), context* (light), op (integer).
        int OperatorThunk(lua_State* state)
        {
            auto* type =
                static_cast<const TypeInfo*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(2)));
            const auto op = static_cast<MethodOperator>(lua_tointeger(state, lua_upvalueindex(3)));
            Variant* left = VariantAt(state, 1);
            if (left == nullptr || left->Type() != type)
            {
                lua_pushstring(state, "the left operand of this operator is not the type that "
                                      "defines it");
                lua_error(state);
            }
            const bool unary = op == MethodOperator::Negate;
            const MethodInfo* chosen = nullptr;
            for (const MethodInfo& method : Methods(*type))
            {
                if (method.op != op || !IsBindableOperator(*type, method))
                {
                    continue;
                }
                if (unary)
                {
                    chosen = &method;
                    break;
                }
                const TypeInfo* right = method.params[1].type();
                const Variant* boxed = VariantAt(state, 2);
                const bool fits = (boxed != nullptr) ? boxed->Type() == right
                                                     : lua_isnumber(state, 2) &&
                                                           (right == &TypeOf<f32>() ||
                                                            right == &TypeOf<f64>());
                if (fits)
                {
                    chosen = &method;
                    break;
                }
            }
            if (chosen == nullptr)
            {
                lua_pushstring(state, "no operator of this type takes that right operand");
                lua_error(state);
            }
            Variant args[2];
            args[0] = *left;
            if (!unary)
            {
                args[1] = context->ToVariantForParam(state, 2, chosen->params[1].type());
            }
            ScriptCallScope scope(context);
            Result<Variant> result = InvokeStatic(*chosen, Span<Variant>{args, unary ? 1u : 2u});
            if (!result.HasValue())
            {
                lua_pushstring(state, "reflected operator invocation failed");
                lua_error(state);
            }
            context->PushVariant(state, result.Value());
            return 1;
        }

        int MethodThunk(lua_State* state)
        {
            auto* method = static_cast<const MethodInfo*>(
                lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(2)));
            return InvokeReflected(state, context, *method);
        }

        // Arity-family dispatch: resolve the family member whose paramCount matches the call's
        // argument count (SOUND - argc is exact, no type guessing), then invoke it.
        // Upvalues: type* (light), context* (light), script name (string), isStatic (boolean).
        int ArityMethodThunk(lua_State* state)
        {
            auto* type =
                static_cast<const TypeInfo*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(2)));
            const char* scriptName = lua_tostring(state, lua_upvalueindex(3));
            const bool isStatic = lua_toboolean(state, lua_upvalueindex(4)) != 0;

            const int total = lua_gettop(state);
            const int argc = isStatic ? total : total - 1; // instance calls carry self in slot 1
            const MethodInfo* method = ResolveArityMethod(type, scriptName, isStatic, argc < 0 ? 0 : argc);
            if (method == nullptr)
            {
                lua_pushstring(state, "no overload of this method takes that many arguments");
                lua_error(state);
            }
            return InvokeReflected(state, context, *method);
        }

        // Resolve a polymorphic container's add-by-name to a concrete derived type: match the
        // name against each creatable derived type's `displayName` attribute, then its bare type
        // name (the AngelScript ResolveElementType twin - keep in sync).
        [[nodiscard]] const TypeInfo* ResolveContainerElementType(const TypeInfo& base,
                                                                  StringView name)
        {
            Array<const TypeInfo*> derived;
            EnumerateDerived(base, derived);
            for (const TypeInfo* t : derived)
            {
                if (TypeAttrString(*t, "displayName", StringView{}) == name)
                {
                    return t;
                }
            }
            for (const TypeInfo* t : derived)
            {
                if (StringView(reinterpret_cast<const utf8char*>(t->name)) == name)
                {
                    return t;
                }
            }
            return nullptr;
        }

        // Container-member ops on the OWNER (`shelf:rooms_count()`, `shelf:rooms_at(0)`, ...),
        // mirroring AngelScript's RegisterContainerMethods surface so both backends share ONE
        // write-through contract for reflected container members (the bare
        // property would cross as a detached copy-table whose mutations are silently lost).
        // Indices are ZERO-based on both backends - this is an engine accessor, not a Lua table.
        // Elements come back like AS's ContainerDispatch: Object / copyable values via getAt
        // (owned; Object handles write through by refcount), non-copyable values as
        // generation-guarded BORROWS pinned to the owner - edited in place.
        enum class ContainerOp : int
        {
            Count = 0,
            At,
            Add,
            RemoveAt,
            MoveElement,
        };

        // The element Variant for index `index`: getAt when the element is copyable/Object-owned,
        // else a borrow over its address pinned to `parent` (the AS ContainerElementVariant twin).
        [[nodiscard]] Variant ContainerElementForScript(const ContainerInfo& ci,
                                                        const Instance& container, usize index,
                                                        const Variant& parent)
        {
            Variant element = ContainerGetAt(ci, container, index);
            if (element.IsEmpty())
            {
                const Instance addr = ContainerAddressAt(ci, container, index);
                if (addr.Pointer() != nullptr)
                {
                    element = Variant::Borrow(addr.Pointer(), addr.Type(), parent);
                }
            }
            return element;
        }

        int ContainerOpThunk(lua_State* state)
        {
            auto* property = static_cast<const PropertyInfo*>(
                lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(2)));
            const ContainerOp op =
                static_cast<ContainerOp>(lua_tointeger(state, lua_upvalueindex(3)));

            Variant* self = VariantAt(state, 1);
            if (self == nullptr)
            {
                lua_pushstring(state, "container op called without a bound self (use ':')");
                lua_error(state);
            }
            ScriptCallScope scope(context);
            Instance owner = ToInstance(*self);
            if (owner.Pointer() == nullptr || property->type == nullptr ||
                property->type->container == nullptr)
            {
                lua_pushnil(state);
                return 1;
            }
            // The container member is reached transiently via property.address per call (never
            // cached), matching the AS dispatcher - a reallocation between calls cannot dangle.
            Instance container(property->address(owner), property->type);
            const ContainerInfo& ci = *property->type->container;
            const usize size = ContainerSize(ci, container);
            switch (op)
            {
            case ContainerOp::Count:
                lua_pushnumber(state, static_cast<f64>(size));
                return 1;
            case ContainerOp::At:
            {
                const usize index = static_cast<usize>(lua_tointeger(state, 2));
                if (index >= size)
                {
                    lua_pushnil(state);
                    return 1;
                }
                context->PushVariant(state, ContainerElementForScript(ci, container, index, *self));
                return 1;
            }
            case ContainerOp::Add:
            {
                if (IsPolymorphicContainer(ci)) // add by element-type name
                {
                    const char* typeName = lua_tostring(state, 2);
                    const TypeInfo* elem =
                        (typeName != nullptr && ci.elementType != nullptr)
                            ? ResolveContainerElementType(
                                  *ci.elementType,
                                  StringView(reinterpret_cast<const utf8char*>(typeName)))
                            : nullptr;
                    if (elem == nullptr ||
                        ContainerCreateElement(ci, container, size, *elem).Pointer() == nullptr)
                    {
                        lua_pushnil(state);
                        return 1;
                    }
                }
                else if (ContainerEmplaceDefault(ci, container, size).Pointer() == nullptr)
                {
                    lua_pushnil(state);
                    return 1;
                }
                context->PushVariant(state, ContainerElementForScript(ci, container, size, *self));
                return 1;
            }
            case ContainerOp::RemoveAt:
                (void)ContainerRemoveAt(ci, container,
                                        static_cast<usize>(lua_tointeger(state, 2)));
                return 0;
            case ContainerOp::MoveElement:
                (void)ContainerMoveElement(ci, container,
                                           static_cast<usize>(lua_tointeger(state, 2)),
                                           static_cast<usize>(lua_tointeger(state, 3)));
                return 0;
            }
            lua_pushnil(state);
            return 1;
        }

        int IndexThunk(lua_State* state)
        {
            // (userdata, key) -> method (from the methods table) or property value.
            auto* type = static_cast<const TypeInfo*>(
                lua_tolightuserdata(state, lua_upvalueindex(DispatchUpvalues::kType)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(DispatchUpvalues::kContext)));

            // Methods first: the common case, one rawget.
            lua_pushvalue(state, 2);
            lua_rawget(state, lua_upvalueindex(DispatchUpvalues::kMethods));
            if (!lua_isnil(state, -1))
            {
                return 1;
            }
            lua_pop(state, 1);

            const char* key = lua_tostring(state, 2);
            Variant* boxed = VariantAt(state, 1);
            if (key == nullptr || boxed == nullptr)
            {
                lua_pushnil(state);
                return 1;
            }
            if (const PropertyInfo* property = FindPropertyInChain(type, key))
            {
                // A container MEMBER is not a plain value: it binds as owner ops
                // (`<name>_count/_at/_add/_removeAt/_move` - the AS-parity write-through
                // surface). A bare read would hand script a detached copy-table whose
                // mutations are silently lost; nil is the honest answer (same as AS, where
                // the bare property does not exist).
                if (IsNested(*property) && property->type != nullptr &&
                    property->type->container != nullptr)
                {
                    lua_pushnil(state);
                    return 1;
                }
                ScriptCallScope scope(context);
                Instance instance = ToInstance(*boxed);
                if (instance.Pointer() == nullptr)
                {
                    lua_pushnil(state);
                    return 1;
                }
                context->PushVariant(state, GetProperty(*property, instance));
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        int NewIndexThunk(lua_State* state)
        {
            auto* type = static_cast<const TypeInfo*>(
                lua_tolightuserdata(state, lua_upvalueindex(DispatchUpvalues::kType)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(DispatchUpvalues::kContext)));

            const char* key = lua_tostring(state, 2);
            Variant* boxed = VariantAt(state, 1);
            const PropertyInfo* property =
                (key != nullptr) ? FindPropertyInChain(type, key) : nullptr;
            if (boxed == nullptr || property == nullptr)
            {
                lua_pushstring(state, "no such reflected property");
                lua_error(state);
            }
            ScriptCallScope scope(context);
            Instance instance = ToInstance(*boxed);
            if (instance.Pointer() != nullptr)
            {
                // Honour the property type (enum -> number carried as i64).
                Variant value = context->ToVariantForParam(state, 3, property->type);
                if (!SetProperty(*property, instance, value).IsOk())
                {
                    // A read-only property refuses in Core; a script learns it the same
                    // way it learns a misspelled one - at the assignment, not silently.
                    lua_pushstring(state, "property is read-only");
                    lua_error(state);
                }
            }
            return 0;
        }

        int ConstructorThunk(lua_State* state)
        {
            auto* type = static_cast<const TypeInfo*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto* context = static_cast<LuauScriptContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(2)));

            const int argCount = lua_gettop(state);
            // Marshal each arg against the matching constructor's param type (number narrowing to
            // f32/i32/enum), so Float3.new(2, 3, 4) hands three f32s to the (f32,f32,f32) ctor.
            const ConstructorInfo* ctor = nullptr;
            for (const ConstructorInfo& candidate : Constructors(*type))
            {
                if (candidate.paramCount == static_cast<u32>(argCount))
                {
                    ctor = &candidate;
                    break;
                }
            }
            Array<Variant> args;
            for (int i = 1; i <= argCount; ++i)
            {
                const u32 paramIndex = static_cast<u32>(i - 1);
                const TypeInfo* expected =
                    (ctor != nullptr && paramIndex < ctor->paramCount &&
                     ctor->params[paramIndex].type != nullptr)
                        ? ctor->params[paramIndex].type()
                        : nullptr;
                args.PushBack(context->ToVariantForParam(state, i, expected));
            }
            ScriptCallScope scope(context);
            Result<Variant> constructed = Construct(*type, Span<Variant>{args.Data(), args.Size()});
            if (!constructed.HasValue())
            {
                lua_pushstring(state, "no matching reflected constructor");
                lua_error(state);
            }
            context->PushVariant(state, constructed.Value());
            return 1;
        }
    }

    void LuauScriptContext::EmitType(const TypeInfo& type)
    {
        lua_State* state = m_state;

        // The methods table: every instance/static method in the chain as a thunk.
        lua_newtable(state);
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (const MethodInfo& method : Methods(*t))
            {
                if (method.name == nullptr)
                {
                    continue;
                }
                lua_pushstring(state, ScriptMethodName(method)); // overload identity, not the C++ name
                // Skip if a derived type already bound this name (first wins).
                lua_pushvalue(state, -1);
                lua_rawget(state, -3);
                const bool taken = !lua_isnil(state, -1);
                lua_pop(state, 1);
                if (taken)
                {
                    lua_pop(state, 1);
                    continue;
                }
                // An arity family (same script name, >1 arity) dispatches by argc at call; a plain
                // method captures its single MethodInfo* directly.
                if (ScriptNameFamilySize(&type, ScriptMethodName(method), method.isStatic) > 1)
                {
                    lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
                    lua_pushlightuserdata(state, this);
                    lua_pushstring(state, ScriptMethodName(method));
                    lua_pushboolean(state, method.isStatic ? 1 : 0);
                    lua_pushcclosure(state, ArityMethodThunk, "reflected_method_family", 4);
                }
                else
                {
                    lua_pushlightuserdata(state, const_cast<MethodInfo*>(&method));
                    lua_pushlightuserdata(state, this);
                    lua_pushcclosure(state, MethodThunk, "reflected_method", 2);
                }
                lua_rawset(state, -3);
            }
        }
        // Container members bind as owner ops in the SAME methods table (the AngelScript
        // RegisterContainerMethods twin: `<name>_count/_at/_add/_removeAt/_move`), so
        // `shelf:rooms_at(0)` dispatches exactly like a reflected method. First wins up the chain.
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (const PropertyInfo& property : Properties(*t))
            {
                if (!IsNested(property) || property.name == nullptr || property.type == nullptr ||
                    property.type->container == nullptr)
                {
                    continue;
                }
                static constexpr struct
                {
                    const char* suffix;
                    ContainerOp op;
                } kOps[] = {
                    {"_count", ContainerOp::Count},       {"_at", ContainerOp::At},
                    {"_add", ContainerOp::Add},           {"_removeAt", ContainerOp::RemoveAt},
                    {"_move", ContainerOp::MoveElement},
                };
                for (const auto& entry : kOps)
                {
                    char opName[192];
                    usize n = 0;
                    for (const char* c = property.name; *c != '\0' && n < 150; ++c)
                    {
                        opName[n++] = *c;
                    }
                    for (const char* c = entry.suffix; *c != '\0'; ++c)
                    {
                        opName[n++] = *c;
                    }
                    opName[n] = '\0';
                    lua_pushstring(state, opName);
                    lua_pushvalue(state, -1);
                    lua_rawget(state, -3);
                    const bool taken = !lua_isnil(state, -1);
                    lua_pop(state, 1);
                    if (taken)
                    {
                        lua_pop(state, 1);
                        continue;
                    }
                    lua_pushlightuserdata(state, const_cast<PropertyInfo*>(&property));
                    lua_pushlightuserdata(state, this);
                    lua_pushinteger(state, static_cast<int>(entry.op));
                    lua_pushcclosure(state, ContainerOpThunk, "reflected_container_op", 3);
                    lua_rawset(state, -3);
                }
            }
        }
        const int methodsIndex = lua_gettop(state);

        // The dispatch metatable, keyed in the registry by TypeInfo pointer.
        lua_newtable(state);
        lua_pushstring(state, "__index");
        lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
        lua_pushlightuserdata(state, this);
        lua_pushvalue(state, methodsIndex);
        lua_pushcclosure(state, IndexThunk, "reflected_index", 3);
        lua_rawset(state, -3);
        lua_pushstring(state, "__newindex");
        lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
        lua_pushlightuserdata(state, this);
        lua_pushvalue(state, methodsIndex);
        lua_pushcclosure(state, NewIndexThunk, "reflected_newindex", 3);
        lua_rawset(state, -3);
        // The operators reflection marks, one metamethod each (OperatorThunk picks the overload).
        for (const MethodInfo& method : Methods(type))
        {
            const char* metamethod = OperatorMetamethod(method.op);
            if (metamethod == nullptr || !IsBindableOperator(type, method))
            {
                continue;
            }
            lua_pushstring(state, metamethod);
            lua_rawget(state, -2);
            const bool bound = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (bound)
            {
                continue; // an overload set binds once
            }
            lua_pushstring(state, metamethod);
            lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
            lua_pushlightuserdata(state, this);
            lua_pushinteger(state, static_cast<int>(method.op));
            lua_pushcclosure(state, OperatorThunk, "reflected_operator", 3);
            lua_rawset(state, -3);
        }

        lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
        lua_pop(state, 1); // metatable

        // The class table: Name.new(...) plus the static methods.
        lua_newtable(state);
        lua_pushstring(state, "new");
        lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, ConstructorThunk, "reflected_new", 2);
        lua_rawset(state, -3);
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (const MethodInfo& method : Methods(*t))
            {
                if (method.name == nullptr || !method.isStatic)
                {
                    continue;
                }
                lua_pushstring(state, ScriptMethodName(method));
                // First wins: skip a name already emitted (an arity family binds once, dispatching
                // by argc; a re-seen name up the chain is a duplicate).
                lua_pushvalue(state, -1);
                lua_rawget(state, -3);
                const bool taken = !lua_isnil(state, -1);
                lua_pop(state, 1);
                if (taken)
                {
                    lua_pop(state, 1);
                    continue;
                }
                if (ScriptNameFamilySize(&type, ScriptMethodName(method), true) > 1)
                {
                    lua_pushlightuserdata(state, const_cast<TypeInfo*>(&type));
                    lua_pushlightuserdata(state, this);
                    lua_pushstring(state, ScriptMethodName(method));
                    lua_pushboolean(state, 1);
                    lua_pushcclosure(state, ArityMethodThunk, "reflected_static_family", 4);
                }
                else
                {
                    lua_pushlightuserdata(state, const_cast<MethodInfo*>(&method));
                    lua_pushlightuserdata(state, this);
                    lua_pushcclosure(state, MethodThunk, "reflected_static", 2);
                }
                lua_rawset(state, -3);
            }
        }
        // Install the class table under its script-facing name (scriptName alias when set, else the
        // C++ name) - scripts reference it, and typed-decl spellings match (native identity stays name).
        lua_setglobal(state, ScriptTypeName(type));

        lua_pop(state, 1); // methods table
    }

    void LuauScriptContext::EmitEnum(const TypeInfo& type)
    {
        lua_State* state = m_state;
        // Scripts spell an enum value as EnumName.ValueName (matching AngelScript's native enums).
        // Values are numbers - an enum parameter converts the number back
        // (ToVariantForParam), so the round-trip is exact.
        lua_newtable(state);
        for (const EnumValue& value : Enumerators(type))
        {
            lua_pushstring(state, value.name);
            lua_pushnumber(state, static_cast<f64>(value.value));
            lua_rawset(state, -3);
        }
        lua_setglobal(state, ScriptTypeName(type)); // the alias when set (enum table global)
    }

    void LuauScriptContext::EmitRegisteredTypes()
    {
        const Span<const TypeInfo* const> registered = m_manager->RegisteredTypes();
        // Enums emit as named constant tables (not part of the object reachability closure).
        for (const TypeInfo* type : registered)
        {
            if (type != nullptr && type->enumeratorCount > 0)
            {
                EmitEnum(*type);
            }
        }
        // Object types emit as the bounded REACHABILITY CLOSURE (the shared reachability policy):
        // constructor-having seeds + factory-return roots (e.g. RigidBody.of), closed over the
        // reflected graph - NOT every constructible registered type. Keeps the Luau surface (and
        // script_api) identical to the other backends, and includes constructor-less handles the
        // old simple filter missed.
        Array<const TypeInfo*> emit;
        CollectEmittableTypes(registered, emit);
        for (const TypeInfo* type : emit)
        {
            EmitType(*type);
        }
    }

    // =====================================================================
    // Coroutines: startCoroutine(fn[, owner]) / waitSeconds(t) / waitUntil(fn).
    // The scheduler lives on the manager; the primitives run ON the coroutine
    // thread and yield after recording the wait on its entry.
    // =====================================================================
    namespace
    {
        int StartCoroutineThunk(lua_State* state)
        {
            auto* context =
                static_cast<LuauScriptContext*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            if (!lua_isfunction(state, 1))
            {
                lua_pushstring(state, "startCoroutine expects a function");
                lua_error(state);
            }
            LuauScriptManager* manager = context->Manager();

            lua_State* thread = lua_newthread(state); // pushed on the stack
            const int threadRef = lua_ref(state, -1);
            lua_pop(state, 1);
            lua_pushvalue(state, 1);
            lua_xmove(state, thread, 1); // the body function, now on the thread

            LuauScriptManager::Coroutine entry;
            entry.context = context;
            entry.thread = thread;
            entry.threadRef = threadRef;
            entry.owner = (lua_gettop(state) >= 2 && lua_istable(state, 2))
                              ? lua_topointer(state, 2)
                              : nullptr;
            manager->coroutines.PushBack(entry);
            // First resume happens in AdvanceCoroutines (started=false) so a coroutine
            // begun mid-script-call never re-enters the VM re-entrantly here.
            return 0;
        }

        int WaitSecondsThunk(lua_State* state)
        {
            auto* context =
                static_cast<LuauScriptContext*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            LuauScriptManager::Coroutine* entry = context->Manager()->FindCoroutine(state);
            if (entry == nullptr)
            {
                lua_pushstring(state, "waitSeconds outside a startCoroutine coroutine");
                lua_error(state);
            }
            entry->remainingSeconds = lua_tonumber(state, 1);
            entry->predicateRef = LUA_NOREF;
            return lua_yield(state, 0);
        }

        int WaitUntilThunk(lua_State* state)
        {
            auto* context =
                static_cast<LuauScriptContext*>(lua_tolightuserdata(state, lua_upvalueindex(1)));
            LuauScriptManager::Coroutine* entry = context->Manager()->FindCoroutine(state);
            if (entry == nullptr || !lua_isfunction(state, 1))
            {
                lua_pushstring(state, "waitUntil expects a predicate, inside a coroutine");
                lua_error(state);
            }
            lua_pushvalue(state, 1);
            entry->predicateRef = lua_ref(state, -1);
            lua_pop(state, 1);
            entry->remainingSeconds = 0.0;
            return lua_yield(state, 0);
        }
    }

    void LuauScriptContext::InstallCoroutineApi()
    {
        lua_pushlightuserdata(m_state, this);
        lua_pushcclosure(m_state, StartCoroutineThunk, "startCoroutine", 1);
        lua_setglobal(m_state, "startCoroutine");
        lua_pushlightuserdata(m_state, this);
        lua_pushcclosure(m_state, WaitSecondsThunk, "waitSeconds", 1);
        lua_setglobal(m_state, "waitSeconds");
        lua_pushlightuserdata(m_state, this);
        lua_pushcclosure(m_state, WaitUntilThunk, "waitUntil", 1);
        lua_setglobal(m_state, "waitUntil");
    }

    bool LuauScriptManager::ProcessCoroutineResume(Coroutine& entry, int status)
    {
        switch (ClassifyResume(status))
        {
        case ResumeDisposition::Broke:
            // A breakpoint/step landed inside the coroutine body: the debugger holds THIS coroutine
            // thread (AdvanceCoroutines skips LUA_BREAK threads); Continue re-resumes it.
            // A break can only occur with a debugger attached (it alone arms singlestep); guard so a
            // stray status never leaks a scheduled-but-never-resumed thread.
            if (m_debugger != nullptr)
            {
                m_debugger->OnBroke(entry.context, entry.thread);
                return false; // held, not dropped
            }
            return true;
        case ResumeDisposition::Rescheduled:
            return false; // recorded a new wait; stays scheduled
        case ResumeDisposition::Faulted:
        {
            const char* message = lua_tostring(entry.thread, -1);
            entry.context->ReportError(ScriptErrorKind::Runtime, u8"coroutine", ViewOf(message));
            return true;
        }
        case ResumeDisposition::Completed:
        default:
            return true; // finished -> drop
        }
    }

    bool LuauScriptManager::ResumeCoroutine(Coroutine& entry)
    {
        if (m_debugger != nullptr)
        {
            lua_singlestep(entry.thread, 1); // arm stepping so a breakpoint in the body can break
        }
        int status;
        {
            ScriptCallScope scope(entry.context);
            status = lua_resume(entry.thread, nullptr, 0);
        }
        return ProcessCoroutineResume(entry, status);
    }

    void LuauScriptManager::ContinueCoroutine(Coroutine& entry, int status)
    {
        // Same disposition as the scheduler; drop the entry here if it finished (the debugger owns
        // the Continue path, off the AdvanceCoroutines iteration, so mutating the array is safe).
        if (ProcessCoroutineResume(entry, status))
        {
            for (usize i = 0; i < coroutines.Size(); ++i)
            {
                if (&coroutines[i] == &entry)
                {
                    DropCoroutineAt(i);
                    return;
                }
            }
        }
    }

    void LuauScriptManager::AdvanceCoroutines(f64 deltaSeconds)
    {
        for (usize i = 0; i < coroutines.Size();)
        {
            Coroutine& entry = coroutines[i];
            // A coroutine thread the debugger broke has status LUA_BREAK; the scheduler MUST NOT
            // resume it (that would steal the debugger's held thread mid-break, Fable Q5). The
            // debugger's Continue owns resuming it.
            if (lua_status(entry.thread) == LUA_BREAK)
            {
                ++i;
                continue;
            }
            bool drop = false;
            if (!entry.started)
            {
                entry.started = true;
                drop = ResumeCoroutine(entry);
                // Luau defers the FIRST resume to here (never re-entering the VM mid-begin), but
                // the other backends run it inline during onStart, so their first AdvanceCoroutines
                // already counts this frame's dt against the wait. Count it here too so a Luau
                // coroutine's timer starts on the SAME frame it began - identical wait timing.
                // (If the first resume BROKE, the thread is held - never resume it again here.)
                if (!drop && lua_status(entry.thread) != LUA_BREAK &&
                    entry.predicateRef == LUA_NOREF)
                {
                    entry.remainingSeconds -= deltaSeconds;
                    if (entry.remainingSeconds <= 0.0)
                    {
                        drop = ResumeCoroutine(entry);
                    }
                }
            }
            else if (entry.predicateRef != LUA_NOREF)
            {
                lua_State* state = entry.context->State();
                lua_getref(state, entry.predicateRef);
                bool ready = false;
                if (lua_pcall(state, 0, 1, 0) == LUA_OK)
                {
                    ready = lua_toboolean(state, -1) != 0;
                    lua_pop(state, 1);
                }
                else
                {
                    lua_pop(state, 1);
                    drop = true; // faulting predicate cancels the coroutine
                }
                if (!drop && ready)
                {
                    lua_unref(state, entry.predicateRef);
                    entry.predicateRef = LUA_NOREF;
                    drop = ResumeCoroutine(entry);
                }
            }
            else
            {
                entry.remainingSeconds -= deltaSeconds;
                if (entry.remainingSeconds <= 0.0)
                {
                    drop = ResumeCoroutine(entry);
                }
            }

            if (drop)
            {
                DropCoroutineAt(i);
            }
            else
            {
                ++i;
            }
        }
    }

    void LuauScriptManager::CancelCoroutinesFor(ScriptObject& instance)
    {
        const void* owner = static_cast<LuauScriptObject&>(instance).TablePointer();
        if (owner == nullptr)
        {
            return;
        }
        for (usize i = 0; i < coroutines.Size();)
        {
            if (coroutines[i].owner == owner)
            {
                DropCoroutineAt(i);
            }
            else
            {
                ++i;
            }
        }
    }

    void LuauScriptManager::DropCoroutineAt(usize index)
    {
        Coroutine& entry = coroutines[index];
        lua_State* state = entry.context->State();
        if (entry.predicateRef != LUA_NOREF)
        {
            lua_unref(state, entry.predicateRef);
        }
        if (entry.threadRef != LUA_NOREF)
        {
            lua_unref(state, entry.threadRef);
        }
        coroutines.RemoveAt(index);
    }

    void LuauScriptManager::DropCoroutinesOf(LuauScriptContext* context)
    {
        for (usize i = 0; i < coroutines.Size();)
        {
            if (coroutines[i].context == context)
            {
                DropCoroutineAt(i);
            }
            else
            {
                ++i;
            }
        }
    }

    // =====================================================================
    // Context lifecycle + the IScriptContext surface.
    // =====================================================================
    LuauScriptContext::LuauScriptContext(RefPtr<LuauScriptManager> manager)
        : m_manager(Move(manager))
    {
        m_state = luaL_newstate();
        luaL_openlibs(m_state); // Luau's SANDBOXED stdlib (no io/os/loadstring)
        InstallCoroutineApi();
        EmitRegisteredTypes();
        m_manager->TrackContext(this);
    }

    LuauScriptContext::~LuauScriptContext()
    {
        m_manager->UntrackContext(this);
        // The Detach protocol: delegates outliving the context must fail cleanly, never
        // touch a dead lua_State (the conformance battery's owning-context-gone contract).
        for (LuauScriptDelegate* delegate : m_delegates)
        {
            delegate->Detach();
        }
        m_delegates.Clear();
        lua_close(m_state);
    }

    void LuauScriptContext::ReportError(ScriptErrorKind kind, StringView module,
                                        StringView message)
    {
        if (m_errors == nullptr)
        {
            String line(message);
            line += u8"\n";
            ConsoleWriteError(line.AsView());
            return;
        }
        ScriptError error;
        error.kind = kind;
        error.module = module;
        error.line = ParseLuauErrorLine(message); // Luau puts "<chunk>:<line>:" in the message
        error.message = message;
        m_errors->OnError(error);
    }

    void LuauScriptContext::ReportTopOfStack(ScriptErrorKind kind, StringView module)
    {
        const char* message = lua_tostring(m_state, -1);
        ReportError(kind, module, ViewOf(message));
        lua_pop(m_state, 1);
    }

    Status LuauScriptContext::LoadCompiledChunk(const byte* bytecode, usize size,
                                                StringView chunkName)
    {
        // Prefix the chunkname with "=" so Luau uses it VERBATIM as short_src (no `[string
        // "..."]` wrapper): short_src == the sourceName, which is what a debugger breakpoint
        // (file, line) and the error report key on. The reported module below stays the raw name.
        String chunkStorage(u8"=");
        chunkStorage.Append(chunkName);
        const char* chunk = reinterpret_cast<const char*>(chunkStorage.CStr());

        const int loadStatus = luau_load(m_state, chunk, reinterpret_cast<const char*>(bytecode),
                                         size, 0);
        if (loadStatus != LUA_OK)
        {
            // Luau encodes compile errors in the bytecode; they surface here.
            ReportTopOfStack(ScriptErrorKind::Compile, chunkName);
            return Status{ErrorCode::InvalidArgument};
        }

        ScriptCallScope scope(this);
        if (lua_pcall(m_state, 0, 0, 0) != LUA_OK)
        {
            ReportTopOfStack(ScriptErrorKind::Runtime, chunkName);
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    Status LuauScriptContext::LoadSerializedBlobChunk(Span<const byte> serialized,
                                                      StringView chunkName)
    {
        // The pack stores the SERIALIZED LuauScriptBlob (Array<byte> length + raw bytecode), so the
        // runtime is backend-neutral (CreateBlob + Serialize(read) + load). Reconstruct the raw
        // bytecode here, then load it as this class's own chunk for per-class (file,line) identity.
        LuauScriptBlob blob;
        MemoryStream stream;
        (void)stream.Write(serialized.Data(), serialized.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(stream, SerializeMode::Read);
        blob.Serialize(reader);
        const Span<const byte> raw = blob.Bytecode();
        return LoadCompiledChunk(raw.Data(), raw.Size(), chunkName);
    }

    Status LuauScriptContext::Load(StringView source, StringView chunkName)
    {
        // debugLevel 2 = local + upvalue names, which the step debugger needs to inspect locals;
        // optimizationLevel 1 keeps the bytecode debuggable (no inlining). The cook compiles with
        // the SAME options (LuauScriptManager::CompileToBlob), so a stored blob is byte-equivalent
        // to compiling here - the run host can prefer bytecode with no loss of debuggability.
        lua_CompileOptions options{};
        options.optimizationLevel = 1;
        options.debugLevel = 2;
        size_t bytecodeSize = 0;
        char* bytecode = luau_compile(reinterpret_cast<const char*>(source.Data()), source.Size(),
                                      &options, &bytecodeSize);
        const Status status =
            LoadCompiledChunk(reinterpret_cast<const byte*>(bytecode), bytecodeSize, chunkName);
        free(bytecode);
        return status;
    }

    Status LuauScriptContext::LoadBehaviorModule(Span<const BehaviorModuleClass> classes,
                                                 StringView moduleName)
    {
        // Each Luau class is a GLOBAL table, so loading each source as its OWN chunk (chunkName =
        // the class sourceName) is both valid AND better than the default concatenation: a
        // compile/runtime error and a debugger breakpoint key on the authored (file, line),
        // matching AngelScript per-class section identity, and it is the same
        // per-class load path the bytecode blobs use. A reload redefines the class global in
        // place - the natural Luau hot-reload.
        (void)moduleName;
        for (const BehaviorModuleClass& entry : classes)
        {
            // Prefer the cooked bytecode when present (the player path - no luau_compile): load the
            // blob as this class's own chunk (chunkName = sourceName), the SAME per-class identity
            // the source path uses, so breakpoints + errors still key on (file, line). Fall back to
            // compiling the source (editor authored a class the cook has not stamped yet).
            const Status status = entry.bytecode.IsEmpty()
                                      ? Load(entry.source, entry.name)
                                      : LoadSerializedBlobChunk(entry.bytecode, entry.name);
            if (!status.IsOk())
            {
                return status; // the callee already reported against `entry.name`
            }
        }
        return Status{};
    }

    Status LuauScriptContext::LoadBlob(IScriptBlob& blob)
    {
        // Only this backend produces Luau blobs, and the run host pairs a Luau context with a
        // Luau manager, so the blob is always a LuauScriptBlob (IScriptBlob is opaque - it has
        // no RTTI node - so this is a static downcast by construction, the committed pattern).
        const LuauScriptBlob& luauBlob = static_cast<LuauScriptBlob&>(blob);
        const Span<const byte> bytecode = luauBlob.Bytecode();
        if (bytecode.IsEmpty())
        {
            return Status{ErrorCode::InvalidArgument};
        }

        // luau_load (NOT luau_compile): the player ships bytecode only, no compiler.
        const int loadStatus =
            luau_load(m_state, "=blob", reinterpret_cast<const char*>(bytecode.Data()),
                      bytecode.Size(), 0);
        if (loadStatus != LUA_OK)
        {
            ReportTopOfStack(ScriptErrorKind::Compile, u8"blob");
            return Status{ErrorCode::InvalidArgument};
        }

        ScriptCallScope scope(this);
        if (lua_pcall(m_state, 0, 0, 0) != LUA_OK)
        {
            ReportTopOfStack(ScriptErrorKind::Runtime, u8"blob");
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void LuauScriptContext::SetGlobal(StringView name, const Variant& value)
    {
        String storage;
        PushVariant(m_state, value);
        lua_setglobal(m_state, CStr(name, storage));
    }

    Variant LuauScriptContext::GetGlobal(StringView name)
    {
        String storage;
        lua_getglobal(m_state, CStr(name, storage));
        Variant value = ToVariant(m_state, -1);
        lua_pop(m_state, 1);
        return value;
    }

    bool LuauScriptContext::HasFunction(StringView name) const
    {
        String storage;
        auto* self = const_cast<LuauScriptContext*>(this);
        lua_getglobal(self->m_state, CStr(name, storage));
        const bool isFunction = lua_isfunction(self->m_state, -1);
        lua_pop(self->m_state, 1);
        return isFunction;
    }

    // The VM-global debugstep callback: routes to the active debugger stashed in the callbacks'
    // userdata. Fires per Lua line while the running thread has singlestep armed.
    static void DebugStepThunk(lua_State* thread, lua_Debug* ar)
    {
        if (auto* debugger = static_cast<LuauDebugger*>(lua_callbacks(thread)->userdata))
        {
            debugger->OnStep(thread, ar);
        }
    }

    void LuauScriptContext::InstallDebugHooks(LuauDebugger* debugger)
    {
        lua_Callbacks* cb = lua_callbacks(m_state); // per global_State, shared across all threads
        cb->userdata = debugger;
        cb->debugstep = &DebugStepThunk;
    }

    void LuauScriptContext::RemoveDebugHooks()
    {
        lua_Callbacks* cb = lua_callbacks(m_state);
        cb->debugstep = nullptr;
        cb->userdata = nullptr;
    }

    void LuauScriptManager::TrackContext(LuauScriptContext* context)
    {
        m_contexts.PushBack(context);
        if (m_debugger != nullptr)
        {
            // Born while a debugger is attached: hook it now, or its scripts would never break.
            context->InstallDebugHooks(m_debugger);
        }
    }

    UniquePtr<IScriptDebugger> LuauScriptManager::CreateDebugger()
    {
        return MakeUnique<LuauDebugger>(MemoryAllocator(), this);
    }

    lua_State* LuauScriptContext::AcquireThread()
    {
        if (!m_freeThreads.IsEmpty())
        {
            lua_State* thread = m_freeThreads.Back();
            m_freeThreads.PopBack();
            return thread;
        }
        lua_State* thread = lua_newthread(m_state);
        (void)lua_ref(m_state, -1); // pin against GC for the context's lifetime (freed by lua_close)
        lua_pop(m_state, 1);
        return thread;
    }

    void LuauScriptContext::ReleaseThread(lua_State* thread)
    {
        lua_resetthread(thread); // clear stack + status for reuse (Fable Q5: reset only on return)
        m_freeThreads.PushBack(thread);
    }

    int LuauScriptContext::RunCallable(int total)
    {
        // `total` = the callable + its args on top of m_state; move them to a pooled thread and
        // RESUME (never pcall). One result comes back to m_state on success; an error message on
        // failure. A yield from a handler (waitSeconds already guards it) surfaces as an error.
        lua_State* thread = AcquireThread();
        lua_xmove(m_state, thread, total);
        LuauDebugger* debugger = m_manager->ActiveDebugger();
        if (debugger != nullptr)
        {
            lua_singlestep(thread, 1); // arm per-thread stepping only while a debugger is attached
        }
        int status;
        {
            ScriptCallScope scope(this);
            status = lua_resume(thread, m_state, total - 1);
        }
        if (status == LUA_BREAK && debugger != nullptr)
        {
            // A handler hit a breakpoint/step: the debugger OWNS the thread now (holds it for
            // inspect + Continue) and the game pauses via the listener. The dispatcher sees a
            // void-complete return; the handler's remaining body runs from Continue (the
            // AngelScript adopted-context precedent). Do NOT release the held thread.
            debugger->OnBroke(this, thread);
            lua_pushnil(m_state);
            return LUA_OK;
        }
        if (status == LUA_OK)
        {
            if (lua_gettop(thread) >= 1)
            {
                lua_pushvalue(thread, 1); // first return value
                lua_xmove(thread, m_state, 1);
            }
            else
            {
                lua_pushnil(m_state);
            }
        }
        else if (status == LUA_YIELD)
        {
            lua_pushstring(m_state, "script call yielded outside a coroutine");
            status = LUA_ERRRUN;
        }
        else
        {
            if (lua_gettop(thread) >= 1)
            {
                lua_xmove(thread, m_state, 1); // the error message, for ReportTopOfStack(m_state)
            }
            else
            {
                lua_pushnil(m_state);
            }
        }
        ReleaseThread(thread);
        return status;
    }

    Result<Variant> LuauScriptContext::Call(StringView function, Span<Variant> args)
    {
        String storage;
        lua_getglobal(m_state, CStr(function, storage));
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return Err(ErrorCode::NotFound);
        }
        for (const Variant& arg : args)
        {
            PushVariant(m_state, arg);
        }
        if (RunCallable(static_cast<int>(args.Size()) + 1) != LUA_OK)
        {
            ReportTopOfStack(ScriptErrorKind::Runtime, function);
            return Err(ErrorCode::Unknown);
        }
        Variant result = ToVariant(m_state, -1);
        lua_pop(m_state, 1);
        return result;
    }

    RefPtr<ScriptObject> LuauScriptContext::CreateInstance(StringView className,
                                                           Span<Variant> args)
    {
        String storage;
        lua_getglobal(m_state, CStr(className, storage));
        if (!lua_istable(m_state, -1))
        {
            lua_pop(m_state, 1);
            return {};
        }
        lua_getfield(m_state, -1, "new");
        lua_remove(m_state, -2); // the class table
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return {};
        }
        for (const Variant& arg : args)
        {
            PushVariant(m_state, arg);
        }
        if (RunCallable(static_cast<int>(args.Size()) + 1) != LUA_OK)
        {
            ReportTopOfStack(ScriptErrorKind::Runtime, className);
            return {};
        }
        if (!lua_istable(m_state, -1))
        {
            lua_pop(m_state, 1);
            return {};
        }
        const int tableRef = lua_ref(m_state, -1);
        lua_pop(m_state, 1);
        return RefPtr<ScriptObject>(
            MakeRef<LuauScriptObject>(MemoryAllocator(), RefPtr<LuauScriptContext>(this), tableRef)
                .Get());
    }

    // =====================================================================
    // ScriptObject + delegate bodies.
    // =====================================================================
    LuauScriptObject::~LuauScriptObject()
    {
        lua_unref(m_context->State(), m_tableRef);
    }

    const void* LuauScriptObject::TablePointer() const
    {
        lua_State* state = m_context->State();
        lua_getref(state, m_tableRef);
        const void* pointer = lua_topointer(state, -1);
        lua_pop(state, 1);
        return pointer;
    }

    Result<Variant> LuauScriptObject::Invoke(StringView method, Span<Variant> args)
    {
        lua_State* state = m_context->State();
        String storage;

        // The neutral property-apply path Invokes the setter as `<name>=` with one argument
        // (the setter convention). Luau editor properties are plain instance FIELDS
        // (harvested by walking the constructed table), so a trailing `=` with exactly one arg
        // writes the same-named field directly - the Luau equivalent of AngelScript's settable
        // member. The field always exists once the constructor set it; this applies its default
        // or the hash-keyed override.
        if (args.Size() == 1 && !method.IsEmpty() && method[method.Size() - 1] == u8'=')
        {
            const StringView fieldName = method.SubStr(0, method.Size() - 1);
            lua_getref(state, m_tableRef);
            m_context->PushVariant(state, args[0]);
            lua_setfield(state, -2, CStr(fieldName, storage)); // instance[field] = value
            lua_pop(state, 1);                                 // pop the instance table
            return Variant{};
        }

        lua_getref(state, m_tableRef);
        lua_getfield(state, -1, CStr(method, storage)); // honors the class metatable chain
        if (!lua_isfunction(state, -1))
        {
            lua_pop(state, 2);
            return Err(ErrorCode::NotFound);
        }
        lua_pushvalue(state, -2); // self
        lua_remove(state, -3);
        for (const Variant& arg : args)
        {
            m_context->PushVariant(state, arg);
        }
        // method fn + self + N args = N + 2 items to run on a pooled resumable thread.
        if (m_context->RunCallable(static_cast<int>(args.Size()) + 2) != LUA_OK)
        {
            m_context->ReportTopOfStack(ScriptErrorKind::Runtime, method);
            return Err(ErrorCode::Unknown);
        }
        Variant result = m_context->ToVariant(state, -1);
        lua_pop(state, 1);
        return result;
    }

    Result<Variant> LuauScriptObject::GetProperty(StringView name)
    {
        // The instance's own field, raw: the class metatable holds the methods, which are not
        // properties, and a field the constructor never set is no property either.
        lua_State* state = m_context->State();
        String storage;
        lua_getref(state, m_tableRef);
        lua_rawgetfield(state, -1, CStr(name, storage));
        if (lua_isnil(state, -1) || lua_isfunction(state, -1))
        {
            lua_pop(state, 2);
            return Err(ErrorCode::NotFound);
        }
        Variant value = m_context->ToVariant(state, -1);
        lua_pop(state, 2);
        return value;
    }

    LuauScriptDelegate::~LuauScriptDelegate()
    {
        if (m_context != nullptr)
        {
            m_context->UntrackDelegate(this);
            lua_unref(m_state, m_functionRef);
        }
    }

    Result<Variant> LuauScriptDelegate::Invoke(Span<Variant> args)
    {
        if (m_context == nullptr || m_functionRef == LUA_NOREF)
        {
            return Err(ErrorCode::Internal); // the owning context is gone
        }
        lua_State* state = m_state;
        lua_getref(state, m_functionRef);
        for (const Variant& arg : args)
        {
            m_context->PushVariant(state, arg);
        }
        ScriptCallScope scope(m_context);
        if (lua_pcall(state, static_cast<int>(args.Size()), 1, 0) != LUA_OK)
        {
            m_context->ReportTopOfStack(ScriptErrorKind::Runtime, u8"delegate");
            return Err(ErrorCode::Unknown);
        }
        Variant result = m_context->ToVariant(state, -1);
        lua_pop(state, 1);
        return result;
    }

    // =====================================================================
    // Manager: contexts + the bound-API report.
    // =====================================================================
    RefPtr<IScriptContext> LuauScriptManager::CreateContext()
    {
        return RefPtr<IScriptContext>(
            MakeRef<LuauScriptContext>(MemoryAllocator(), RefPtr<LuauScriptManager>(this)).Get());
    }

    Result<RefPtr<IScriptBlob>> LuauScriptManager::CompileToBlob(StringView source,
                                                               StringView chunkName)
    {
        // Cook with the SAME options the runtime Load uses (optimizationLevel 1, debugLevel 2) so
        // the stored blob is byte-equivalent to compiling in the player - and stays debuggable
        // (local names), so consuming bytecode never degrades the editor's step debugger.
        lua_CompileOptions options{};
        options.optimizationLevel = 1;
        options.debugLevel = 2;
        size_t bytecodeSize = 0;
        char* bytecode = luau_compile(reinterpret_cast<const char*>(source.Data()), source.Size(),
                                      &options, &bytecodeSize);
        if (bytecode == nullptr)
        {
            return Err(ErrorCode::InvalidArgument);
        }

        // luau_compile never fails outright: a syntax error is ENCODED in the bytecode and only
        // surfaces when luau_load runs it. Load it into a throwaway state so a broken source
        // fails the cook here rather than reaching the player as an un-loadable blob.
        String chunkStorage;
        const char* chunk = CStr(chunkName, chunkStorage);
        lua_State* probe = luaL_newstate();
        const int loadStatus = luau_load(probe, chunk, bytecode, bytecodeSize, 0);
        lua_close(probe);
        if (loadStatus != LUA_OK)
        {
            free(bytecode);
            return Err(ErrorCode::InvalidArgument);
        }

        RefPtr<LuauScriptBlob> blob = MakeRef<LuauScriptBlob>(MemoryAllocator());
        blob->SetBytecode(reinterpret_cast<const byte*>(bytecode), bytecodeSize);
        free(bytecode);
        return RefPtr<IScriptBlob>(blob.Get());
    }

    Array<ScriptApiType> LuauScriptManager::DescribeBoundApi() const
    {
        Array<ScriptApiType> surface;
        // The same reachability closure the emitter binds - so script_api reports exactly the set
        // scripts can actually use, identical to the other backends.
        Array<const TypeInfo*> emittable;
        CollectEmittableTypes(Span<const TypeInfo* const>{m_types.Data(), m_types.Size()}, emittable);
        for (const TypeInfo* type : emittable)
        {
            ScriptApiType api;
            api.scriptName = String(ViewOf(type->name));
            api.typeId = type->id;

            ScriptApiMember ctor;
            ctor.name = String(u8"new");
            ctor.kind = ScriptApiMemberKind::Method;
            ctor.isStatic = true;
            ctor.signature = api.scriptName;
            ctor.signature += u8".new(...)";
            api.members.PushBack(Move(ctor));

            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                for (const MethodInfo& method : Methods(*t))
                {
                    if (method.name == nullptr)
                    {
                        continue;
                    }
                    ScriptApiMember member;
                    member.name = String(ViewOf(ScriptMethodName(method)));
                    member.kind = ScriptApiMemberKind::Method;
                    member.isStatic = method.isStatic;
                    member.signature = member.name;
                    member.signature += u8"(";
                    for (u32 p = 0; p < method.paramCount; ++p)
                    {
                        if (p > 0)
                        {
                            member.signature += u8", ";
                        }
                        const ParamInfo& param = method.params[p];
                        member.signature +=
                            (param.name != nullptr && param.name[0] != '\0')
                                ? ViewOf(param.name)
                                : StringView(u8"arg");
                    }
                    member.signature += u8")";
                    api.members.PushBack(Move(member));
                    // The operator it also binds as (OperatorThunk), as a script writes it.
                    if (t == type && IsBindableOperator(*type, method))
                    {
                        ScriptApiMember op;
                        op.name = String(OperatorSymbol(method.op));
                        op.kind = ScriptApiMemberKind::Operator;
                        const bool unary = method.op == MethodOperator::Negate;
                        const TypeInfo* result =
                            method.returnType != nullptr ? method.returnType() : nullptr;
                        if (unary)
                        {
                            op.signature = Format(u8"-{}", api.scriptName.AsView());
                        }
                        else
                        {
                            const TypeInfo* right = method.params[1].type();
                            const bool number = right == &TypeOf<f32>() || right == &TypeOf<f64>();
                            op.signature = Format(u8"{} {} {}", api.scriptName.AsView(),
                                                  OperatorSymbol(method.op),
                                                  number ? StringView(u8"number")
                                                         : (right != nullptr ? ViewOf(right->name)
                                                                             : StringView(u8"?")));
                        }
                        op.signature += Format(u8" -> {}", result != nullptr ? ViewOf(result->name)
                                                                             : StringView(u8"?"));
                        if (!unary && method.op != MethodOperator::Equals && result == type)
                        {
                            op.signature += Format(u8" (and {}=)", OperatorSymbol(method.op));
                        }
                        api.members.PushBack(Move(op));
                    }
                }
                for (u32 p = 0; p < t->propertyCount; ++p)
                {
                    if (t->properties[p].name == nullptr)
                    {
                        continue;
                    }
                    ScriptApiMember member;
                    member.name = String(ViewOf(t->properties[p].name));
                    member.kind = ScriptApiMemberKind::Property;
                    member.readOnly = (static_cast<u32>(t->properties[p].flags) &
                                       static_cast<u32>(PropertyFlags::ReadOnly)) != 0;
                    member.signature = member.name;
                    if (member.readOnly)
                    {
                        member.signature += u8" (read only)";
                    }
                    api.members.PushBack(Move(member));
                }
            }
            surface.PushBack(Move(api));
        }
        return surface;
    }
}

export namespace foundation::script
{
    /// The backend factory: a Luau IScriptManager (interpreter-only, longjmp error
    /// unwinding, sandboxed stdlib). Language id: "luau".
    [[nodiscard]] core::RefPtr<IScriptManager>
    CreateLuauScriptManager(core::IAllocator& allocator)
    {
        return core::RefPtr<IScriptManager>(core::MakeRef<LuauScriptManager>(allocator).Get());
    }

    /// The vendored Luau bytecode-format version (LBC_VERSION_TARGET). Bytecode is NOT stable
    /// across Luau versions, so the cook folds this into its fingerprint: a vendor bump changes
    /// the number and every Luau script pack recooks automatically.
    [[nodiscard]] inline core::u32 LuauBytecodeVersion() noexcept
    {
        return static_cast<core::u32>(LBC_VERSION_TARGET);
    }

    /// Emits `.d.luau` TYPE DECLARATIONS for the bound engine surface (the agent payoff):
    /// reflection -> Luau declarations (classes with typed instance methods + properties, static
    /// tables with `new` + statics, enums as number tables) so luau-analyze / an editor / an agent
    /// type-checks a script against the REAL API. The object set is the same bounded reachability
    /// closure the emitter binds, so declarations match the runtime surface exactly. A cook-time
    /// artifact - regenerate when reflection changes; never committed. Arity families (one script
    /// name, several arities) collapse to their first overload (luau-analyze overload types are
    /// not emitted).
    [[nodiscard]] core::String
    EmitLuauDeclarations(core::Span<const core::TypeInfo* const> registeredTypes);

    /// Registers Luau with the backend registry - the ONE line that makes the
    /// language available; consumers resolve by extension/language, never by type.
    inline void RegisterLuauScriptBackend()
    {
        ScriptBackendDesc desc;
        desc.languageId = core::String(u8"luau");
        desc.displayName = core::String(u8"Luau");
        desc.fileExtensions.PushBack(core::String(u8"luau"));
        desc.create = [](core::IAllocator& allocator)
        { return CreateLuauScriptManager(allocator); };
        ScriptBackendRegistry::Get().Register(core::Move(desc));
    }

    core::String EmitLuauDeclarations(core::Span<const core::TypeInfo* const> registeredTypes)
    {
        using namespace foundation::core;

        // The bounded reachability closure: the exact object types scripts can use (the one policy
        // shared with the emitter + DescribeBoundApi), so declarations mirror the runtime surface.
        Array<const TypeInfo*> declared;
        CollectEmittableTypes(registeredTypes, declared);
        const Span<const TypeInfo* const> declaredSpan{declared.Data(), declared.Size()};

        String out;
        out += u8"--!strict\n";
        out += u8"-- Engine API declarations (.d.luau), generated from reflection. Do not edit.\n\n";

        // Enums: a named number table (EnumName.ValueName), matching the runtime's EmitEnum.
        for (const TypeInfo* type : registeredTypes)
        {
            if (type == nullptr || type->enumeratorCount == 0)
            {
                continue;
            }
            out += u8"declare ";
            out += ViewOf(type->name);
            out += u8": {\n";
            for (const EnumValue& value : Enumerators(*type))
            {
                out += u8"    ";
                out += ViewOf(value.name);
                out += u8": number,\n";
            }
            out += u8"}\n\n";
        }

        // Object types: the instance TYPE (`declare class`) + the VALUE table (`new` + statics).
        for (const TypeInfo* type : declared)
        {
            const StringView name = ViewOf(type->name);

            // --- instance type: properties + colon methods (self-first) ---
            out += u8"declare class ";
            out += name;
            out += u8"\n";
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                for (u32 p = 0; p < t->propertyCount; ++p)
                {
                    const PropertyInfo& prop = t->properties[p];
                    if (prop.name == nullptr)
                    {
                        continue;
                    }
                    out += u8"    ";
                    if ((static_cast<u32>(prop.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0)
                    {
                        out += u8"read "; // luau-analyze then refuses the assignment at check time
                    }
                    out += ViewOf(prop.name);
                    out += u8": ";
                    out += LuauTypeName(prop.type, declaredSpan);
                    out += u8"\n";
                }
            }
            Array<StringView> seenInstance; // dedup arity families (first overload wins)
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
            {
                for (const MethodInfo& method : Methods(*t))
                {
                    if (method.name == nullptr || method.isStatic)
                    {
                        continue;
                    }
                    const StringView methodName = ViewOf(ScriptMethodName(method));
                    bool seen = false;
                    for (const StringView existing : seenInstance)
                    {
                        if (existing == methodName)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (seen)
                    {
                        continue;
                    }
                    seenInstance.PushBack(methodName);
                    out += u8"    function ";
                    out += methodName;
                    out += u8"(self";
                    for (u32 p = 0; p < method.paramCount; ++p)
                    {
                        out += u8", ";
                        const ParamInfo& param = method.params[p];
                        if (param.name != nullptr && param.name[0] != '\0')
                        {
                            out += ViewOf(param.name);
                        }
                        else
                        {
                            out += Format(u8"arg{}", p);
                        }
                        out += u8": ";
                        out += LuauTypeName(param.type != nullptr ? param.type() : nullptr,
                                            declaredSpan);
                    }
                    out += u8"): ";
                    out += LuauTypeName(method.returnType(), declaredSpan);
                    out += u8"\n";
                }
            }
            out += u8"end\n";

            // --- value table: `new` (first constructor) + static methods, on the global name ---
            out += u8"declare ";
            out += name;
            out += u8": {\n";
            if (type->constructorCount > 0)
            {
                // Every constructor is an overload of `new` - a Luau intersection of function
                // types (`((a) -> T) & ((b) -> T)`), so a type-checker accepts each real arity
                // (e.g. Float3.new() AND Float3.new(x, y, z)). A single constructor needs no `&`.
                out += u8"    new: ";
                const bool overloaded = type->constructorCount > 1;
                for (u32 c = 0; c < type->constructorCount; ++c)
                {
                    if (c > 0)
                    {
                        out += u8" & ";
                    }
                    if (overloaded)
                    {
                        out += u8"(";
                    }
                    const ConstructorInfo& ctor = type->constructors[c];
                    AppendLuauParamList(out, ctor.params, ctor.paramCount, declaredSpan);
                    out += u8" -> ";
                    out += name;
                    if (overloaded)
                    {
                        out += u8")";
                    }
                }
                out += u8",\n";
            }
            Array<StringView> seenStatic;
            for (const MethodInfo& method : Methods(*type))
            {
                if (method.name == nullptr || !method.isStatic)
                {
                    continue;
                }
                const StringView methodName = ViewOf(ScriptMethodName(method));
                bool seen = false;
                for (const StringView existing : seenStatic)
                {
                    if (existing == methodName)
                    {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                {
                    continue;
                }
                seenStatic.PushBack(methodName);
                out += u8"    ";
                out += methodName;
                out += u8": ";
                AppendLuauParamList(out, method.params, method.paramCount, declaredSpan);
                out += u8" -> ";
                out += LuauTypeName(method.returnType(), declaredSpan);
                out += u8",\n";
            }
            out += u8"}\n\n";
        }

        return out;
    }
}
