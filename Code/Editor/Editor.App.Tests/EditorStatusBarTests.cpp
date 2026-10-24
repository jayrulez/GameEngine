// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorStatusBar over the editor's own frame (Data/Editor/Shell.gml): the message
// line; the job with its name, progress and count, a sweep when its length is unknown, hidden
// when none runs; the agent hidden while there is none; a frame without the items skipped.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
using editor::app::EditorStatusBar;

namespace
{
    RefPtr<gui::Node> LoadFrame()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), u8"Editor/Shell.gml");
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* Find(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = Find<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }
}

TEST_CASE("editor status bar: the message, a job with its progress and count or a sweep, the agent; an item with nothing to say hidden")
{
    RefPtr<gui::Node> frame = LoadFrame();
    REQUIRE(frame.Get() != nullptr);
    EditorStatusBar status(*frame);
    gui::Node* job = Find<gui::Node>(*frame, u8"status-job");
    gui::Node* agent = Find<gui::Node>(*frame, u8"status-agent");
    auto* progress = Find<gui::ProgressBar>(*frame, u8"status-job-progress");
    auto* count = Find<gui::Label>(*frame, u8"status-job-count");
    REQUIRE(job != nullptr);
    REQUIRE(agent != nullptr);
    REQUIRE(progress != nullptr);
    REQUIRE(count != nullptr);
    CHECK_FALSE(job->IsDisplayed()); // nothing running
    CHECK_FALSE(agent->IsDisplayed());

    status.SetMessage(u8"Saved 'Level 1'.");
    CHECK(status.Message() == StringView(u8"Saved 'Level 1'."));

    status.SetJob(u8"Export: Linux", 0.25f, u8"3 of 12");
    CHECK(status.ShowsJob());
    CHECK(Find<gui::Label>(*frame, u8"status-job-name")->GetText() == StringView(u8"Export: Linux"));
    CHECK_FALSE(progress->IsIndeterminate());
    CHECK(progress->GetValue() == doctest::Approx(0.25));
    CHECK(count->GetText() == StringView(u8"3 of 12"));
    CHECK(count->IsDisplayed());
    status.SetJob(u8"Cooking", -1.0f, {}); // its length unknown: a sweep, no count
    CHECK(progress->IsIndeterminate());
    CHECK_FALSE(count->IsDisplayed());
    status.ClearJob();
    CHECK_FALSE(status.ShowsJob());
    CHECK_FALSE(progress->IsIndeterminate());

    status.SetAgent(u8"MCP on 127.0.0.1:7405");
    CHECK(status.ShowsAgent());
    CHECK(Find<gui::Label>(*frame, u8"status-agent-text")->GetText() == StringView(u8"MCP on 127.0.0.1:7405"));
    status.SetAgent({});
    CHECK_FALSE(status.ShowsAgent());
}

TEST_CASE("editor status bar: a frame without its items is skipped, not a fault")
{
    RefPtr<gui::Panel> bare = MakeRef<gui::Panel>(DefaultAllocator());
    EditorStatusBar status(*bare);
    status.SetMessage(u8"nothing to show it on");
    status.SetJob(u8"Cooking", -1.0f, {});
    status.SetAgent(u8"MCP");
    CHECK(status.Message().IsEmpty());
    CHECK_FALSE(status.ShowsJob());
    CHECK_FALSE(status.ShowsAgent());
}
