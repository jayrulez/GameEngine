// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the MCP action bridge over a real registry: action_list with every
// declaration's state over the active page, action_state for one, action_execute through the
// funnel (a toggle flips, its state comes back), the refusals (unknown id, disabled over the
// active page), and the unattended scope: an action that opens a dialog runs with the dialog
// suppressed and reported.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.json;
import foundation.mcp;
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
    class TogglePage final : public EditorPage
    {
    public:
        TogglePage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"toggling"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        bool on = false;
    };

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
    const JsonValue* FindById(const JsonValue& actions, StringView id, JsonValue& storage)
    {
        for (usize i = 0; i < static_cast<usize>(actions.Count()); ++i)
        {
            if (actions.At(i).Get(u8"id").AsString() == id)
            {
                storage = actions.At(i);
                return &storage;
            }
        }
        return nullptr;
    }
}

TEST_CASE("action-tools: list and state over the active page, execute through the funnel with "
          "its refusals, and the unattended scope reporting a suppressed dialog")
{
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{800.0f, 600.0f});

    EditorContext context{DefaultAllocator()};
    EditorActionRegistry& actions = context.Actions();
    u32 exits = 0;
    {
        EditorActionDeclaration d;
        d.id = String(u8"file.exit");
        d.label = String(u8"Exit");
        d.description = String(u8"Exit the editor");
        d.menuPath = String(u8"File/Exit");
        d.execute = [&exits](EditorPage*) { ++exits; };
        REQUIRE(actions.Register(Move(d)));
    }
    {
        EditorActionDeclaration d;
        d.id = String(u8"page.flip");
        d.label = String(u8"Flip");
        d.kind = EditorActionKind::Toggle;
        d.shortcut = EditorShortcut::Of(u8"ctrl-f");
        d.enabled = [](EditorPage* page) { return page != nullptr; };
        d.disabledReason = [](EditorPage*) { return String(u8"Open a page to flip."); };
        d.checked = [](EditorPage* page)
        { return page != nullptr && static_cast<TogglePage*>(page)->on; }; // asked disabled too
        d.execute = [](EditorPage* page)
        {
            auto* toggling = static_cast<TogglePage*>(page);
            toggling->on = !toggling->on;
        };
        REQUIRE(actions.Register(Move(d)));
    }
    gui::SceneRoot* over = root.Get();
    bool asked = false;
    {
        EditorActionDeclaration d;
        d.id = String(u8"project.close");
        d.label = String(u8"Close Project");
        d.readOnly = false;
        d.execute = [over, &asked](EditorPage*)
        {
            // The flow asks before it acts; dismissed, it acts not.
            auto box = MakeRef<gui::MessageBox>(DefaultAllocator());
            box->Configure(u8"Unsaved changes", u8"Close anyway?", gui::MessageBox::Buttons::OkCancel);
            box->SetOnResult([&asked](gui::MessageBox::Result result) { asked = result == gui::MessageBox::Result::Ok; });
            box->OpenModal(*over);
        };
        REQUIRE(actions.Register(Move(d)));
    }

    McpServer server;
    app::ActionToolSeams seams;
    seams.context = &context;
    seams.dialogs = root->GetEventDispatcher();
    app::RegisterActionTools(server, Move(seams));
    CHECK(server.ToolCount() == app::kActionToolCount);

    // The list, with no page active: the page-bound toggle is disabled.
    Answer listed = Call(server, u8"action_list", u8"{}");
    REQUIRE(listed.ok);
    CHECK(listed.payload.Get(u8"count").AsInt() == 3);
    JsonValue found;
    REQUIRE(FindById(listed.payload.Get(u8"actions"), u8"page.flip", found) != nullptr);
    CHECK(found.Get(u8"kind").AsString() == StringView(u8"toggle"));
    CHECK(found.Get(u8"shortcut").AsString() == StringView(u8"Ctrl+F"));
    CHECK_FALSE(found.Get(u8"enabled").AsBool());
    CHECK(found.Get(u8"disabledReason").AsString() == StringView(u8"Open a page to flip.")); // said where it has one
    CHECK_FALSE(found.Get(u8"checked").AsBool());
    REQUIRE(FindById(listed.payload.Get(u8"actions"), u8"file.exit", found) != nullptr);
    CHECK(found.Get(u8"menuPath").AsString() == StringView(u8"File/Exit"));
    CHECK(found.Get(u8"enabled").AsBool());

    // Execute: the editor-wide action runs; the disabled one is refused with the reason.
    Answer ran = Call(server, u8"action_execute", u8"{\"id\":\"file.exit\"}");
    REQUIRE(ran.ok);
    CHECK(ran.payload.Get(u8"executed").AsBool());
    CHECK(ran.payload.Get(u8"suppressedDialogs").Count() == 0);
    CHECK(exits == 1u);
    Answer refused = Call(server, u8"action_execute", u8"{\"id\":\"page.flip\"}");
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == String(u8"action 'page.flip' is not enabled over the active page (no page is active): Open a page to flip."));
    Answer unknown = Call(server, u8"action_execute", u8"{\"id\":\"nobody.home\"}");
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error.AsView().StartsWith(u8"no action 'nobody.home'"));
    CHECK_FALSE(Call(server, u8"action_state", u8"{\"id\":\"nobody.home\"}").ok);

    // A page active: the toggle flips and reports its state after.
    auto* page = static_cast<TogglePage*>(context.AdoptPage(
        UniquePtr<EditorPage>(DefaultAllocator().New<TogglePage>(), DefaultAllocator())));
    Answer state = Call(server, u8"action_state", u8"{\"id\":\"page.flip\"}");
    REQUIRE(state.ok);
    CHECK(state.payload.Get(u8"enabled").AsBool());
    CHECK_FALSE(state.payload.Get(u8"checked").AsBool());
    Answer flipped = Call(server, u8"action_execute", u8"{\"id\":\"page.flip\"}");
    REQUIRE(flipped.ok);
    CHECK(page->on);
    CHECK(flipped.payload.Get(u8"checked").AsBool());
    CHECK(flipped.payload.Get(u8"enabled").AsBool());

    // Unattended: the dialog the action opens is suppressed and named; the flow saw a
    // dismissal, so nothing happened.
    Answer closed = Call(server, u8"action_execute", u8"{\"id\":\"project.close\"}");
    REQUIRE(closed.ok);
    REQUIRE(closed.payload.Get(u8"suppressedDialogs").Count() == 1);
    CHECK(closed.payload.Get(u8"suppressedDialogs").At(0).AsString() == StringView(u8"Unsaved changes"));
    CHECK(closed.payload.Has(u8"note"));
    CHECK_FALSE(asked);
    CHECK(root->GetEventDispatcher()->PopupCount() == 0u);           // never shown
    CHECK_FALSE(static_cast<bool>(root->GetEventDispatcher()->TakeModalInterceptor())); // the scope restored it

    context.ClosePage(page);
}
