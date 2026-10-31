// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: the diagnostics tools log_read, log_write and known_issues. The loop an agent
// runs: drop a marker, make the engine log, then read incrementally from the marker's sequence and
// see exactly what happened after it. Also covers the filters and reading the known-issues
// register.
#include <doctest/doctest.h>
// the known-issues fixture writes through Core::WriteFile, not <cstdio>
#include <cstring>
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    JsonValue LogCallResponse(McpServer& s, StringView tool, JsonValue arguments)
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

    JsonValue LogCallOk(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = LogCallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    String LogCallErr(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = LogCallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
        return String(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue LogObj() { return JsonValue::MakeObject(); }

    bool AnyEntryContains(const JsonValue& entries, StringView needle)
    {
        for (i64 i = 0; i < entries.Count(); ++i)
        {
            const String msg(entries.At(i).Get(u8"message").AsString());
            if (msg == needle)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("integration.mcp: log tools - markers, incremental reads, filters, known issues")
{
    // As a host does it: one buffer sink on the global logger, registered for the test's scope.
    editor::EditorLogBuffer buffer(DefaultAllocator());
    GlobalLogger().AddSink(&buffer);

    // A stand-in known-issues register on disk.
    const char* issuesPath = "mcp_known_issues.md";
    {
        // Core's WriteFile, not stdio: the MSVC CRT marks fopen deprecated, so -Werror rejects
        // it on Windows. glibc does not deprecate it, which is why it builds on Linux.
        const char* text = "# Known Issues\n- I99: the teapot renders upside down\n";
        REQUIRE(WriteFile(StringView(reinterpret_cast<const utf8char*>(issuesPath)),
                          Span<const byte>(reinterpret_cast<const byte*>(text), std::strlen(text)))
                    .IsOk());
    }

    McpServer server;
    editor::mcp::RegisterLogTools(server, buffer,
                                  String(StringView(reinterpret_cast<const utf8char*>(issuesPath))));

    // log_write drops an Agent marker and reports its sequence.
    JsonValue marker = LogCallOk(server, u8"log_write", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"message", JsonValue::MakeString(u8"before-risky-op"));
        return a;
    }());
    CHECK(marker.Get(u8"written").AsBool() == true);
    const f64 markerSeq = marker.Get(u8"sequence").AsNumber();
    CHECK(markerSeq > 0.0);

    // The engine logs after the marker.
    LOG_WARNING(u8"Cook", u8"teapot failed to cook");
    LOG_INFO(u8"Scene", u8"loaded arena");

    // Read everything: the marker is there, category Agent.
    JsonValue all = LogCallOk(server, u8"log_read", LogObj());
    CHECK(AnyEntryContains(all.Get(u8"entries"), u8"before-risky-op"));
    CHECK(all.Get(u8"lastSequence").AsNumber() >= markerSeq + 2.0);

    // Incremental: from the marker forward, exactly the two engine lines and nothing stale.
    JsonValue after = LogCallOk(server, u8"log_read", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"sinceSequence", JsonValue::MakeNumber(markerSeq));
        return a;
    }());
    CHECK(after.Get(u8"entries").Count() == 2);
    CHECK(AnyEntryContains(after.Get(u8"entries"), u8"teapot failed to cook"));
    CHECK(!AnyEntryContains(after.Get(u8"entries"), u8"before-risky-op"));

    // Filters: minLevel=warning drops the info line; category=Scene keeps only the scene line.
    JsonValue warnings = LogCallOk(server, u8"log_read", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"sinceSequence", JsonValue::MakeNumber(markerSeq));
        a.Set(u8"minLevel", JsonValue::MakeString(u8"warning"));
        return a;
    }());
    CHECK(warnings.Get(u8"entries").Count() == 1);
    CHECK(AnyEntryContains(warnings.Get(u8"entries"), u8"teapot failed to cook"));
    JsonValue sceneOnly = LogCallOk(server, u8"log_read", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"sinceSequence", JsonValue::MakeNumber(markerSeq));
        a.Set(u8"category", JsonValue::MakeString(u8"Scene"));
        return a;
    }());
    CHECK(sceneOnly.Get(u8"entries").Count() == 1);
    CHECK(AnyEntryContains(sceneOnly.Get(u8"entries"), u8"loaded arena"));

    // Nothing new past the high-water mark.
    JsonValue quiet = LogCallOk(server, u8"log_read", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"sinceSequence", all.Get(u8"lastSequence"));
        return a;
    }());
    CHECK(quiet.Get(u8"entries").Count() == 0);

    // Refusal: an empty marker never writes.
    CHECK(LogCallErr(server, u8"log_write", [&]
    {
        JsonValue a = LogObj();
        a.Set(u8"message", JsonValue::MakeString(u8""));
        return a;
    }()).Size() > 0u);

    // known_issues returns the register verbatim; a host without one errs with guidance.
    JsonValue issues = LogCallOk(server, u8"known_issues", LogObj());
    const String text(issues.Get(u8"text").AsString());
    CHECK(text.Size() > 0u);
    CHECK(String(issues.Get(u8"path").AsString()).Size() > 0u);
    {
        McpServer bare;
        editor::EditorLogBuffer bareBuffer(DefaultAllocator());
        editor::mcp::RegisterLogTools(bare, bareBuffer, String());
        const String why = LogCallErr(bare, u8"known_issues", LogObj());
        // It names the file the hosts actually look for.
        CHECK(why.AsView().ContainsIgnoreCase(u8"KnownIssues.md"));
    }

    GlobalLogger().RemoveSink(&buffer);
    (void)FileDelete(StringView(reinterpret_cast<const utf8char*>(issuesPath)));
}
