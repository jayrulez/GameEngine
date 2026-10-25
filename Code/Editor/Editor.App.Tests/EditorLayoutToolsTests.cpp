// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the layout tools over the editor's dock: layout_get's tree of splits (any
// number of children, with their weights) and stacks, panel_dock putting one Game tab beside
// another (the networked playtest's layout), a side's share, a tab back into a stack, a hidden
// panel docked again, and the refusals.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.gui;
import foundation.gui.docking;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
using namespace editor::app;
namespace json = foundation::json;
namespace gui = foundation::gui;
using json::JsonValue;

namespace
{
    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer Call(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        LineOutcome outcome = server.HandleLine(line.AsView());
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }

    bool HoldsTab(const JsonValue& group, StringView panel)
    {
        const JsonValue tabs = group.Get(u8"tabs");
        for (i64 i = 0; i < tabs.Count(); ++i)
        {
            if (tabs.At(i).Get(u8"panel").AsString() == panel)
            {
                return true;
            }
        }
        return false;
    }

    // The stack of a layout_get tree that holds `panel`, named by its tabs' ids ("a,b"); empty when
    // no stack holds it. Two panels share a stack when their names match.
    String GroupOf(const JsonValue& node, StringView panel)
    {
        if (node.Has(String(u8"tabs")))
        {
            if (!HoldsTab(node, panel))
                return String();
            String name;
            const JsonValue tabs = node.Get(u8"tabs");
            for (i64 i = 0; i < tabs.Count(); ++i)
                name += Format(u8"{}{}", i == 0 ? u8"" : u8",", tabs.At(i).Get(u8"panel").AsString().AsView()).AsView();
            return name;
        }
        if (node.Has(String(u8"split")))
        {
            const JsonValue children = node.Get(u8"children");
            for (i64 i = 0; i < children.Count(); ++i)
            {
                String found = GroupOf(children.At(i), panel);
                if (!found.IsEmpty())
                    return found;
            }
        }
        return String();
    }

    // Two neighbours in a split: the stack holding `first` right before the stack holding
    // `second`, their split's axis, and `second`'s share of the two.
    struct Pair
    {
        bool found = false;
        String axis;
        f64 secondShare = 0.0;
    };
    Pair PairOf(const JsonValue& node, StringView first, StringView second)
    {
        if (!node.Has(String(u8"split")))
            return {};
        const JsonValue children = node.Get(u8"children");
        const JsonValue weights = node.Get(u8"weights");
        for (i64 i = 0; i + 1 < children.Count(); ++i)
        {
            const JsonValue a = children.At(i);
            const JsonValue b = children.At(i + 1);
            if (a.Has(String(u8"tabs")) && b.Has(String(u8"tabs")) && HoldsTab(a, first) && HoldsTab(b, second))
            {
                const f64 wa = weights.At(i).AsNumber();
                const f64 wb = weights.At(i + 1).AsNumber();
                return Pair{true, node.Get(u8"split").AsString(), wb / (wa + wb)};
            }
        }
        for (i64 i = 0; i < children.Count(); ++i)
            if (Pair found = PairOf(children.At(i), first, second); found.found)
                return found;
        return {};
    }

    bool Lists(const JsonValue& panels, StringView id)
    {
        for (i64 i = 0; i < panels.Count(); ++i)
            if (panels.At(i).Get(u8"panel").AsString() == id)
                return true;
        return false;
    }

    // The editor's dock with two Game tabs among the documents, as Play and Play New Instance
    // leave them.
    struct LayoutRig
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Panel> into = MakeRef<gui::Panel>(DefaultAllocator());
        EditorDock dock{DefaultAllocator()};
        McpServer server;

        LayoutRig()
        {
            root->SetSize(Float2{1280.0f, 720.0f});
            root->AddChild(into.Get());
            dock.Open(*into, nullptr, EditorDock::Contents{});
            const char8_t* const games[] = {u8"game-page", u8"game-page-1"};
            for (const char8_t* id : games)
            {
                gui::DockPanelDescription description;
                description.Id = String(id);
                description.Title = String(u8"Game");
                description.Default.Central = true;
                description.Factory = []() { return RefPtr<gui::Node>(MakeRef<gui::Panel>(DefaultAllocator())); };
                REQUIRE(dock.Controller().Register(Move(description)));
                REQUIRE(dock.Controller().Show(StringView(id)).Ok());
            }
            RegisterLayoutTools(server, dock.Controller());
        }
    };
}

TEST_CASE("layout-tools: two Game tabs go side by side, then back into one stack")
{
    LayoutRig rig;
    CHECK(rig.server.ToolCount() == kLayoutToolCount);

    // Both tabs in the documents' stack, with the Project panel.
    Answer got = Call(rig.server, u8"layout_get", u8"{}");
    REQUIRE(got.ok);
    const JsonValue root = got.payload.Get(u8"root");
    REQUIRE_FALSE(GroupOf(root, u8"game-page").IsEmpty());
    CHECK(GroupOf(root, u8"game-page") == GroupOf(root, u8"game-page-1"));
    CHECK(GroupOf(root, u8"welcome") == GroupOf(root, u8"game-page"));
    CHECK_FALSE(GroupOf(root, u8"console").IsEmpty());

    // The client to the right of the host: across, the host's stack then the client alone, half
    // each.
    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-1\",\"position\":\"right\",\"relativeTo\":\"game-page\"}");
    REQUIRE(got.ok);
    {
        const JsonValue tree = got.payload.Get(u8"root");
        CHECK(GroupOf(tree, u8"game-page-1") == String(u8"game-page-1")); // alone in its stack
        CHECK(GroupOf(tree, u8"welcome") == GroupOf(tree, u8"game-page")); // the rest stay with the host
        const Pair pair = PairOf(tree, u8"game-page", u8"game-page-1");
        REQUIRE(pair.found);
        CHECK(pair.axis == String(u8"horizontal"));
        CHECK(pair.secondShare == doctest::Approx(0.5));
    }
    // The dock agrees: both are their stacks' active tabs, so both show.
    const gui::DockLayout& layout = rig.dock.Controller().GetLayout();
    const gui::DockNodeId host = layout.StackOf(u8"game-page");
    const gui::DockNodeId client = layout.StackOf(u8"game-page-1");
    CHECK(host != client);
    CHECK(layout.FindNode(client)->Panels[layout.FindNode(client)->Active] == String(u8"game-page-1"));

    // Back into the host's stack as a tab: the split goes.
    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-1\",\"position\":\"center\",\"relativeTo\":\"game-page\"}");
    REQUIRE(got.ok);
    CHECK(GroupOf(got.payload.Get(u8"root"), u8"game-page") == GroupOf(got.payload.Get(u8"root"), u8"game-page-1"));
    CHECK(rig.dock.Controller().GetLayout().StackOf(u8"game-page") == rig.dock.Controller().GetLayout().StackOf(u8"game-page-1"));
}

TEST_CASE("layout-tools: a side's share is the moved panel's, on either side")
{
    LayoutRig rig;
    // Below the host, three tenths of the room it had.
    Answer got = Call(rig.server, u8"panel_dock",
                      u8"{\"panel\":\"game-page-1\",\"position\":\"bottom\",\"relativeTo\":\"game-page\",\"share\":0.3}");
    REQUIRE(got.ok);
    Pair pair = PairOf(got.payload.Get(u8"root"), u8"game-page", u8"game-page-1");
    REQUIRE(pair.found);
    CHECK(pair.axis == String(u8"vertical"));
    CHECK(pair.secondShare == doctest::Approx(0.3));

    // Left of the host, a quarter: now the moved panel is first.
    got = Call(rig.server, u8"panel_dock",
               u8"{\"panel\":\"game-page-1\",\"position\":\"left\",\"relativeTo\":\"game-page\",\"share\":0.25}");
    REQUIRE(got.ok);
    pair = PairOf(got.payload.Get(u8"root"), u8"game-page-1", u8"game-page");
    REQUIRE(pair.found);
    CHECK(pair.secondShare == doctest::Approx(0.75));
}

TEST_CASE("layout-tools: a hidden panel is listed, and docks again; one floats")
{
    LayoutRig rig;
    REQUIRE(rig.dock.Controller().Close(u8"console").Ok()); // a shared panel keeps its place, hidden
    Answer got = Call(rig.server, u8"layout_get", u8"{}");
    REQUIRE(got.ok);
    CHECK(Lists(got.payload.Get(u8"hidden"), u8"console"));
    CHECK(GroupOf(got.payload.Get(u8"root"), u8"console").IsEmpty());

    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"console\",\"position\":\"center\",\"relativeTo\":\"assets\"}");
    REQUIRE(got.ok);
    CHECK_FALSE(Lists(got.payload.Get(u8"hidden"), u8"console"));
    CHECK(GroupOf(got.payload.Get(u8"root"), u8"console") == GroupOf(got.payload.Get(u8"root"), u8"assets"));

    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-1\",\"position\":\"float\"}");
    REQUIRE(got.ok);
    CHECK(Lists(got.payload.Get(u8"floating"), u8"game-page-1"));
    CHECK(GroupOf(got.payload.Get(u8"root"), u8"game-page-1").IsEmpty());
}

TEST_CASE("layout-tools: refusals name what is wrong")
{
    LayoutRig rig;
    Answer got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-9\",\"position\":\"right\"}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().ContainsIgnoreCase(u8"layout_get"));

    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page\",\"position\":\"right\",\"relativeTo\":\"game-page\"}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().ContainsIgnoreCase(u8"itself"));

    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-1\",\"position\":\"center\",\"relativeTo\":\"game-page\",\"share\":0.3}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().ContainsIgnoreCase(u8"share"));

    REQUIRE(rig.dock.Controller().Close(u8"console").Ok());
    got = Call(rig.server, u8"panel_dock", u8"{\"panel\":\"game-page-1\",\"position\":\"right\",\"relativeTo\":\"console\"}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().ContainsIgnoreCase(u8"not docked"));

    // Nothing moved.
    got = Call(rig.server, u8"layout_get", u8"{}");
    CHECK(GroupOf(got.payload.Get(u8"root"), u8"game-page") == GroupOf(got.payload.Get(u8"root"), u8"game-page-1"));
}
