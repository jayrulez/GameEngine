// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The MCP panel over its markup (Data/Editor/Mcp.xml). Covers the bar following the host (listening
// with its client, stopped, a start that could not listen), Start, Stop and Copy Connect Command, the
// preference's check box; the calls newest first with the host's events between, the filter chips
// with their counts, the search; the chosen call's detail and Copy as JSON; Clear; each row's time
// and duration.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.json;
import foundation.mcp;
import editor.app;

using namespace foundation::core;
using namespace editor::app;
namespace gui = foundation::gui;
using foundation::json::JsonValue;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolCallPhase;
using foundation::mcp::ToolCallReport;

namespace
{
    RefPtr<gui::Node> LoadPanel()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(kMcpPanelPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    // A host the panel drives: whether it listens, its port and client, what was copied.
    struct Bed
    {
        McpActivity activity{DefaultAllocator()};
        bool listening = false;
        bool canListen = true;
        bool withProject = false;
        String client;
        String copied;
        String copiedWhat;
        int preferences = 0;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<McpPanel> panel;

        Bed()
        {
            McpPanelSeams seams;
            seams.activity = [this]() { return &activity; };
            seams.listening = [this]() { return listening; };
            seams.port = [this]() { return listening ? u16(7405) : u16(0); };
            seams.configuredPort = []() { return 7405u; };
            seams.client = [this]() { return client; };
            seams.start = [this]()
            {
                listening = canListen;
                return listening;
            };
            seams.stop = [this]() { listening = false; };
            seams.connectCommand = [this]() { return listening ? String(u8"claude mcp add engine-editor") : String(); };
            seams.startsWithProject = [this]() { return withProject; };
            seams.setStartsWithProject = [this](bool on) { withProject = on; };
            seams.copy = [this](StringView text, StringView what)
            {
                copied = String(text);
                copiedWhat = String(what);
            };
            seams.preferences = [this]() { ++preferences; };
            RefPtr<gui::Node> content = LoadPanel();
            REQUIRE(content);
            root->AddChild(content.Get());
            root->SetSize(Float2{1800.0f, 500.0f});
            panel = MakeUnique<McpPanel>(DefaultAllocator(), DefaultAllocator(), Move(seams), content);
            (void)root->UpdateLayout();
        }
        template <typename T>
        T* Find(StringView id) { return FindId<T>(*panel->Content(), id); }
        // Activates a button or check box, as a click does.
        void Click(StringView id)
        {
            auto* widget = Find<gui::UIWidget>(id);
            REQUIRE(widget != nullptr);
            CHECK(widget->HandleActivate());
        }
        [[nodiscard]] StringView Text(StringView id) { return Find<gui::Label>(id)->GetText(); }

        void Call(u64 serial, StringView tool, StringView arguments, ToolAnnotations annotations, bool isError, StringView result, i64 at,
                  f64 seconds = 0.002, ToolCallPhase phase = ToolCallPhase::Finished)
        {
            const JsonValue args = foundation::json::Parse(arguments).value;
            ToolCallReport report;
            report.serial = serial;
            report.phase = phase;
            report.tool = tool;
            report.arguments = &args;
            report.annotations = annotations;
            report.isError = isError;
            report.result = result;
            report.elapsed = Duration::FromSeconds(seconds);
            activity.Record(report, at);
        }
    };
}

TEST_CASE("mcp panel: the bar follows the host; Start, Stop and Copy Connect Command; a start that could not listen says why")
{
    Bed bed;
    CHECK(bed.Text(u8"mcp-state") == StringView(u8"Stopped"));
    CHECK_FALSE(bed.Find<gui::UIWidget>(u8"mcp-dot")->HasClass(u8"on"));
    CHECK(bed.Find<gui::Button>(u8"mcp-start")->IsDisplayed());
    CHECK_FALSE(bed.Find<gui::Button>(u8"mcp-stop")->IsDisplayed());
    CHECK_FALSE(bed.Find<gui::Button>(u8"mcp-copy-connect")->IsDisplayed());

    bed.Click(u8"mcp-start");
    CHECK(bed.listening);
    CHECK(bed.Text(u8"mcp-state") == StringView(u8"Listening on 127.0.0.1:7405"));
    CHECK(bed.Text(u8"mcp-client") == StringView(u8"no client yet"));
    CHECK(bed.Find<gui::UIWidget>(u8"mcp-dot")->HasClass(u8"on"));
    CHECK_FALSE(bed.Find<gui::Button>(u8"mcp-start")->IsDisplayed());
    CHECK(bed.Find<gui::Button>(u8"mcp-stop")->IsDisplayed());
    bed.client = String(u8"claude-code 2.1.0");
    bed.panel->Update();
    CHECK(bed.Text(u8"mcp-client") == StringView(u8"claude-code 2.1.0"));
    bed.Click(u8"mcp-copy-connect");
    CHECK(bed.copied == u8"claude mcp add engine-editor");
    CHECK(bed.copiedWhat == u8"the connect command");

    bed.Click(u8"mcp-stop");
    CHECK_FALSE(bed.listening);
    CHECK(bed.Text(u8"mcp-state") == StringView(u8"Stopped"));

    bed.canListen = false; // another editor holds the port
    bed.Click(u8"mcp-start");
    CHECK(bed.Text(u8"mcp-state") == StringView(u8"Could not start"));
    CHECK(bed.Text(u8"mcp-client").Contains(u8"127.0.0.1:7405 is taken"));

    // The check box is the preference; the gear opens the preferences.
    auto* withProject = bed.Find<gui::CheckBox>(u8"mcp-with-project");
    CHECK_FALSE(withProject->IsChecked());
    bed.Click(u8"mcp-with-project");
    CHECK(bed.withProject);
    bed.Click(u8"mcp-preferences");
    CHECK(bed.preferences == 1);
}

TEST_CASE("mcp panel: the calls newest first with the host's events, the filters with their counts, the search, Clear")
{
    Bed bed;
    CHECK(bed.panel->ShownCount() == 0u);
    CHECK(bed.Find<gui::Label>(u8"mcp-empty")->IsDisplayed());
    bed.activity.RecordEvent(u8"Listening on 127.0.0.1:7405", 100);
    bed.Call(1, u8"scene_read", u8"{\"path\":\"Content/Scenes/Level1.scene\"}", ToolAnnotations::ReadOnly(), false, u8"{}", 101);
    bed.Call(2, u8"component_set", u8"{\"entity\":\"Lamp 3\",\"value\":4.5}", ToolAnnotations::Overwrites(), false, u8"{\"after\":4.5}", 102);
    bed.Call(3, u8"script_validate", u8"{\"path\":\"Scripts/Player.luau\"}", ToolAnnotations::ReadOnly(), true, u8"'speed' is not a member", 103);
    bed.Call(4, u8"scene_write", u8"{}", ToolAnnotations::Overwrites(), true, u8"scene_write: missing required field 'source'", 104, 0.0,
             ToolCallPhase::Refused);
    bed.panel->Update();

    REQUIRE(bed.panel->ShownCount() == 5u);
    CHECK(bed.panel->ShownEntry(0)->tool == u8"scene_write"); // newest first
    CHECK(bed.panel->ShownEntry(1)->tool == u8"script_validate");
    CHECK(bed.panel->ShownEntry(4)->tool == u8"Listening on 127.0.0.1:7405");
    CHECK_FALSE(bed.Find<gui::Label>(u8"mcp-empty")->IsDisplayed());
    CHECK(bed.Find<gui::Button>(u8"mcp-all")->GetText() == StringView(u8"All  4")); // calls, not events
    CHECK(bed.Find<gui::Button>(u8"mcp-edits")->GetText() == StringView(u8"Edits  1")); // a refused write is no edit
    CHECK(bed.Find<gui::Button>(u8"mcp-failed")->GetText() == StringView(u8"Failed  2")); // a failure and a refusal

    bed.Click(u8"mcp-edits");
    CHECK(bed.panel->GetFilter() == McpPanel::Filter::Edits);
    REQUIRE(bed.panel->ShownCount() == 1u);
    CHECK(bed.panel->ShownEntry(0)->tool == u8"component_set");
    CHECK(bed.Find<gui::Button>(u8"mcp-edits")->HasClass(u8"checked"));
    CHECK_FALSE(bed.Find<gui::Button>(u8"mcp-all")->HasClass(u8"checked"));
    bed.Click(u8"mcp-failed");
    REQUIRE(bed.panel->ShownCount() == 2u);
    CHECK(bed.panel->ShownEntry(0)->tool == u8"scene_write");
    CHECK(bed.panel->ShownEntry(1)->tool == u8"script_validate");
    bed.panel->Select(0); // the refusal: its reason under Error
    CHECK(bed.Text(u8"mcp-detail-outcome") == StringView(u8"Refused"));
    CHECK(bed.Text(u8"mcp-result-title") == StringView(u8"Error"));
    CHECK(bed.Find<gui::TextArea>(u8"mcp-result")->GetText() == StringView(u8"scene_write: missing required field 'source'"));
    bed.Click(u8"mcp-all");

    // The search reads a call's tool, its summary and its arguments.
    bed.panel->SetSearch(u8"level1");
    REQUIRE(bed.panel->ShownCount() == 1u);
    CHECK(bed.panel->ShownEntry(0)->tool == u8"scene_read");
    bed.panel->SetSearch(u8"nothing like it");
    CHECK(bed.panel->ShownCount() == 0u);
    CHECK(bed.Text(u8"mcp-empty") == StringView(u8"No call matches."));
    bed.panel->SetSearch({});

    bed.Click(u8"mcp-clear");
    CHECK(bed.activity.Size() == 0u);
    CHECK(bed.panel->ShownCount() == 0u);
    CHECK(bed.Find<gui::Button>(u8"mcp-all")->GetText() == StringView(u8"All"));
}

TEST_CASE("mcp panel: the chosen call's arguments and answer, pretty; an error under Error; Copy as JSON; nothing chosen shows the placeholder")
{
    Bed bed;
    bed.Call(1, u8"component_set", u8"{\"entity\":\"Lamp 3\",\"value\":4.5}", ToolAnnotations::Overwrites(), false, u8"{\"after\":4.5}", 3600 * 12 + 4 * 60 + 27,
             0.002);
    bed.Call(2, u8"script_validate", u8"{}", ToolAnnotations::ReadOnly(), true, u8"'speed' is not a member", 3600 * 12 + 5 * 60, 1.25);
    bed.panel->Update();
    CHECK_FALSE(bed.Find<gui::UIWidget>(u8"mcp-detail")->IsDisplayed());
    CHECK(bed.Find<gui::UIWidget>(u8"mcp-no-detail")->IsDisplayed());
    CHECK(bed.panel->Selected() == nullptr);
    CHECK(bed.panel->SelectedCallJson().IsEmpty());

    bed.panel->Select(1); // the older: component_set
    REQUIRE(bed.panel->Selected() != nullptr);
    CHECK(bed.Find<gui::UIWidget>(u8"mcp-detail")->IsDisplayed());
    CHECK(bed.Text(u8"mcp-detail-tool") == StringView(u8"component_set"));
    CHECK(bed.Text(u8"mcp-detail-outcome") == StringView(u8"Done"));
    CHECK(bed.Text(u8"mcp-detail-when") == StringView(u8"12:04:27, 2 ms"));
    CHECK(bed.Text(u8"mcp-result-title") == StringView(u8"Result"));
    CHECK(bed.Find<gui::TextArea>(u8"mcp-arguments")->GetText() == JsonValue::Parse(String(u8"{\"entity\":\"Lamp 3\",\"value\":4.5}")).ToString(true).AsView());
    CHECK(bed.Find<gui::TextArea>(u8"mcp-arguments")->IsReadOnly());
    CHECK(bed.Find<gui::TextArea>(u8"mcp-result")->GetCaret() == 0u); // read from the top
    bed.Click(u8"mcp-detail-copy");
    CHECK(bed.copiedWhat == u8"the call");
    const JsonValue copied = JsonValue::Parse(bed.copied);
    CHECK(copied.Get(u8"tool").AsString() == u8"component_set");
    CHECK(copied.Get(u8"arguments").Get(u8"entity").AsString() == u8"Lamp 3");
    CHECK(copied.Get(u8"outcome").AsString() == u8"done");
    CHECK(copied.Get(u8"result").Get(u8"after").AsNumber() == doctest::Approx(4.5));
    CHECK(copied.Get(u8"milliseconds").AsNumber() == doctest::Approx(2.0));

    bed.panel->Select(0); // the failure: its error, as text
    CHECK(bed.Text(u8"mcp-detail-outcome") == StringView(u8"Failed"));
    CHECK(bed.Text(u8"mcp-detail-when") == StringView(u8"12:04:59, 1.3 s")); // it answered at 12:05:00 after 1.25 s
    CHECK(bed.Text(u8"mcp-result-title") == StringView(u8"Error"));
    CHECK(bed.Find<gui::TextArea>(u8"mcp-result")->GetText() == StringView(u8"'speed' is not a member"));
    bed.panel->CopySelectedCall();
    CHECK(JsonValue::Parse(bed.copied).Get(u8"error").AsString() == u8"'speed' is not a member");
}

TEST_CASE("mcp panel: a row's time and duration")
{
    CHECK(McpPanel::TimeText(3600 * 9 + 60 * 3 + 7) == u8"09:03:07");
    CHECK(McpPanel::TimeText(86400 * 3 + 3600 * 23 + 60 * 59 + 59) == u8"23:59:59");
    CHECK(McpPanel::TookText(Duration::FromMilliseconds(0.4)) == u8"0 ms");
    CHECK(McpPanel::TookText(Duration::FromMilliseconds(38.0)) == u8"38 ms");
    CHECK(McpPanel::TookText(Duration::FromSeconds(2.34)) == u8"2.3 s");
}
