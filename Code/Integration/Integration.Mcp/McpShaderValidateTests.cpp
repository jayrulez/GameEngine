// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp - shader_validate: the New Shader starter checks clean against the engine's
// shader library, with its properties and both stages' variants reported; a broken shader comes
// back with the file, line and column of its error.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.vfs;
import foundation.shaders;
import shaders.pipeline;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    JsonValue ShaderValidate(McpServer& s, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(u8"shader_validate"));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue resp = json::Parse(line.response.AsView()).value;
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    bool HaveCompiler()
    {
        foundation::shaders::Compiler* compiler = nullptr;
        if (!foundation::shaders::createCompiler(foundation::shaders::CompilerDesc{}, compiler)
                 .IsOk() ||
            compiler == nullptr)
        {
            return false;
        }
        compiler->Destroy();
        return true;
    }
}

TEST_CASE("integration.mcp: shader_validate - the starter checks clean; a broken shader "
          "reports its line")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String library = foundation::vfs::DataPath(dataRoot.AsView(), u8"Engine/Shaders");
    McpServer server;
    editor::mcp::RegisterShaderValidateTool(server, library.AsView());

    JsonValue args = JsonValue::MakeObject();
    args.Set(u8"source", JsonValue::MakeString(String(pipeline::kShaderStarter)));
    args.Set(u8"name", JsonValue::MakeString(u8"Lava.hlsl"));
    const JsonValue good = ShaderValidate(server, Move(args));
    CHECK(good.Get(u8"valid").AsBool());
    CHECK(good.Get(u8"errors").Count() == 0);
    const JsonValue& properties = good.Get(u8"properties");
    REQUIRE(properties.Count() == 6);
    CHECK(properties.At(0).Get(u8"name").AsString() == StringView(u8"BaseColor"));
    CHECK(properties.At(0).Get(u8"type").AsString() == StringView(u8"Color"));
    CHECK(properties.At(1).Get(u8"range").Count() == 2); // Roughness [0, 1]
    CHECK(good.Get(u8"vertexVariants").Count() == 2);    // SKINNED INSTANCED
    CHECK(good.Get(u8"fragmentVariants").Count() == 3);  // ALPHA_TEST GBUFFER DITHER
    CHECK(good.Get(u8"kind").AsString() == StringView(u8"lowLevel"));

    // A surface shader: its kind and its Vertex().
    JsonValue surface = JsonValue::MakeObject();
    surface.Set(u8"source", JsonValue::MakeString(String(pipeline::kLitSurfaceShaderStarter)));
    const JsonValue lava = ShaderValidate(server, Move(surface));
    CHECK(lava.Get(u8"valid").AsBool());
    CHECK(lava.Get(u8"kind").AsString() == StringView(u8"lit"));
    CHECK_FALSE(lava.Get(u8"hasVertex").AsBool());
    CHECK(lava.Get(u8"properties").Count() == 5);

    JsonValue broken = JsonValue::MakeObject();
    broken.Set(u8"source",
               JsonValue::MakeString(
                   u8"properties\n{\n    float Glow = 1;\n}\n"
                   u8"float4 VSMain(uint id : SV_VertexID) : SV_Position { return 0; }\n"
                   u8"float4 PSMain() : SV_Target0 { return Glw; }\n"));
    broken.Set(u8"name", JsonValue::MakeString(u8"Bad.hlsl"));
    const JsonValue bad = ShaderValidate(server, Move(broken));
    CHECK_FALSE(bad.Get(u8"valid").AsBool());
    REQUIRE(bad.Get(u8"errors").Count() >= 1);
    const JsonValue& error = bad.Get(u8"errors").At(0);
    CHECK(error.Get(u8"file").AsString() == StringView(u8"Bad.hlsl"));
    CHECK(error.Get(u8"line").AsNumber() == 6.0);
    CHECK(error.Get(u8"column").AsNumber() > 0.0);
}
