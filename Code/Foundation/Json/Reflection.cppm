// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Json - :reflection partition
//
// Declares the reflection entry point. Call RegisterJsonTypes() once at startup: it patches
// TypeOf<JsonValue>() with the reflected surface (constructor and methods) and registers it in the
// GlobalTypeRegistry, so scripts can parse, build, query and stringify JSON through the standard
// bound-object machinery. The REFLECT_* body lives in ReflectionImpl.cpp, out of this interface,
// as GCC's modules need.

module;
#include "Core/Prelude.h"

export module foundation.json:reflection;

export namespace foundation::json
{
    // Reflects JsonValue (patches TypeOf<JsonValue>()) and adds it to the GlobalTypeRegistry.
    // Idempotent; call once at startup before scripts that use the Json type run.
    void RegisterJsonTypes();
}
