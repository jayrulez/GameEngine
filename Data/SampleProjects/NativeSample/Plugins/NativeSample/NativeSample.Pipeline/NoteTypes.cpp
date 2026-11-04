// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The note types' definitions, out of the interface (a type's definition in a .cppm makes GCC emit a
// gcm cluster).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module nativesample.pipeline;

namespace nativesample
{
    RTTI_DEFINE_OBJECT(NoteAsset, "rtti::nativesample")
    RTTI_DEFINE_OBJECT(NoteProduct, "rtti::nativesample")
}
