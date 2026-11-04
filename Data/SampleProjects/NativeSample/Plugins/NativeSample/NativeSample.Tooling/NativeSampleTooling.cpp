// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Tooling's entry. Load adds an MCP tool, nativesample_notes, that every MCP host serves
// while the plugin is loaded: the open project's notes (the Pipeline part's asset type), each with
// its text; and an export hook that writes how many notes the project has into each dist
// (nativesample-notes.txt). Both leave with the plugin.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import editor.project;
import editor.mcp; // McpPluginServices
import nativesample.pipeline;

namespace
{
    using foundation::json::JsonValue;

    void Load(foundation::runtime::PluginContext& context)
    {
        // The exporter, the editor and the MCP host run export hooks; each offers the service.
        if (editor::ExportPluginServices* exports = context.Find<editor::ExportPluginServices>())
        {
            (void)exports->AddHook(
                u8"nativesample",
                [](const editor::ExportHookContext& at) -> foundation::core::String
                {
                    foundation::core::usize count = 0;
                    nativesample::ForEachNote(*at.project->SourceDb().RootGroup(),
                                              [&count](foundation::content::Instance&) { ++count; });
                    const foundation::core::String text = foundation::core::Format(u8"{}\n", count);
                    const foundation::core::String path = foundation::core::PathJoin(at.outputDir, u8"nativesample-notes.txt");
                    if (!foundation::core::WriteFile(path.AsView(), foundation::core::Span<const foundation::core::byte>(
                                                                         reinterpret_cast<const foundation::core::byte*>(text.Data()), text.Size()))
                             .IsOk())
                    {
                        return foundation::core::String(u8"could not write nativesample-notes.txt");
                    }
                    return foundation::core::String{};
                });
        }
        editor::mcp::McpPluginServices* tools = context.Find<editor::mcp::McpPluginServices>();
        if (tools == nullptr)
        {
            return;
        }
        (void)tools->AddTools(
            [](editor::mcp::PluginTools& registry)
            {
                editor::mcp::ProjectSession* session = &registry.Session();
                (void)registry.RegisterTool(
                    foundation::core::String(u8"nativesample_notes"),
                    foundation::core::String(u8"The open project's notes (NativeSample's own asset type), each its name, "
                                             u8"guid and text."),
                    foundation::mcp::SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
                    [session](const JsonValue&) -> foundation::mcp::ToolResult
                    {
                        if (session->project == nullptr)
                        {
                            return foundation::core::Err(foundation::core::String(u8"no project is open (call project_open first)"));
                        }
                        JsonValue notes = JsonValue::MakeArray();
                        nativesample::ForEachNote(*session->project->SourceDb().RootGroup(),
                                                  [&notes](foundation::content::Instance& instance)
                                                  {
                                                      JsonValue note = JsonValue::MakeObject();
                                                      note.Set(u8"name", JsonValue::MakeString(foundation::core::String(instance.Name())));
                                                      foundation::core::utf8char id[37];
                                                      instance.Id().ToChars(id);
                                                      note.Set(u8"guid", JsonValue::MakeString(foundation::core::String(foundation::core::StringView(id, 36))));
                                                      foundation::core::RefPtr<foundation::core::ISerializable> read = instance.ReadObject();
                                                      if (auto* asset = read ? foundation::core::Cast<nativesample::NoteAsset>(read.Get()) : nullptr)
                                                      {
                                                          note.Set(u8"text", JsonValue::MakeString(asset->text));
                                                      }
                                                      notes.Add(foundation::core::Move(note));
                                                  });
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"count", JsonValue::MakeNumber(static_cast<foundation::core::f64>(notes.Count())));
                        out.Set(u8"notes", foundation::core::Move(notes));
                        return out;
                    });
            });
    }
}

PLUGIN_PART(&Load, nullptr)
