// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :gui_validate partition
//
// gui_validate: check the engine GUI's markup (a `.gml` document) or a sheet (`.css`) WITHOUT saving
// it, through the check its cook runs (the markup loader's or the CSS parser's diagnostics: what was
// dropped a warning, markup that does not build an error), so an agent writing a game's screens
// loops on it as on script_validate and shader_validate. Returns each diagnostic with its line, and
// the asset guids the text names (`{guid}`), each a dependency of its cook.

module;
#include "Core/Prelude.h"

export module editor.mcp:gui_validate;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.gui;
import gui.pipeline;
import editor.project; // EditorRootAllocator

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace editor::mcp
{
    // Registers gui_validate. Project-independent (checks in-memory text).
    inline void RegisterGuiValidateTool(foundation::mcp::McpServer& server)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;

        Array<String> kinds;
        kinds.PushBack(String(u8"document"));
        kinds.PushBack(String(u8"sheet"));
        server.RegisterTool(
            u8"gui_validate",
            u8"CHECK the engine GUI's text without saving: a `document` (.gml markup) or a `sheet` "
            u8"(.css), the exact check its cook runs. Returns `valid` (false only when markup does not "
            u8"build; a sheet always reads), each diagnostic {level: error | warning, line (from 1, 0 "
            u8"when not known), message} (what the loader or the parser dropped, a property name "
            u8"nothing takes), and `references`, the asset guids the text names ({guid}: images, "
            u8"fonts, sheets, documents). Use as the loop while writing a game's screens, then edit "
            u8"the asset's linked source file and cook; the editor's page for it previews it live.",
            SchemaBuilder()
                .Str(u8"source", u8"the full text", true)
                .Enum(u8"kind", Move(kinds), u8"document (.gml) or sheet (.css)", true)
                .Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [](const JsonValue& args) -> ToolResult
            {
                const String source = args.Get(u8"source").AsString();
                const bool document = args.Get(u8"kind").AsString().AsView() == StringView(u8"document");
                Array<foundation::gui::GuiDiagnostic> found;
                bool valid = true;
                if (document)
                    valid = pipeline::ValidateGuiDocument(source.AsView(), found, editor::EditorRootAllocator());
                else
                    pipeline::ValidateGuiStyleSheet(source.AsView(), found);

                JsonValue diagnostics = JsonValue::MakeArray();
                for (const foundation::gui::GuiDiagnostic& d : found)
                {
                    JsonValue entry = JsonValue::MakeObject();
                    entry.Set(u8"level", JsonValue::MakeString(String(d.Level == foundation::gui::GuiDiagnostic::Severity::Error ? StringView(u8"error") : StringView(u8"warning"))));
                    entry.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(d.Line)));
                    entry.Set(u8"message", JsonValue::MakeString(d.Message));
                    diagnostics.Add(Move(entry));
                }
                JsonValue references = JsonValue::MakeArray();
                for (const Guid& id : pipeline::FindGuidReferences(source.AsView()))
                    references.Add(JsonValue::MakeString(Format(u8"{}", id)));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"valid", JsonValue::MakeBool(valid));
                out.Set(u8"diagnostics", Move(diagnostics));
                out.Set(u8"references", Move(references));
                return out;
            });
    }
}
