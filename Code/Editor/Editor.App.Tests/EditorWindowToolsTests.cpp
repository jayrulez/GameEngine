// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - editor_screenshot over fake window seams: the main window captured to the
// given path or a new one under the screenshots folder, every window with "all" (each further one
// beside the first), the call re-entered until the captures land, and the failures it names (a
// failed capture, no window to capture, no frame in time).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    // What the application would be: its windows, and a capture the test moves along by hand.
    struct FakeWindows
    {
        Array<u32> windows{DefaultAllocator()};
        Array<app::WindowShot> shots{DefaultAllocator()};
        u32 requests = 0;

        app::WindowToolSeams Seams()
        {
            app::WindowToolSeams seams;
            seams.windows = [this]()
            {
                Array<u32> copy(DefaultAllocator());
                for (const u32 id : windows)
                {
                    copy.PushBack(id);
                }
                return copy;
            };
            seams.request = [this](Span<const app::WindowShotRequest> requested)
            {
                ++requests;
                shots.Clear();
                for (const app::WindowShotRequest& r : requested)
                {
                    app::WindowShot shot;
                    shot.window = r.window;
                    shot.main = r.main;
                    shot.path = r.path;
                    shots.PushBack(Move(shot));
                }
            };
            seams.shots = [this]()
            {
                Array<app::WindowShot> copy(DefaultAllocator());
                for (const app::WindowShot& shot : shots)
                {
                    copy.PushBack(shot);
                }
                return copy;
            };
            return seams;
        }

        void Land(app::WindowCaptureState state)
        {
            for (app::WindowShot& shot : shots)
            {
                shot.state = state;
                shot.width = 1600;
                shot.height = 900;
            }
        }
    };

    LineOutcome Pump(McpServer& server, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"editor_screenshot\",\"arguments\":{}}}}}",
                                   argumentsJson);
        return server.HandleLine(line.AsView());
    }

    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer AnswerOf(const LineOutcome& outcome)
    {
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
}

TEST_CASE("editor_screenshot: the main window to a PNG, waiting for its frame")
{
    FakeWindows fake;
    fake.windows.PushBack(7); // the main window
    fake.windows.PushBack(9); // a floating panel
    McpServer server;
    app::RegisterWindowTools(server, fake.Seams());
    CHECK(server.ToolCount() == app::kWindowToolCount);

    // The first pump asks for the main window alone, to the path given.
    CHECK(Pump(server, u8"{\"path\":\"/tmp/editor.png\"}").state == LineState::NotFinished);
    REQUIRE(fake.shots.Size() == 1);
    CHECK(fake.shots[0].window == 7u);
    CHECK(fake.shots[0].main);
    CHECK(fake.shots[0].path == StringView(u8"/tmp/editor.png"));

    // Re-entered until it lands; the same request, not a new one.
    CHECK(Pump(server, u8"{\"path\":\"/tmp/editor.png\"}").state == LineState::NotFinished);
    CHECK(fake.requests == 1u);
    fake.Land(app::WindowCaptureState::Written);
    const Answer got = AnswerOf(Pump(server, u8"{\"path\":\"/tmp/editor.png\"}"));
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"path").AsString() == StringView(u8"/tmp/editor.png"));
    const JsonValue shots = got.payload.Get(u8"shots");
    REQUIRE(shots.Count() == 1);
    CHECK(shots.At(0).Get(u8"window").AsNumber() == doctest::Approx(7));
    CHECK(shots.At(0).Get(u8"main").AsBool());
    CHECK(shots.At(0).Get(u8"width").AsNumber() == doctest::Approx(1600));
    CHECK(shots.At(0).Get(u8"height").AsNumber() == doctest::Approx(900));

    // With no path, a new file under the screenshots folder, a new one each time.
    CHECK(Pump(server, u8"{}").state == LineState::NotFinished);
    REQUIRE(fake.shots.Size() == 1);
    const String first = fake.shots[0].path;
    CHECK(first.AsView().EndsWith(u8".png"));
    CHECK(first.AsView().ContainsIgnoreCase(u8"screenshots"));
    CHECK(first.AsView().ContainsIgnoreCase(u8"editor-"));
    fake.Land(app::WindowCaptureState::Written);
    REQUIRE(AnswerOf(Pump(server, u8"{}")).ok);
    CHECK(Pump(server, u8"{}").state == LineState::NotFinished);
    CHECK(fake.shots[0].path != first);
    fake.Land(app::WindowCaptureState::Written);
    REQUIRE(AnswerOf(Pump(server, u8"{}")).ok);
}

TEST_CASE("editor_screenshot: every window with \"all\", each further one beside the first")
{
    FakeWindows fake;
    fake.windows.PushBack(7);
    fake.windows.PushBack(9);
    fake.windows.PushBack(12);
    McpServer server;
    app::RegisterWindowTools(server, fake.Seams());

    const StringView args = u8"{\"window\":\"all\",\"path\":\"/tmp/all.png\"}";
    CHECK(Pump(server, args).state == LineState::NotFinished);
    REQUIRE(fake.shots.Size() == 3);
    CHECK(fake.shots[0].path == StringView(u8"/tmp/all.png"));
    CHECK(fake.shots[0].main);
    CHECK(fake.shots[1].path == StringView(u8"/tmp/all-window9.png"));
    CHECK_FALSE(fake.shots[1].main);
    CHECK(fake.shots[2].path == StringView(u8"/tmp/all-window12.png"));

    // Not done while one window has yet to draw.
    fake.shots[0].state = app::WindowCaptureState::Written;
    CHECK(Pump(server, args).state == LineState::NotFinished);
    fake.Land(app::WindowCaptureState::Written);
    const Answer got = AnswerOf(Pump(server, args));
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"shots").Count() == 3);
    CHECK(got.payload.Get(u8"path").AsString() == StringView(u8"/tmp/all.png"));
}

TEST_CASE("editor_screenshot: says why when it cannot capture")
{
    FakeWindows fake;
    McpServer server;
    app::RegisterWindowTools(server, fake.Seams());

    // No window that can draw (the editor minimised).
    Answer got = AnswerOf(Pump(server, u8"{}"));
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"the editor has no window"));
    CHECK(fake.requests == 0u);

    // A failed capture names the window and the log.
    fake.windows.PushBack(7);
    CHECK(Pump(server, u8"{}").state == LineState::NotFinished);
    fake.Land(app::WindowCaptureState::Failed);
    got = AnswerOf(Pump(server, u8"{}"));
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"the capture of window 7 failed"));

    // No frame in time: it gives up rather than waiting forever.
    CHECK(Pump(server, u8"{}").state == LineState::NotFinished);
    LineOutcome outcome;
    for (u32 pump = 0; pump < 1000; ++pump)
    {
        outcome = Pump(server, u8"{}");
        if (outcome.state == LineState::Answered)
        {
            break;
        }
    }
    got = AnswerOf(outcome);
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"the editor drew no frame in ten seconds"));
}
