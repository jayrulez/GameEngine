// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The MCP activity: each tools/call the host reports becomes one entry (added at once, or when it
// starts running and closed when it answers or is abandoned), the host's own events sit between
// them, a stopped host abandons what was still running, and the entries are capped. A row's summary
// is a failure's first line, else the arguments as name: value pairs.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.app;

using namespace foundation::core;
using foundation::json::JsonValue;
using editor::app::McpActivity;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolCallPhase;
using foundation::mcp::ToolCallReport;

namespace
{
    ToolCallReport Report(u64 serial, ToolCallPhase phase, StringView tool, const JsonValue& arguments, ToolAnnotations annotations,
                          bool isError = false, StringView result = {}, f64 seconds = 0.0)
    {
        ToolCallReport report;
        report.serial = serial;
        report.phase = phase;
        report.tool = tool;
        report.arguments = &arguments;
        report.annotations = annotations;
        report.isError = isError;
        report.result = result;
        report.elapsed = Duration::FromSeconds(seconds);
        return report;
    }
}

TEST_CASE("mcp activity: an at-once call is one finished entry; a running one is closed by its own serial")
{
    McpActivity activity(DefaultAllocator());
    JsonValue args = JsonValue::MakeObject();
    args.Set(u8"entity", JsonValue::MakeString(u8"Lamp 3"));
    args.Set(u8"value", JsonValue::MakeNumber(4.5));

    const u64 before = activity.Revision();
    activity.Record(Report(1, ToolCallPhase::Finished, u8"component_set", args, ToolAnnotations::Overwrites(), false, u8"{\"after\":4.5}"),
                    1000);
    CHECK(activity.Revision() != before);
    REQUIRE(activity.Size() == 1u);
    const McpActivity::Entry& set = activity.At(0);
    CHECK(set.IsCall());
    CHECK(set.outcome == McpActivity::Outcome::Done);
    CHECK(set.tool == u8"component_set");
    CHECK(set.arguments == u8"{\"entity\":\"Lamp 3\",\"value\":4.5}");
    CHECK(set.result == u8"{\"after\":4.5}");
    CHECK(set.changes);
    CHECK(set.at == 1000);
    CHECK(activity.RunningCall() == nullptr);

    // A cook runs across pumps: Started adds it running, a read lands meanwhile, then it answers.
    JsonValue none = JsonValue::MakeObject();
    activity.Record(Report(2, ToolCallPhase::Started, u8"asset_cook", none, ToolAnnotations::Rebuilds()), 1010);
    REQUIRE(activity.RunningCall() != nullptr);
    CHECK(activity.RunningCall()->tool == u8"asset_cook");
    CHECK(activity.RunningCall()->outcome == McpActivity::Outcome::Running);
    CHECK_FALSE(activity.RunningCall()->changes); // a rebuild of derived output
    activity.Record(Report(3, ToolCallPhase::Finished, u8"asset_list", none, ToolAnnotations::ReadOnly()), 1011);
    activity.Record(Report(2, ToolCallPhase::Finished, u8"asset_cook", none, ToolAnnotations::Rebuilds(), true, u8"cook failed\nat step 2", 4.0),
                    1014);
    REQUIRE(activity.Size() == 3u);
    CHECK(activity.At(1).tool == u8"asset_cook");
    CHECK(activity.At(1).outcome == McpActivity::Outcome::Failed);
    CHECK(activity.At(1).elapsed == Duration::FromSeconds(4.0));
    CHECK(activity.At(1).at == 1010); // when it started, not when it answered
    CHECK(activity.At(2).tool == u8"asset_list");
    CHECK(activity.RunningCall() == nullptr);
    CHECK(activity.LastCall() == &activity.At(2));

    // Its caller left: abandoned. An Abandoned for a call never heard Started adds nothing.
    activity.Record(Report(4, ToolCallPhase::Started, u8"pie_run", none, ToolAnnotations::Adjusts()), 1020);
    activity.Record(Report(4, ToolCallPhase::Abandoned, u8"pie_run", none, ToolAnnotations::Adjusts()), 1030);
    CHECK(activity.At(3).outcome == McpActivity::Outcome::Abandoned);
    activity.Record(Report(9, ToolCallPhase::Abandoned, u8"pie_run", none, ToolAnnotations::Adjusts()), 1031);
    CHECK(activity.Size() == 4u);

    // Refused before any tool ran: an entry of its own with the refusal, never an edit.
    activity.Record(Report(10, ToolCallPhase::Refused, u8"scene_write", none, ToolAnnotations::Overwrites(), true,
                           u8"scene_write: missing required field 'source'"),
                    1040);
    REQUIRE(activity.Size() == 5u);
    CHECK(activity.At(4).outcome == McpActivity::Outcome::Refused);
    CHECK_FALSE(activity.At(4).changes);
    CHECK(activity.At(4).result == u8"scene_write: missing required field 'source'");
    CHECK(McpActivity::Summary(activity.At(4)) == u8"scene_write: missing required field 'source'");
    CHECK(activity.RunningCall() == nullptr);
}

TEST_CASE("mcp activity: the host's events sit between the calls, and a stop abandons what still runs")
{
    McpActivity activity(DefaultAllocator());
    JsonValue none = JsonValue::MakeObject();
    activity.RecordEvent(u8"Listening on 127.0.0.1:7405", 100);
    activity.Record(Report(1, ToolCallPhase::Started, u8"asset_cook", none, ToolAnnotations::Rebuilds()), 101);
    activity.HostStopped(102);
    REQUIRE(activity.Size() == 3u);
    CHECK_FALSE(activity.At(0).IsCall());
    CHECK(activity.At(0).tool == u8"Listening on 127.0.0.1:7405");
    CHECK(activity.At(1).outcome == McpActivity::Outcome::Abandoned);
    CHECK(activity.At(2).tool == u8"Stopped");
    CHECK(activity.RunningCall() == nullptr);
    // The next host numbers its calls from 1 again: a new call, not the old one finishing.
    activity.Record(Report(1, ToolCallPhase::Finished, u8"host_info", none, ToolAnnotations::ReadOnly()), 103);
    CHECK(activity.Size() == 4u);
    CHECK(activity.At(1).outcome == McpActivity::Outcome::Abandoned);
    activity.Clear();
    CHECK(activity.Size() == 0u);
    CHECK(activity.LastCall() == nullptr);
}

TEST_CASE("mcp activity: the entries are capped, the oldest trimmed")
{
    McpActivity activity(DefaultAllocator());
    JsonValue none = JsonValue::MakeObject();
    for (u64 i = 1; i <= editor::app::kMcpActivityMaxEntries + 5; ++i)
        activity.Record(Report(i, ToolCallPhase::Finished, u8"host_info", none, ToolAnnotations::ReadOnly()), static_cast<i64>(i));
    CHECK(activity.Size() == editor::app::kMcpActivityMaxEntries);
    CHECK(activity.At(0).at == 6);
    CHECK(activity.FindById(activity.At(0).id) == &activity.At(0));
}

TEST_CASE("mcp activity: a row's summary is a failure's first line, else the arguments by name, cut short")
{
    McpActivity::Entry call;
    call.arguments = String(u8"{\"path\":\"Content/Scenes/Level1.scene\",\"force\":true,\"ids\":[1,2]}");
    CHECK(McpActivity::Summary(call) == u8"path: Content/Scenes/Level1.scene, force: true, ids: [1,2]");
    CHECK(McpActivity::Summary(call, 10) == u8"path: Cont...");
    call.arguments = String(u8"{}");
    CHECK(McpActivity::Summary(call).IsEmpty());
    call.outcome = McpActivity::Outcome::Failed;
    call.result = String(u8"Scripts/Player.luau: 'speed' is not a member\nline 4");
    CHECK(McpActivity::Summary(call) == u8"Scripts/Player.luau: 'speed' is not a member");
    McpActivity::Entry event;
    event.kind = McpActivity::Kind::Event;
    event.tool = String(u8"claude-code 2.1.0 connected");
    CHECK(McpActivity::Summary(event) == u8"claude-code 2.1.0 connected");
}
