// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Json - :reflection implementation unit
//
// The REFLECT_VALUE body for JsonValue and RegisterJsonTypes(). Kept out of the :reflection
// interface partition (REFLECT_* bodies in a partition interface make GCC emit a gcm cluster for
// consumers). The script surface is value-semantic: accessors return owned copies, mutators copy
// in, so nested handles never alias a reallocatable document.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.json;

import foundation.core;
import :value;
import :writer; // Write() for JsonValue::ToString
import :parser; // json::Parse() for JsonValue::Parse

using namespace foundation::core;

namespace foundation::json
{
    // Declared on JsonValue in :value; defined here (not inline in the primary interface) so MSVC
    // emits a linkable body (see the note in JsonModule.cppm).
    String JsonValue::ToString(bool pretty) const
    {
        return Write(*this, pretty);
    }

    JsonValue JsonValue::Parse(String text)
    {
        return foundation::json::Parse(text.AsView()).value; // Null on error, never a half-value
    }

    REFLECT_VALUE(JsonValue, "rtti::foundation::json")
    {
        builder
            .Constructor() // default: a Null value; build up with Set/Add
            // Factories (static): start a container or wrap a scalar.
            .Method<&JsonValue::MakeNull>("MakeNull")
            .Method<&JsonValue::MakeBool>("MakeBool", {"value"})
            .Method<&JsonValue::MakeNumber>("MakeNumber", {"value"})
            .Method<&JsonValue::MakeString>("MakeString", {"value"})
            .Method<&JsonValue::MakeArray>("MakeArray")
            .Method<&JsonValue::MakeObject>("MakeObject")
            // Type queries.
            .Method<&JsonValue::IsNull>("IsNull")
            .Method<&JsonValue::IsBool>("IsBool")
            .Method<&JsonValue::IsNumber>("IsNumber")
            .Method<&JsonValue::IsString>("IsString")
            .Method<&JsonValue::IsArray>("IsArray")
            .Method<&JsonValue::IsObject>("IsObject")
            // Scalar accessors (typed; caller passes the fallback).
            .Method<&JsonValue::AsBool>("AsBool", {"fallback"})
            .Method<&JsonValue::AsNumber>("AsNumber", {"fallback"})
            .Method<&JsonValue::AsInt>("AsInt", {"fallback"})
            .Method<&JsonValue::AsString>("AsString")
            // Array / object shared.
            .Method<&JsonValue::Count>("Count")
            // Array.
            .Method<&JsonValue::At>("At", {"index"})
            .Method<&JsonValue::Add>("Add", {"value"})
            // Object.
            .Method<&JsonValue::Has>("Has", {"key"})
            .Method<&JsonValue::Get>("Get", {"key"})
            .Method<&JsonValue::Set>("Set", {"key", "value"})
            .Method<&JsonValue::KeyAt>("KeyAt", {"index"})
            // Serialize / parse.
            .Method<&JsonValue::ToString>("ToString", {"pretty"})
            .Method<&JsonValue::Parse>("Parse", {"text"});
    }

    void RegisterJsonTypes()
    {
        RttiRegisterValue_JsonValue();
        GlobalTypeRegistry().Register(TypeOf<JsonValue>());
    }
}
