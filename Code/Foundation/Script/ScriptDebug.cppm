// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_debug partition
//
// The neutral interfaces for a step debugger, a VM profiler, and a cook-to-bytecode blob.
// Both backends implement the debugger and the bytecode blob and declare those
// ScriptCapabilities flags; neither implements the profiler yet, so its flag stays absent and
// its factory returns null. The conformance battery certifies each capability a backend
// declares and skips the ones it lacks.
//
// The debugger snapshot value types (ScriptStackFrame, ScriptVariable,
// ScriptValueObject) are plain data with a Serialize() that runs identically in both
// directions, so a remote debug transport can move them across a socket.

module;
#include "Core/Prelude.h"

export module foundation.script:script_debug;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::script
{
    // ---- debugger snapshot value types (plain data, wire-symmetric) --------------

    /// One frame of a captured call stack.
    struct ScriptStackFrame
    {
        core::String file;
        core::String function;
        core::i32 line = -1;
    };

    /// One captured named value (local, argument, or object member). `value` is the
    /// display text; a non-zero `objectRef` marks it as a lazily-expandable object whose
    /// members are fetched with IScriptDebugger::CaptureObject(objectRef).
    struct ScriptVariable
    {
        core::String name;
        core::String typeName;
        core::String value;
        core::u64 objectRef = 0; // 0 = a leaf scalar; non-zero = expandable object handle
    };

    /// A lazily-expandable object handle in a capture: its opaque reference id (stable for
    /// the duration of a break) plus a short display text.
    struct ScriptValueObject
    {
        core::u64 ref = 0;
        core::String text;
    };

    // Wire symmetry: one description, both directions. ADL finds these for Serialize(ar, x).
    inline void Serialize(core::ISerializer& ar, ScriptStackFrame& frame)
    {
        ar.BeginObject();
        core::Serialize(ar, "file", frame.file);
        core::Serialize(ar, "function", frame.function);
        core::Serialize(ar, "line", frame.line);
        ar.EndObject();
    }

    inline void Serialize(core::ISerializer& ar, ScriptVariable& variable)
    {
        ar.BeginObject();
        core::Serialize(ar, "name", variable.name);
        core::Serialize(ar, "typeName", variable.typeName);
        core::Serialize(ar, "value", variable.value);
        core::Serialize(ar, "objectRef", variable.objectRef);
        ar.EndObject();
    }

    inline void Serialize(core::ISerializer& ar, ScriptValueObject& object)
    {
        ar.BeginObject();
        core::Serialize(ar, "ref", object.ref);
        core::Serialize(ar, "text", object.text);
        ar.EndObject();
    }

    // ---- debugger ----------------------------------------------------------------

    /// How the debugger's execution state moved (delivered asynchronously to a listener).
    enum class ScriptDebuggerState : core::u8
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

    /// A step debugger. Both backends implement it and declare ScriptCapabilities::Debugger
    /// (AngelScript's suspends the executing context at a breakpoint). The editor drives
    /// the debugger only through this interface.
    class IScriptDebugger
    {
    public:
        virtual ~IScriptDebugger() = default;

        virtual void SetBreakpoint(core::StringView file, core::i32 line) = 0;
        virtual void RemoveBreakpoint(core::StringView file, core::i32 line) = 0;

        virtual void Break() = 0;
        virtual void Continue() = 0;
        virtual void StepInto() = 0;
        virtual void StepOver() = 0;

        /// Capture the call stack, innermost frame first.
        [[nodiscard]] virtual core::Array<ScriptStackFrame> CaptureStackFrames() = 0;
        /// Capture the locals visible in the frame at `depth` (0 = innermost).
        [[nodiscard]] virtual core::Array<ScriptVariable> CaptureLocals(core::u32 depth) = 0;
        /// Lazily expand the members of a captured object handle.
        [[nodiscard]] virtual core::Array<ScriptVariable> CaptureObject(core::u64 objectRef) = 0;

        virtual void SetListener(IScriptDebuggerListener* listener) = 0;
    };

    // ---- profiler ----------------------------------------------------------------

    /// One measured script call, delivered to the profiler listener.
    struct ScriptCallMeasurement
    {
        core::u32 scriptId = 0;
        core::String function;
        core::u32 callCount = 0;
        core::f64 inclusiveSeconds = 0.0; // time in this call and everything it invoked
        core::f64 exclusiveSeconds = 0.0; // time in this call alone
    };

    /// VM-level profiling sink. Non-owning.
    class IScriptProfilerListener
    {
    public:
        virtual ~IScriptProfilerListener() = default;
        virtual void OnCallMeasured(const ScriptCallMeasurement& measurement) = 0;
    };

    /// A VM profiler. No backend implements it yet (ScriptCapabilities::Profiler is
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

    /// An opaque, serializable compiled script unit (AngelScript SaveByteCode output, Luau
    /// bytecode). A backend with ScriptCapabilities::Bytecode produces it from
    /// IScriptManager::CompileToBlob and consumes it in IScriptContext::LoadBlob. The blob
    /// serializes so a cook can store it and the runtime load it.
    class IScriptBlob : public core::Object
    {
        // Object-derived only for RefPtr lifetime; it is never a registry type, so it
        // keeps Object's default type identity (no RTTI_OBJECT needed).
    public:
        /// Move the blob's opaque bytes in whichever direction `ar` runs.
        virtual void Serialize(core::ISerializer& ar) = 0;
    };
}
