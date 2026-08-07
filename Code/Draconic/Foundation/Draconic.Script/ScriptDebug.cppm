// Draconic Script - :script_debug partition
//
// COMMITTED SEAMS (not yet implemented on any backend): the neutral interfaces for a
// step debugger, a VM profiler, and a cook-to-bytecode blob. Each backend declares the
// matching ScriptCapabilities flag ABSENT and its factory returns null; the conformance
// battery has a skipped-when-absent skeleton for each, so turning a capability on later
// has an immediate certification target. Modelled (trimmed) on Traktor's script debug
// stack - full impl + editor UI + remote transport is a later dedicated track.
//
// The debugger SNAPSHOT value types (ScriptStackFrame / ScriptVariable /
// ScriptValueObject) are plain data and WIRE-SYMMETRIC: they carry a Serialize() that
// runs identically in both directions, because a future remote debug transport must move
// them across a socket. Every one has a round-trip test in the Script test suite.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script:script_debug;

import draconic.foundation;

namespace foundation = draconic::foundation;

export namespace draconic::script
{
    // ---- debugger snapshot value types (plain data, wire-symmetric) --------------

    /// One frame of a captured call stack.
    struct ScriptStackFrame
    {
        foundation::String file;
        foundation::String function;
        foundation::i32 line = -1;
    };

    /// One captured named value (local, argument, or object member). `value` is the
    /// display text; a non-zero `objectRef` marks it as a lazily-expandable object whose
    /// members are fetched with IScriptDebugger::CaptureObject(objectRef).
    struct ScriptVariable
    {
        foundation::String name;
        foundation::String typeName;
        foundation::String value;
        foundation::u64 objectRef = 0; // 0 = a leaf scalar; non-zero = expandable object handle
    };

    /// A lazily-expandable object handle in a capture: its opaque reference id (stable for
    /// the duration of a break) plus a short display text.
    struct ScriptValueObject
    {
        foundation::u64 ref = 0;
        foundation::String text;
    };

    // Wire symmetry: one description, both directions. ADL finds these for Serialize(ar, x).
    inline void Serialize(foundation::ISerializer& ar, ScriptStackFrame& frame)
    {
        ar.BeginObject();
        foundation::Serialize(ar, "file", frame.file);
        foundation::Serialize(ar, "function", frame.function);
        foundation::Serialize(ar, "line", frame.line);
        ar.EndObject();
    }

    inline void Serialize(foundation::ISerializer& ar, ScriptVariable& variable)
    {
        ar.BeginObject();
        foundation::Serialize(ar, "name", variable.name);
        foundation::Serialize(ar, "typeName", variable.typeName);
        foundation::Serialize(ar, "value", variable.value);
        foundation::Serialize(ar, "objectRef", variable.objectRef);
        ar.EndObject();
    }

    inline void Serialize(foundation::ISerializer& ar, ScriptValueObject& object)
    {
        ar.BeginObject();
        foundation::Serialize(ar, "ref", object.ref);
        foundation::Serialize(ar, "text", object.text);
        ar.EndObject();
    }

    // ---- debugger ----------------------------------------------------------------

    /// How the debugger's execution state moved (delivered asynchronously to a listener).
    enum class ScriptDebuggerState : foundation::u8
    {
        Running,
        Breakpoint, // stopped at a breakpoint
        Stepped,    // stopped after a step
        Terminated,
    };

    /// Async debugger state-change sink (a remote UI or the editor). Non-owning.
    class IScriptDebuggerListener
    {
    public:
        virtual ~IScriptDebuggerListener() = default;
        virtual void OnDebuggerStateChanged(ScriptDebuggerState state) = 0;
    };

    /// Step-debug seam. AngelScript implements it (suspension-based - see the
    /// AngelScriptDebugger in the backend impl unit; ScriptCapabilities::Debugger declared
    /// and battery-certified). Wren stays absent (no official VM debug API). The editor UI
    /// and the future remote transport drive only this contract.
    class IScriptDebugger
    {
    public:
        virtual ~IScriptDebugger() = default;

        virtual void SetBreakpoint(foundation::StringView file, foundation::i32 line) = 0;
        virtual void RemoveBreakpoint(foundation::StringView file, foundation::i32 line) = 0;

        virtual void Break() = 0;
        virtual void Continue() = 0;
        virtual void StepInto() = 0;
        virtual void StepOver() = 0;

        /// Capture the call stack, innermost frame first.
        [[nodiscard]] virtual foundation::Array<ScriptStackFrame> CaptureStackFrames() = 0;
        /// Capture the locals visible in the frame at `depth` (0 = innermost).
        [[nodiscard]] virtual foundation::Array<ScriptVariable> CaptureLocals(foundation::u32 depth) = 0;
        /// Lazily expand the members of a captured object handle.
        [[nodiscard]] virtual foundation::Array<ScriptVariable> CaptureObject(foundation::u64 objectRef) = 0;

        virtual void SetListener(IScriptDebuggerListener* listener) = 0;
    };

    // ---- profiler ----------------------------------------------------------------

    /// One measured script call, delivered to the profiler listener.
    struct ScriptCallMeasurement
    {
        foundation::u32 scriptId = 0;
        foundation::String function;
        foundation::u32 callCount = 0;
        foundation::f64 inclusiveSeconds = 0.0; // time in this call and everything it invoked
        foundation::f64 exclusiveSeconds = 0.0; // time in this call alone
    };

    /// VM-level profiling sink. Non-owning.
    class IScriptProfilerListener
    {
    public:
        virtual ~IScriptProfilerListener() = default;
        virtual void OnCallMeasured(const ScriptCallMeasurement& measurement) = 0;
    };

    /// VM profiling seam. No backend implements it yet (ScriptCapabilities::Profiler is
    /// absent everywhere and CreateProfiler returns null).
    class IScriptProfiler
    {
    public:
        virtual ~IScriptProfiler() = default;
        virtual void SetListener(IScriptProfilerListener* listener) = 0;
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
    };

    // ---- cook-to-bytecode blob ---------------------------------------------------

    /// An opaque, serializable compiled script unit (e.g. AngelScript SaveByteCode). No
    /// backend produces one yet (ScriptCapabilities::Bytecode is absent everywhere;
    /// IScriptManager::CompileToBlob returns an error and IScriptContext::LoadBlob returns
    /// NotSupported). The blob serializes so a cook can store it and the runtime load it.
    class IScriptBlob : public foundation::Object
    {
        // Object-derived only for RefPtr lifetime; it is never a registry type, so it
        // keeps Object's default type identity (no DRACONIC_OBJECT needed).
    public:
        /// Move the blob's opaque bytes in whichever direction `ar` runs.
        virtual void Serialize(foundation::ISerializer& ar) = 0;
    };
}
