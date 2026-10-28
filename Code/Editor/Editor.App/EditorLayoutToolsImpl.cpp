// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - the layout tools' bodies.
module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.gui.docking;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;
namespace gui = foundation::gui;

namespace editor::app
{
    namespace
    {
        JsonValue PanelJson(const gui::DockController& dock, StringView panel)
        {
            const gui::DockPanelDescription* description = dock.Describe(panel);
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"panel", JsonValue::MakeString(String(panel)));
            out.Set(u8"title", JsonValue::MakeString(description != nullptr && !description->Title.IsEmpty() ? description->Title : String(panel)));
            return out;
        }

        // A node of the dock tree: a split with its children and their weights, or a stack of tabs.
        JsonValue NodeJson(const gui::DockController& dock, gui::DockNodeId id)
        {
            const gui::DockNode* node = dock.GetLayout().FindNode(id);
            if (node == nullptr)
                return JsonValue{};
            JsonValue out = JsonValue::MakeObject();
            if (node->IsStack())
            {
                JsonValue tabs = JsonValue::MakeArray();
                for (const String& panel : node->Panels)
                    tabs.Add(PanelJson(dock, panel.AsView()));
                out.Set(u8"tabs", Move(tabs));
                if (!node->Panels.IsEmpty())
                    out.Set(u8"active", JsonValue::MakeString(node->Panels[Min(static_cast<usize>(node->Active), node->Panels.Size() - 1)]));
                return out;
            }
            out.Set(u8"split", JsonValue::MakeString(node->Axis == gui::DockAxis::Horizontal ? u8"horizontal" : u8"vertical"));
            JsonValue children = JsonValue::MakeArray();
            JsonValue weights = JsonValue::MakeArray();
            for (usize i = 0; i < node->Children.Size(); ++i)
            {
                children.Add(NodeJson(dock, node->Children[i]));
                weights.Add(JsonValue::MakeNumber(i < node->Weights.Size() ? node->Weights[i] : 0.0f));
            }
            out.Set(u8"children", Move(children));
            out.Set(u8"weights", Move(weights));
            return out;
        }

        void CollectPanels(const gui::DockController& dock, gui::DockNodeId id, JsonValue& into)
        {
            const gui::DockNode* node = dock.GetLayout().FindNode(id);
            if (node == nullptr)
                return;
            if (node->IsStack())
            {
                for (const String& panel : node->Panels)
                    into.Add(PanelJson(dock, panel.AsView()));
                return;
            }
            for (gui::DockNodeId child : node->Children)
                CollectPanels(dock, child, into);
        }

        JsonValue LayoutJson(const gui::DockController& dock)
        {
            const gui::DockLayout& layout = dock.GetLayout();
            JsonValue floating = JsonValue::MakeArray();
            for (const gui::DockSurface& surface : layout.Surfaces())
                if (surface.Kind == gui::DockSurfaceKind::Floating)
                    CollectPanels(dock, surface.Root, floating);
            // Hidden: closed with their place kept, and registered panels the layout never placed.
            JsonValue hidden = JsonValue::MakeArray();
            for (const gui::DockHidden& closed : layout.Hidden())
                hidden.Add(PanelJson(dock, closed.Panel.AsView()));
            for (const gui::DockPanelDescription& description : dock.Panels())
                if (layout.StackOf(description.Id.AsView()) == gui::kNoDock && layout.FindHidden(description.Id.AsView()) == nullptr)
                    hidden.Add(PanelJson(dock, description.Id.AsView()));
            JsonValue out = JsonValue::MakeObject();
            const gui::DockSurface* main = layout.FindSurface(layout.MainSurface());
            out.Set(u8"root", main != nullptr ? NodeJson(dock, main->Root) : JsonValue{});
            out.Set(u8"floating", Move(floating));
            out.Set(u8"hidden", Move(hidden));
            return out;
        }

        enum class Position : u8
        {
            Side,
            Center,
            Float,
        };
        struct Where
        {
            Position Kind = Position::Side;
            gui::DockSide Side = gui::DockSide::End;
        };
        Result<Where, String> ParsePosition(StringView text)
        {
            if (text == u8"left")
                return Where{Position::Side, gui::DockSide::Start};
            if (text == u8"right")
                return Where{Position::Side, gui::DockSide::End};
            if (text == u8"top")
                return Where{Position::Side, gui::DockSide::Top};
            if (text == u8"bottom")
                return Where{Position::Side, gui::DockSide::Bottom};
            if (text == u8"center")
                return Where{Position::Center, gui::DockSide::End};
            if (text == u8"float")
                return Where{Position::Float, gui::DockSide::End};
            return Err(Format(u8"unknown position '{}' (left, right, top, bottom, center or float)", text));
        }

        Array<String> PositionChoices()
        {
            static constexpr StringView kNames[] = {u8"left", u8"right", u8"top", u8"bottom", u8"center", u8"float"};
            Array<String> choices(editor::EditorRootAllocator());
            for (StringView name : kNames)
                choices.PushBack(String(name));
            return choices;
        }

        // A panel the tools may name: registered, or placed by the layout.
        [[nodiscard]] bool Known(const gui::DockController& dock, StringView id)
        {
            return !id.IsEmpty() && (dock.Describe(id) != nullptr || dock.GetLayout().StackOf(id) != gui::kNoDock);
        }

        // A float's size when the tool makes one: where a drag out of the window would put it.
        constexpr Rectangle kFloatBounds{120.0f, 120.0f, 640.0f, 420.0f};
    }

    void RegisterLayoutTools(foundation::mcp::McpServer& server, gui::DockController& dock)
    {
        gui::DockController* docks = &dock;

        server.RegisterTool(
            u8"layout_get",
            u8"The editor's dock layout: `root`, a tree whose nodes are splits ({split: horizontal "
            u8"(children left to right) or vertical (top to bottom), children: [nodes], weights: "
            u8"[each child's share, summing to one]}) or stacks of tabs ({tabs: [{panel, title}], "
            u8"active}), and the `floating` and `hidden` (closed) panels. A panel is named by its id: "
            u8"a page's asset guid, a Game tab's PIE id (game-page, game-page-1, ...), or a shared "
            u8"panel's (welcome is Project; assets, console, profiler). panel_dock moves them.",
            SchemaBuilder().Build(), ToolAnnotations::ReadOnly(),
            [docks](const JsonValue&) -> ToolResult { return LayoutJson(*docks); });

        server.RegisterTool(
            u8"panel_dock",
            u8"Move a panel, as dragging its tab does: to the `left`, `right`, `top` or `bottom` of "
            u8"another panel's stack (`relativeTo`; default: along the whole window's edge), into "
            u8"that stack as a tab (`center`; default: the documents' stack), or into a window of its "
            u8"own (`float`). A hidden panel docks again the same way. `share` (0.05 to 0.95, default "
            u8"0.5) is the moved panel's fraction of the room a side position splits. Two Game tabs "
            u8"side by side for a networked playtest: {panel: game-page-1, position: right, "
            u8"relativeTo: game-page}. Returns the layout as layout_get gives it. Ids come from "
            u8"layout_get.",
            SchemaBuilder()
                .Str(u8"panel", u8"the panel to move, by its id", true)
                .Enum(u8"position", PositionChoices(), u8"where it goes", true)
                .Str(u8"relativeTo", u8"the panel whose stack it goes beside or into (default: the window)")
                .Number(u8"share", u8"the moved panel's fraction of the room (side positions; default 0.5)")
                .Build(),
            ToolAnnotations::Adjusts(),
            [docks](const JsonValue& args) -> ToolResult
            {
                const String id = args.Get(u8"panel").AsString();
                if (!Known(*docks, id.AsView()))
                    return Err(Format(u8"no panel '{}' (layout_get lists them)", id.AsView()));
                Result<Where, String> where = ParsePosition(args.Get(u8"position").AsString().AsView());
                if (!where.HasValue())
                    return Err(Move(where.Error()));
                const bool side = where.Value().Kind == Position::Side;
                const bool hasShare = args.Get(u8"share").IsNumber();
                const f32 share = static_cast<f32>(args.Get(u8"share").AsNumber(0.5));
                if (hasShare && (!side || share < 0.05f || share > 0.95f))
                    return Err(String(u8"share is a side position's fraction, 0.05 to 0.95"));
                const gui::DockLayout& layout = docks->GetLayout();
                const String relativeId = args.Get(u8"relativeTo").AsString();
                gui::DockNodeId target = gui::kNoDock;
                if (!relativeId.IsEmpty())
                {
                    if (!Known(*docks, relativeId.AsView()))
                        return Err(Format(u8"no panel '{}' (layout_get lists them)", relativeId.AsView()));
                    if (relativeId == id)
                        return Err(String(u8"a panel cannot go beside or into itself"));
                    target = layout.StackOf(relativeId.AsView());
                    if (target == gui::kNoDock)
                        return Err(Format(u8"panel '{}' is not docked (it is hidden); dock it first", relativeId.AsView()));
                }
                const bool docked = layout.StackOf(id.AsView()) != gui::kNoDock;
                const StringView panel = id.AsView();
                const gui::DockSide sideOf = where.Value().Side;
                const Position kind = where.Value().Kind;
                const gui::DockOutcome done = docks->Edit(
                    [&](gui::DockLayout& edit) -> gui::DockOutcome
                    {
                        if (kind == Position::Float)
                            return edit.Float(panel, kFloatBounds);
                        if (kind == Position::Center)
                        {
                            const gui::DockNodeId stack = target != gui::kNoDock ? target : edit.CentralStack();
                            return docked ? edit.MoveToTab(panel, stack) : edit.InsertTab(panel, stack);
                        }
                        if (target != gui::kNoDock)
                            return docked ? edit.MoveBeside(panel, target, sideOf, share) : edit.InsertBeside(panel, target, sideOf, share);
                        return docked ? edit.MoveToEdge(panel, edit.MainSurface(), sideOf, share) : edit.InsertAtEdge(panel, edit.MainSurface(), sideOf, share);
                    });
                if (!done.Ok())
                    return Err(Format(u8"'{}' cannot go there: {}", panel, done.Refused));
                (void)docks->Edit([panel](gui::DockLayout& edit) { return edit.Activate(panel); });
                return LayoutJson(*docks);
            });
    }
}
