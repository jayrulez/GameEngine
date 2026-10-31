// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :log_tools partition
//
// The diagnostics tools: log_read, log_write and known_issues. The host registers one
// EditorLogBuffer sink on the global logger first thing in main, so every LOG_* line across the
// engine (cook warnings, scene-load errors, subsystem output) is captured with a monotonic
// sequence. log_read polls it incrementally (sinceSequence); log_write drops an agent marker into
// the same stream, to correlate agent actions with engine output; known_issues returns the
// engine's KnownIssues.md register so an agent can check whether a symptom is already known
// before diagnosing it again. All are read-only against the repo; log_write touches only the
// in-memory log.

module;
#include "Core/Prelude.h"

export module editor.mcp:log_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::mcp::detail
{
    // Level name <-> LogLevel for the tool's enum args (subset an agent filters by).
    inline Array<String> LogLevelChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"trace"));
        choices.PushBack(String(u8"debug"));
        choices.PushBack(String(u8"info"));
        choices.PushBack(String(u8"warning"));
        choices.PushBack(String(u8"error"));
        return choices;
    }

    inline LogLevel ParseLogLevel(StringView name, LogLevel fallback)
    {
        if (name == StringView(u8"trace"))
        {
            return LogLevel::Trace;
        }
        if (name == StringView(u8"debug"))
        {
            return LogLevel::Debug;
        }
        if (name == StringView(u8"info"))
        {
            return LogLevel::Info;
        }
        if (name == StringView(u8"warning"))
        {
            return LogLevel::Warning;
        }
        if (name == StringView(u8"error"))
        {
            return LogLevel::Error;
        }
        return fallback;
    }
}

export namespace editor::mcp
{
    // Registers log_read, log_write and known_issues. `logBuffer` is the host's sink (registered
    // on the global logger before anything else logs; it must outlive the server).
    // `knownIssuesPath` is the resolved absolute path of KnownIssues.md ("" when not found; the
    // tool then fails with guidance instead of being absent, so agents learn why).
    inline void RegisterLogTools(foundation::mcp::McpServer& server,
                                 editor::EditorLogBuffer& logBuffer, String knownIssuesPath)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        editor::EditorLogBuffer* buffer = &logBuffer;

        server.RegisterTool(
            u8"log_read",
            u8"Read the host's captured engine log (cook warnings, scene-load errors, subsystem "
            u8"output, agent markers). Poll incrementally: pass the lastSequence from the "
            u8"previous call as sinceSequence to get only what is new. Returns the newest "
            u8"`limit` matching entries; `dropped` > 0 means the ring overflowed and old "
            u8"entries were lost.",
            SchemaBuilder()
                .Number(u8"sinceSequence",
                        u8"only entries with sequence > this (default 0 = everything buffered)")
                .Integer(u8"limit", u8"maximum entries to return, newest kept (default 200)")
                .Enum(u8"minLevel", detail::LogLevelChoices(),
                      u8"only entries at or above this level (default trace = all)")
                .Str(u8"category", u8"only entries with exactly this category (e.g. Cook, Scene)")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [buffer](const JsonValue& args) -> ToolResult
            {
                const u64 since = static_cast<u64>(args.Get(u8"sinceSequence").AsNumber());
                const f64 limitRaw = args.Get(u8"limit").AsNumber();
                const usize limit = limitRaw > 0.0 ? static_cast<usize>(limitRaw) : usize(200);
                const LogLevel minLevel = detail::ParseLogLevel(
                    args.Get(u8"minLevel").AsString(), LogLevel::Trace);
                const String category = args.Get(u8"category").AsString();

                Array<editor::EditorLogEntry> collected;
                const u64 lastSequence = buffer->CollectSince(since, collected);

                Array<const editor::EditorLogEntry*> matching;
                for (const editor::EditorLogEntry& entry : collected)
                {
                    if (static_cast<u8>(entry.level) < static_cast<u8>(minLevel))
                    {
                        continue;
                    }
                    if (!category.IsEmpty() && entry.category != category.AsView())
                    {
                        continue;
                    }
                    matching.PushBack(&entry);
                }
                const usize start = matching.Size() > limit ? matching.Size() - limit : 0;

                JsonValue entries = JsonValue::MakeArray();
                for (usize i = start; i < matching.Size(); ++i)
                {
                    const editor::EditorLogEntry& entry = *matching[i];
                    JsonValue e = JsonValue::MakeObject();
                    e.Set(u8"sequence",
                          JsonValue::MakeNumber(static_cast<f64>(entry.sequence)));
                    e.Set(u8"level",
                          JsonValue::MakeString(String(StringView(LogLevelName(entry.level)))));
                    e.Set(u8"category", JsonValue::MakeString(entry.category));
                    e.Set(u8"message", JsonValue::MakeString(entry.message));
                    entries.Add(Move(e));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"entries", Move(entries));
                out.Set(u8"lastSequence",
                        JsonValue::MakeNumber(static_cast<f64>(lastSequence)));
                out.Set(u8"dropped",
                        JsonValue::MakeNumber(static_cast<f64>(buffer->DroppedCount())));
                return out;
            });

        server.RegisterTool(
            u8"log_write",
            u8"Write a marker line into the host's engine log (category 'Agent'). Use it to "
            u8"correlate your actions with engine output: drop a marker before a risky "
            u8"operation, then log_read from the returned sequence to see exactly what the "
            u8"engine said afterwards.",
            SchemaBuilder()
                .Str(u8"message", u8"the marker text", true)
                .Enum(u8"level", detail::LogLevelChoices(), u8"log level (default info)")
                .Build(),
                foundation::mcp::ToolAnnotations::Creates(),
            [buffer](const JsonValue& args) -> ToolResult
            {
                const String message = args.Get(u8"message").AsString();
                if (message.IsEmpty())
                {
                    return Err(String(u8"message must not be empty"));
                }
                const LogLevel level =
                    detail::ParseLogLevel(args.Get(u8"level").AsString(), LogLevel::Info);
                GlobalLogger().Dispatch(level, u8"Agent", message.AsView());

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"written", JsonValue::MakeBool(true));
                // The marker's sequence: the buffer's high-water right after the dispatch.
                out.Set(u8"sequence",
                        JsonValue::MakeNumber(static_cast<f64>(buffer->LatestSequence())));
                return out;
            });

        server.RegisterTool(
            u8"known_issues",
            u8"The engine's curated known-issues register (read-only): user-visible "
            u8"limitations with impact and workaround. Check it when you hit an error or odd "
            u8"behavior BEFORE re-diagnosing: if the symptom matches a recorded issue, report "
            u8"the match and apply its workaround instead of proposing a fix for something "
            u8"already known or deliberately deferred.",
            SchemaBuilder().Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [knownIssuesPath](const JsonValue& /*args*/) -> ToolResult
            {
                if (knownIssuesPath.IsEmpty())
                {
                    return Err(String(u8"KNOWN_ISSUES.md was not found near the host executable "
                                      u8"- run the MCP host from inside the engine checkout"));
                }
                FileStream stream(knownIssuesPath.AsView(), FileMode::Read);
                if (!stream.IsValid())
                {
                    return Err(Format(u8"could not read '{}'", knownIssuesPath.AsView()));
                }
                const i64 size = stream.Size();
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(size));
                if (stream.Read(bytes.Data(), bytes.Size()) != static_cast<u64>(size))
                {
                    return Err(Format(u8"could not read '{}'", knownIssuesPath.AsView()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"path", JsonValue::MakeString(knownIssuesPath));
                out.Set(u8"text",
                        JsonValue::MakeString(String(
                            StringView(reinterpret_cast<const utf8char*>(bytes.Data()),
                                       bytes.Size()))));
                return out;
            });
    }
}
