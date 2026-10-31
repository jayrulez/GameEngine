// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.mcp.reflection;

using namespace foundation::core;
using namespace foundation::mcp;
using foundation::json::JsonValue;
namespace json = foundation::json;

namespace
{
    // Build and send a tools/call and return the parsed response (through the real JSON-RPC path).
    JsonValue CallResponse(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        return json::Parse(line.response.AsView()).value;
    }

    // The tool's JSON payload (result.content[0].text, re-parsed), asserting a non-error result.
    JsonValue CallOk(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = CallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    bool HasTypeNamed(const JsonValue& typeList, StringView name)
    {
        const JsonValue types = typeList.Get(u8"types");
        for (i64 i = 0; i < types.Count(); ++i)
        {
            if (types.At(i).Get(u8"name").AsString() == name)
            {
                return true;
            }
        }
        return false;
    }

    JsonValue Arg(StringView key, StringView value)
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(String(key), JsonValue::MakeString(String(value)));
        return a;
    }
}

TEST_CASE("mcp.reflection: type_list surfaces registered types + honors the namespace filter")
{
    RegisterCoreTypes();
    json::RegisterJsonTypes();
    McpServer s;
    RegisterReflectionTools(s);

    const JsonValue all = CallOk(s, u8"type_list", JsonValue::MakeObject());
    CHECK(all.Get(u8"count").AsInt() > 0);
    CHECK(HasTypeNamed(all, u8"JsonValue"));
    CHECK(HasTypeNamed(all, u8"Float3"));

    const JsonValue jsonOnly = CallOk(s, u8"type_list", Arg(u8"namespace", u8"rtti::foundation::json"));
    CHECK(HasTypeNamed(jsonOnly, u8"JsonValue"));
    CHECK_FALSE(HasTypeNamed(jsonOnly, u8"Float3")); // Float3 lives in rtti::core
}

TEST_CASE("mcp.reflection: type_info describes namespace + methods (params/returns)")
{
    RegisterCoreTypes();
    json::RegisterJsonTypes();
    McpServer s;
    RegisterReflectionTools(s);

    const JsonValue info = CallOk(s, u8"type_info", Arg(u8"type", u8"JsonValue"));
    CHECK(info.Get(u8"name").AsString() == StringView(u8"JsonValue"));
    CHECK(info.Get(u8"namespace").AsString() == StringView(u8"rtti::foundation::json"));

    const JsonValue methods = info.Get(u8"methods");
    CHECK(methods.Count() > 0);
    bool hasParse = false;
    bool hasSet = false;
    for (i64 i = 0; i < methods.Count(); ++i)
    {
        const String name = methods.At(i).Get(u8"name").AsString();
        if (name == StringView(u8"Parse"))
        {
            hasParse = true;
            // Parse takes one string param.
            CHECK(methods.At(i).Get(u8"params").Count() == 1);
        }
        if (name == StringView(u8"Set"))
        {
            hasSet = true;
            CHECK(methods.At(i).Get(u8"params").Count() == 2); // key, value
        }
    }
    CHECK(hasParse);
    CHECK(hasSet);
}

TEST_CASE("mcp.reflection: type_info on an unknown type is a TOOL error (isError, not a protocol fault)")
{
    McpServer s;
    RegisterReflectionTools(s);

    JsonValue resp = CallResponse(s, u8"type_info", Arg(u8"type", u8"NoSuchType"));
    REQUIRE(resp.Has(u8"result"));
    CHECK_FALSE(resp.Has(u8"error"));
    CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
    CHECK(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().Size() > 0u);
}
