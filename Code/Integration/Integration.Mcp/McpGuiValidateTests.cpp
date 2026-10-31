// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: gui_validate. A document that builds is valid and lists the asset guids it
// names; a document that does not build is invalid and reports its error's line. A sheet always
// reads, and an unknown property is a warning on the line of its rule.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    JsonValue GuiValidate(McpServer& s, StringView kind, StringView source)
    {
        JsonValue arguments = JsonValue::MakeObject();
        arguments.Set(u8"kind", JsonValue::MakeString(String(kind)));
        arguments.Set(u8"source", JsonValue::MakeString(String(source)));
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(u8"gui_validate"));
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
        return JsonValue::Parse(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }
}

TEST_CASE("integration.mcp: gui_validate - a document that builds, one that does not, a sheet's warning")
{
    McpServer server;
    editor::mcp::RegisterGuiValidateTool(server);

    const JsonValue good = GuiValidate(server, u8"document",
                                       u8"<Panel class=\"hud\">\n  <Image src=\"{0b9d6a52-1c1e-4e0f-9a43-7d2f61c0aa01}\"/>\n  <Label text=\"0\"/>\n</Panel>\n");
    CHECK(good.Get(u8"valid").AsBool());
    REQUIRE(good.Get(u8"references").Count() == 1);
    CHECK(good.Get(u8"references").At(0).AsString().AsView().ContainsIgnoreCase(u8"0b9d6a52"));

    const JsonValue broken = GuiValidate(server, u8"document", u8"<Panel>\n  <Label text=\"0\">\n</Panel\n");
    CHECK_FALSE(broken.Get(u8"valid").AsBool());
    REQUIRE(broken.Get(u8"diagnostics").Count() > 0);
    CHECK(broken.Get(u8"diagnostics").At(0).Get(u8"level").AsString() == StringView(u8"error"));

    // The name check reports the line of the rule holding the property.
    const JsonValue sheet = GuiValidate(server, u8"sheet", u8".a { padding: 2px; }\n\n.hud { padding: 8px; not-a-property: 3; }\n");
    CHECK(sheet.Get(u8"valid").AsBool());
    REQUIRE(sheet.Get(u8"diagnostics").Count() > 0);
    const JsonValue warning = sheet.Get(u8"diagnostics").At(0);
    CHECK(warning.Get(u8"level").AsString() == StringView(u8"warning"));
    CHECK(warning.Get(u8"line").AsNumber() == doctest::Approx(3.0));
    CHECK(warning.Get(u8"message").AsString().AsView().ContainsIgnoreCase(u8"not-a-property"));
}
