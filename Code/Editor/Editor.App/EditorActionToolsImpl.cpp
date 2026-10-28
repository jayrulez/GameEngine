// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - the action tools' bodies.
module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;

namespace editor::app
{
    namespace
    {
        // The seams outlive the server through the handlers' shared ref (Function is move-only).
        class ActionToolState final : public RefCounted
        {
        public:
            explicit ActionToolState(ActionToolSeams toolSeams) : seams(Move(toolSeams)) {}
            ActionToolSeams seams;
        };

        StringView KindName(editor::EditorActionKind kind) noexcept
        {
            switch (kind)
            {
            case editor::EditorActionKind::Toggle:
                return u8"toggle";
            case editor::EditorActionKind::Window:
                return u8"window";
            case editor::EditorActionKind::Command:
            default:
                return u8"command";
            }
        }

        /// One action as the agent sees it, its state over the active page.
        JsonValue ActionJson(const editor::EditorActionRegistry& actions,
                             const editor::EditorActionDeclaration& action)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"id", JsonValue::MakeString(action.id));
            out.Set(u8"label", JsonValue::MakeString(action.label));
            out.Set(u8"description", JsonValue::MakeString(action.description));
            out.Set(u8"menuPath", JsonValue::MakeString(action.menuPath));
            out.Set(u8"kind", JsonValue::MakeString(String(KindName(action.kind))));
            out.Set(u8"readOnly", JsonValue::MakeBool(action.readOnly));
            out.Set(u8"shortcut", JsonValue::MakeString(editor::FormatShortcut(
                                      actions.Shortcut(action.id.AsView()))));
            editor::EditorPage* subject = actions.Subject();
            out.Set(u8"enabled",
                    JsonValue::MakeBool(editor::EditorActionRegistry::IsEnabled(action, subject)));
            out.Set(u8"checked",
                    JsonValue::MakeBool(editor::EditorActionRegistry::IsChecked(action, subject)));
            // Why it is disabled, when it says (a tool with nothing to work on): what to change.
            const String reason = actions.DisabledReason(action.id.AsView(), subject);
            if (!reason.IsEmpty())
                out.Set(u8"disabledReason", JsonValue::MakeString(reason));
            return out;
        }

        Result<const editor::EditorActionDeclaration*, String>
        FindAction(const editor::EditorActionRegistry& actions, const JsonValue& args)
        {
            const String id = args.Get(u8"id").AsString();
            const editor::EditorActionDeclaration* action = actions.Find(id.AsView());
            if (action == nullptr)
            {
                return Err(Format(u8"no action '{}' (action_list names them all)", id.AsView()));
            }
            return action;
        }
    }

    void RegisterActionTools(foundation::mcp::McpServer& server, ActionToolSeams seams)
    {
        RefPtr<ActionToolState> state =
            MakeRef<ActionToolState>(editor::EditorRootAllocator(), Move(seams));

        server.RegisterTool(
            u8"action_list",
            u8"Every editor action - what a user can do by menu, chord, toolbar or palette - "
            u8"with its state over the ACTIVE page: id, label, description, menuPath, kind "
            u8"(command / toggle / window), readOnly, shortcut, enabled, checked. Page-bound "
            u8"actions are disabled when no page of their kind is active: page_open one first.",
            SchemaBuilder().Build(), ToolAnnotations::ReadOnly(),
            [state](const JsonValue&) -> ToolResult
            {
                const editor::EditorActionRegistry& actions = state->seams.context->Actions();
                JsonValue list = JsonValue::MakeArray();
                for (const editor::EditorActionDeclaration& action : actions.Actions())
                {
                    list.Add(ActionJson(actions, action));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(actions.Count())));
                out.Set(u8"actions", Move(list));
                return out;
            });

        server.RegisterTool(
            u8"action_state",
            u8"One action's declaration and its state over the active page (as action_list "
            u8"shows it).",
            SchemaBuilder().Str(u8"id", u8"the action's id (from action_list)", true).Build(),
            ToolAnnotations::ReadOnly(),
            [state](const JsonValue& args) -> ToolResult
            {
                const editor::EditorActionRegistry& actions = state->seams.context->Actions();
                Result<const editor::EditorActionDeclaration*, String> action = FindAction(actions, args);
                if (!action.HasValue())
                {
                    return Err(Move(action.Error()));
                }
                return ActionJson(actions, *action.Value());
            });

        server.RegisterTool(
            u8"action_execute",
            u8"Run an editor action over the active page, exactly as a menu click or its chord "
            u8"would - through the one funnel every surface uses. REFUSED when the action is "
            u8"not enabled over the active page (action_state says why to look). Runs "
            u8"UNATTENDED: any dialog the action would open is kept off the screen and closed "
            u8"as cancelled, and the result lists it under `suppressedDialogs` - the action "
            u8"then most likely did nothing; use a dedicated tool for that step, or ask the "
            u8"user. Returns {id, executed, suppressedDialogs, enabled, checked} (the state "
            u8"after).",
            SchemaBuilder().Str(u8"id", u8"the action's id (from action_list)", true).Build(),
            ToolAnnotations::Overwrites(),
            [state](const JsonValue& args) -> ToolResult
            {
                const ActionToolSeams& toolSeams = state->seams;
                const editor::EditorActionRegistry& actions = toolSeams.context->Actions();
                Result<const editor::EditorActionDeclaration*, String> found = FindAction(actions, args);
                if (!found.HasValue())
                {
                    return Err(Move(found.Error()));
                }
                const editor::EditorActionDeclaration& action = *found.Value();
                if (!editor::EditorActionRegistry::IsEnabled(action, actions.Subject()))
                {
                    const String reason = actions.DisabledReason(action.id.AsView(), actions.Subject());
                    if (!reason.IsEmpty())
                        return Err(Format(u8"action '{}' is not enabled over the active page ({}): {}", action.id.AsView(),
                                          actions.Subject() != nullptr ? actions.Subject()->Title() : StringView(u8"no page is active"),
                                          reason.AsView()));
                    return Err(Format(u8"action '{}' is not enabled over the active page ({}) - "
                                      u8"page_open the page it needs, or select what it acts on",
                                      action.id.AsView(),
                                      actions.Subject() != nullptr
                                          ? actions.Subject()->Title()
                                          : StringView(u8"no page is active")));
                }
                Array<String> suppressed;
                {
                    UnattendedDialogs unattended(toolSeams.dialogs);
                    const Status ran = actions.Execute(action.id.AsView());
                    if (!ran.IsOk())
                    {
                        return Err(Format(u8"action '{}' could not run (status {})",
                                          action.id.AsView(), static_cast<i32>(ran.Code())));
                    }
                    for (const String& title : unattended.Suppressed())
                    {
                        suppressed.PushBack(title);
                    }
                }
                JsonValue dialogs = JsonValue::MakeArray();
                for (const String& title : suppressed)
                {
                    dialogs.Add(JsonValue::MakeString(title));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"id", JsonValue::MakeString(action.id));
                out.Set(u8"executed", JsonValue::MakeBool(true));
                out.Set(u8"suppressedDialogs", Move(dialogs));
                if (!suppressed.IsEmpty())
                {
                    out.Set(u8"note",
                            JsonValue::MakeString(
                                u8"the action opened a dialog that was closed unattended - it most "
                                u8"likely did nothing; use a dedicated tool for that step, or ask "
                                u8"the user"));
                }
                editor::EditorPage* subject = actions.Subject();
                out.Set(u8"enabled", JsonValue::MakeBool(
                                         editor::EditorActionRegistry::IsEnabled(action, subject)));
                out.Set(u8"checked", JsonValue::MakeBool(
                                         editor::EditorActionRegistry::IsChecked(action, subject)));
                return out;
            });
    }
}
