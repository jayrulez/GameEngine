// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the input tools over a GUI root of their own: what a target finds, a click
// reaching what it names (or refused: none, several, covered), a point clicked, text typed into
// the focus and a key pressed, each answering a frame (a pump) after it acts.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace json = foundation::json;
namespace gui = foundation::gui;
using json::JsonValue;

namespace
{
    template <typename T>
    RefPtr<T> Add(gui::Node& parent, StringView id, f32 x, f32 y, f32 w, f32 h)
    {
        auto node = MakeRef<T>(DefaultAllocator());
        node->SetId(id);
        node->SetPosition(Float2{x, y});
        node->SetSize(Float2{w, h});
        parent.AddChild(node.Get());
        return node;
    }

    // A form: "Show" and "OK" buttons, a field, and a second "OK" in a panel.
    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Button> show;
        RefPtr<gui::Button> ok;
        RefPtr<gui::TextField> field;
        RefPtr<gui::Panel> dialog;
        RefPtr<gui::Button> dialogOk;
        int shows = 0;
        int oks = 0;
        McpServer server;

        Bed()
        {
            root->SetSize(Float2{400.0f, 300.0f});
            show = Add<gui::Button>(*root.Get(), u8"show", 10.0f, 10.0f, 80.0f, 30.0f);
            show->SetText(u8"Show");
            show->SetOnClick([this]() { ++shows; });
            ok = Add<gui::Button>(*root.Get(), u8"ok", 10.0f, 90.0f, 80.0f, 30.0f);
            ok->SetText(u8"OK");
            field = Add<gui::TextField>(*root.Get(), u8"field", 10.0f, 50.0f, 150.0f, 30.0f);
            dialog = Add<gui::Panel>(*root.Get(), u8"dialog", 200.0f, 10.0f, 180.0f, 150.0f);
            dialogOk = Add<gui::Button>(*dialog.Get(), u8"dialog-ok", 10.0f, 10.0f, 80.0f, 30.0f);
            dialogOk->SetText(u8"OK");
            dialogOk->SetOnClick([this]() { ++oks; });
            root->UpdateLayout();
            app::InputToolSeams seams;
            gui::SceneRoot* over = root.Get();
            seams.root = [over]() { return over; };
            app::RegisterInputTools(server, Move(seams));
        }
    };

    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
        u32 pumps = 0; // entries before it answered
    };
    // The call, pumped as the host would until it answers.
    Answer Call(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        Answer answer;
        LineOutcome outcome = server.HandleLine(line.AsView());
        while (outcome.state == LineState::NotFinished && answer.pumps < 5)
        {
            ++answer.pumps;
            outcome = server.HandleLine(line.AsView());
        }
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
            answer.payload = json::Parse(text.AsView()).value;
        else
            answer.error = text;
        return answer;
    }
}

TEST_CASE("editor input tools: find lists what a target names, with where it is")
{
    Bed bed;
    CHECK(bed.server.ToolCount() == app::kInputToolCount);
    const Answer found = Call(bed.server, u8"editor_find", u8"{\"target\":\"text=OK\"}");
    REQUIRE(found.ok);
    CHECK(found.pumps == 0u); // a read answers at once
    CHECK(found.payload.Get(u8"count").AsInt() == 2);
    const Answer within = Call(bed.server, u8"editor_find", u8"{\"target\":\"text=OK\",\"within\":\"#dialog\"}");
    REQUIRE(within.ok);
    REQUIRE(within.payload.Get(u8"count").AsInt() == 1);
    const JsonValue node = within.payload.Get(u8"nodes").At(0);
    CHECK(node.Get(u8"node").AsString().AsView().ContainsIgnoreCase(u8"#dialog-ok"));
    CHECK(node.Get(u8"text").AsString() == StringView(u8"OK"));
    CHECK(node.Get(u8"x").AsNumber() == doctest::Approx(210.0));
    CHECK(node.Get(u8"width").AsNumber() == doctest::Approx(80.0));
}

TEST_CASE("editor input tools: a click reaches what it names a frame later, and is refused when it cannot")
{
    Bed bed;
    const Answer clicked = Call(bed.server, u8"editor_click", u8"{\"target\":\"text=Show\"}");
    REQUIRE(clicked.ok);
    CHECK(clicked.pumps == 1u); // answered on the frame after
    CHECK(bed.shows == 1);
    CHECK(clicked.payload.Get(u8"clicked").Get(u8"node").AsString().AsView().ContainsIgnoreCase(u8"#show"));
    CHECK(clicked.payload.Get(u8"at").Get(u8"x").AsNumber() == doctest::Approx(50.0));

    // Several: refused, each named; `index` or `within` picks one.
    const Answer several = Call(bed.server, u8"editor_click", u8"{\"target\":\"text=OK\"}");
    CHECK_FALSE(several.ok);
    CHECK(several.error.AsView().ContainsIgnoreCase(u8"2 shown node(s) match"));
    CHECK(several.error.AsView().ContainsIgnoreCase(u8"#dialog-ok"));
    REQUIRE(Call(bed.server, u8"editor_click", u8"{\"target\":\"text=OK\",\"index\":1}").ok);
    CHECK(bed.oks == 1);
    CHECK_FALSE(Call(bed.server, u8"editor_click", u8"{\"target\":\"#nothing\"}").ok);

    // Covered: refused, naming what lies over it; nothing is clicked.
    auto cover = Add<gui::Panel>(*bed.root.Get(), u8"cover", 0.0f, 0.0f, 120.0f, 45.0f);
    bed.root->UpdateLayout();
    const Answer covered = Call(bed.server, u8"editor_click", u8"{\"target\":\"#show\"}");
    CHECK_FALSE(covered.ok);
    CHECK(covered.error.AsView().ContainsIgnoreCase(u8"covered at its middle by Panel #cover"));
    CHECK(bed.shows == 1);
    bed.root->RemoveChild(cover.Get());
    bed.root->UpdateLayout();

    // A point, as the screenshot shows it.
    REQUIRE(Call(bed.server, u8"editor_click", u8"{\"x\":30,\"y\":20}").ok);
    CHECK(bed.shows == 2);
    CHECK_FALSE(Call(bed.server, u8"editor_click", u8"{\"x\":30}").ok);
    CHECK_FALSE(Call(bed.server, u8"editor_click", u8"{\"target\":\"#show\",\"modifiers\":\"hyper\"}").ok);
}

TEST_CASE("editor input tools: text goes to the focus, a key acts on it, and the answer says what has it")
{
    Bed bed;
    const Answer unfocused = Call(bed.server, u8"editor_type", u8"{\"text\":\"lost\"}");
    CHECK_FALSE(unfocused.ok);
    CHECK(unfocused.error.AsView().ContainsIgnoreCase(u8"nothing has the focus"));

    const Answer clicked = Call(bed.server, u8"editor_click", u8"{\"target\":\"#field\"}");
    REQUIRE(clicked.ok);
    CHECK(clicked.payload.Get(u8"focus").Get(u8"node").AsString().AsView().ContainsIgnoreCase(u8"#field"));
    REQUIRE(Call(bed.server, u8"editor_type", u8"{\"text\":\"hello\"}").ok);
    CHECK(bed.field->GetText() == StringView(u8"hello"));
    const Answer key = Call(bed.server, u8"editor_key", u8"{\"chord\":\"backspace\"}");
    REQUIRE(key.ok);
    CHECK(bed.field->GetText() == StringView(u8"hell"));
    CHECK_FALSE(Call(bed.server, u8"editor_key", u8"{\"chord\":\"ctrl-nokey\"}").ok);

    // Hover and scroll answer with what the pointer is over.
    const Answer hover = Call(bed.server, u8"editor_hover", u8"{\"target\":\"text=Show\"}");
    REQUIRE(hover.ok);
    CHECK(hover.payload.Get(u8"over").Get(u8"node").AsString().AsView().ContainsIgnoreCase(u8"#show"));
    REQUIRE(Call(bed.server, u8"editor_scroll", u8"{\"target\":\"#dialog\",\"dy\":-1}").ok);
}
