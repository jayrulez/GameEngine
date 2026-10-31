// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Script - JSON through the scripting backends.
//
// SUBJECT is the cross-collection flow foundation.json x foundation.script.angelscript: a script
// parses, queries, builds, and stringifies JSON through the reflected value type - the user
// requirement that made JsonValue RTTI-reflected. This needs the script VMs (a "weird dependency"
// for the JSON module), so it lives here, not in Json.Tests (which stays pure DOM).

#include <doctest/doctest.h>

#include <cstring>
#include <initializer_list>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.script;
#if INTEGRATION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#if INTEGRATION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::json;
using namespace foundation::script;

// The reflected surface scripts consume (a precursor sanity check before driving a VM).
TEST_CASE("integration.script: JsonValue's reflected surface is registered")
{
    RegisterJsonTypes();
    const TypeInfo& t = TypeOf<JsonValue>();
    CHECK(std::strcmp(t.namespaceName, "rtti::foundation::json") == 0);
    for (const char* m : {"MakeObject", "MakeNumber", "MakeBool", "Set", "Get", "AsNumber",
                          "AsString", "IsObject", "ToString", "Parse"})
    {
        CHECK_MESSAGE(FindMethod(t, m) != nullptr, m);
    }
}

#if INTEGRATION_HAS_ANGELSCRIPT
TEST_CASE("integration.script: parse / query / build / stringify from AngelScript")
{
    RegisterJsonTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    const Status status =
        scriptContext->Load(u8"double N = 0;\n"
                  u8"string NAME;\n"
                  u8"bool ISOBJ = false;\n"
                  u8"string OUT;\n"
                  u8"void main() {\n"
                  u8"  JsonValue@ doc = JsonValue::Parse(\"{\\\"n\\\":7,\\\"name\\\":\\\"tool\\\"}\");\n"
                  u8"  N = doc.Get(\"n\").AsNumber(0);\n"
                  u8"  NAME = doc.Get(\"name\").AsString();\n"
                  u8"  ISOBJ = doc.IsObject();\n"
                  u8"  JsonValue@ o = JsonValue::MakeObject();\n"
                  u8"  o.Set(\"id\", JsonValue::MakeNumber(1));\n"
                  u8"  o.Set(\"ok\", JsonValue::MakeBool(true));\n"
                  u8"  OUT = o.ToString(false);\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());

    CHECK(scriptContext->GetGlobal(u8"N").Get<f64>() == 7.0);
    CHECK(scriptContext->GetGlobal(u8"NAME").Get<String>() == StringView(u8"tool"));
    CHECK(scriptContext->GetGlobal(u8"ISOBJ").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"OUT").Get<String>() == StringView(u8"{\"id\":1,\"ok\":true}"));
}
#endif

#if INTEGRATION_HAS_LUAU
TEST_CASE("integration.script: parse / query / build / stringify from Luau")
{
    RegisterJsonTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();

    // Luau: statics via `.` (JsonValue.Parse), instance methods via `:`; the chunk runs at top level.
    const Status status =
        scriptContext->Load(u8"local doc = JsonValue.Parse(\"{\\\"n\\\":7,\\\"name\\\":\\\"tool\\\"}\")\n"
                  u8"N = doc:Get(\"n\"):AsNumber(0)\n"
                  u8"NAME = doc:Get(\"name\"):AsString()\n"
                  u8"ISOBJ = doc:IsObject()\n"
                  u8"local o = JsonValue.MakeObject()\n"
                  u8"o:Set(\"id\", JsonValue.MakeNumber(1))\n"
                  u8"o:Set(\"ok\", JsonValue.MakeBool(true))\n"
                  u8"OUT = o:ToString(false)\n",
                  u8"main");
    REQUIRE(status.IsOk());

    CHECK(scriptContext->GetGlobal(u8"N").Get<f64>() == 7.0);
    CHECK(scriptContext->GetGlobal(u8"NAME").Get<String>() == StringView(u8"tool"));
    CHECK(scriptContext->GetGlobal(u8"ISOBJ").Get<bool>() == true);
    CHECK(scriptContext->GetGlobal(u8"OUT").Get<String>() == StringView(u8"{\"id\":1,\"ok\":true}"));
}
#endif
