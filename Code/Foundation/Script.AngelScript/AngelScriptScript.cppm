// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.AngelScript - AngelScript backend (foundation.script.angelscript).
//
// Implements the script interfaces on AngelScript. Reflected types are emitted in two
// phases (IScriptManager::FinalizeTypes): the first declares every collected type via
// RegisterObjectType, then the second registers behaviours, properties and methods, so
// declaration strings may reference any reflected type regardless of registration order.
//
// The emission mapping (how reflected types appear in AngelScript syntax):
//  - Constructible value and Object types -> reference types (asOBJ_REF) whose
//    instances box the engine Variant; each reflected constructor becomes a
//    factory: `Float3(1, 2, 3)`.
//  - Properties -> virtual property accessors: `v.x`, `v.x = 9`.
//  - Instance methods -> object methods: `box.Contains(p)`.
//  - Static methods -> global functions in a namespace named after the class
//    (AngelScript's documented stand-in for statics): `Float3::Dot(a, b)`.
//  - Overloads register per exact signature (no arity-collapsing).
//  - Scalars map f32->float, f64->double, i32->int, u32->uint, i64->int64,
//    u64->uint64 (and the 8 and 16-bit widths), bool->bool, core String<->script `string`.
//
// This is a module interface unit and holds engine types only. All AngelScript SDK
// contact (angelscript.h, scriptstdstring) lives in AngelScriptScriptImpl.cpp, because
// GCC's modules do not cope with heavy third-party headers in an interface unit's
// global module fragment.

module;
#include "Core/Prelude.h"

export module foundation.script.angelscript;

import foundation.core;
import foundation.script;

namespace core = foundation::core;

export namespace foundation::script::angelscript
{
    [[nodiscard]] core::RefPtr<IScriptManager> CreateScriptManager(core::IAllocator& allocator);

    /// Registers AngelScript with the backend registry, which makes the language available;
    /// consumers resolve by extension (u8"as") or language id (u8"angelscript"), never by
    /// backend type. A game or project opts in by calling this from its entry point, as it
    /// registers extra subsystems.
    void RegisterAngelScriptBackend();

    /// For the editor cook: the raw `asIScriptEngine*` (as an opaque `void*`) behind a
    /// manager this backend created, so the AngelScript editor cook can drive the
    /// CScriptBuilder add-on for `[metadata]` property harvest against an engine that
    /// already has the reflected types registered. Returns null for a null/foreign
    /// manager. `void*` keeps the AngelScript SDK header out of this interface unit.
    /// Runtime dispatch never uses this; it is a tooling hook.
    [[nodiscard]] void* AngelScriptEngineHandle(IScriptManager& manager) noexcept;

    /// The in-module coroutine support section (`Coroutine::waitUntil`) the runtime
    /// adds to every loaded behavior module. Exposed so the editor cook, which builds
    /// the behavior through CScriptBuilder, compiles it with the same surface the
    /// runtime does (a coroutine-using behavior harvests exactly as it runs).
    [[nodiscard]] core::StringView AngelScriptCoroutineModulePrelude() noexcept;

    /// The AngelScript bytecode-format version (ANGELSCRIPT_VERSION). Bytecode is tied to the
    /// library version, so the cook folds this into its fingerprint: an AngelScript vendor bump
    /// changes the number and every AngelScript script pack recooks (as LuauBytecodeVersion does).
    /// Kept out of line so the SDK header stays in the implementation unit.
    [[nodiscard]] core::u32 AngelScriptBytecodeVersion() noexcept;

    /// Wrap an already-built module's bytecode into a serializable IScriptBlob (SaveByteCode into
    /// the same blob type CompileToBlob and LoadBlob use). The AngelScript cook builds through
    /// CScriptBuilder (to strip `[metadata]`), so it has to save the module it already built
    /// rather than recompile the raw source; this puts that module on the neutral blob, so the
    /// cook stores and the runtime reconstructs bytecode as it does for Luau. `module` is an
    /// `asIScriptModule*` as `void*` (the SDK header stays out of the cook). Null on failure.
    [[nodiscard]] core::RefPtr<IScriptBlob>
    AngelScriptBlobFromModule(void* module, core::IAllocator& allocator);
}
