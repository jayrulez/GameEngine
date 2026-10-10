// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - the window tools' bodies.
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
using foundation::mcp::ToolOutcome;

namespace editor::app
{
    namespace
    {
        constexpr u32 kCapturePumpLimit = 600; // frames: ten seconds at 60 Hz, then the tool gives up

        // The seams and the call in flight outlive the server through the handler's shared ref.
        class WindowToolState final : public RefCounted
        {
        public:
            explicit WindowToolState(WindowToolSeams seams) : seams(Move(seams)) {}
            WindowToolSeams seams;
            bool pending = false; // a capture requested; the tool is re-entered until it lands
            u32 pumps = 0;
        };

        Array<String> WindowChoices()
        {
            Array<String> choices(editor::EditorRootAllocator());
            choices.PushBack(String(u8"main"));
            choices.PushBack(String(u8"all"));
            return choices;
        }

        // The PNG for a window after the first: the first's path with "-window<id>" before
        // its extension.
        String SecondaryPath(StringView first, u32 window)
        {
            const StringView extension = u8".png";
            const StringView stem =
                first.EndsWith(extension) ? first.SubStr(0, first.Size() - extension.Size()) : first;
            return Format(u8"{}-window{}.png", stem, window);
        }

        JsonValue ShotJson(const WindowShot& shot)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"window", JsonValue::MakeNumber(static_cast<f64>(shot.window)));
            out.Set(u8"main", JsonValue::MakeBool(shot.main));
            out.Set(u8"path", JsonValue::MakeString(shot.path));
            out.Set(u8"width", JsonValue::MakeNumber(static_cast<f64>(shot.width)));
            out.Set(u8"height", JsonValue::MakeNumber(static_cast<f64>(shot.height)));
            return out;
        }
    }

    String NewScreenshotPath(StringView stem)
    {
        static u32 serial = 0; // main thread only: the tool's pump and the action both run there
        const String dir = PathJoin(GetUserDataDirectory().AsView(), u8"screenshots");
        if (!CreateDirectories(dir.AsView()))
        {
            return {};
        }
        return PathJoin(dir.AsView(), Format(u8"{}-{}-{}.png", stem, ProcessId(), ++serial).AsView());
    }

    void RegisterWindowTools(foundation::mcp::McpServer& server, WindowToolSeams seams)
    {
        RefPtr<WindowToolState> state = MakeRef<WindowToolState>(editor::EditorRootAllocator(), Move(seams));

        server.RegisterTool(
            u8"editor_screenshot",
            u8"The whole editor window as the user sees it, as a PNG: the panels, the pages, the "
            u8"menus, the dialogs and the toasts, where viewport_screenshot and pie_screenshot show "
            u8"one viewport. `window` \"main\" (the default) is the main window; \"all\" also writes "
            u8"each floating window, beside the first file with -window<id> before its extension. "
            u8"Waits for the next frame and the GPU, then returns {path, shots: [{window, main, "
            u8"path, width, height}]}; read the file. `path` is where to write the main window (its "
            u8"directory must exist; default: <user-data>/screenshots/editor-<pid>-<n>.png). Gives "
            u8"up after ten seconds without a frame (a minimised editor draws none).",
            SchemaBuilder()
                .Enum(u8"window", WindowChoices(), u8"\"main\" (default) or \"all\" its windows")
                .Str(u8"path", u8"the PNG to write (default: a new file under <user-data>/screenshots)")
                .Build(),
            ToolAnnotations::Creates(),
            [state](const JsonValue& args) -> ToolOutcome
            {
                if (state->pending)
                {
                    // Re-entered: the same call, one pump later.
                    ++state->pumps;
                    const Array<WindowShot> shots = state->seams.shots();
                    bool done = !shots.IsEmpty();
                    for (const WindowShot& shot : shots)
                    {
                        if (shot.state == WindowCaptureState::Failed)
                        {
                            state->pending = false;
                            return Err(Format(u8"the capture of window {} failed (log_read, category "
                                              u8"Screenshot, says why; a window closed before it drew "
                                              u8"fails too)",
                                              shot.window));
                        }
                        done = done && shot.state == WindowCaptureState::Written;
                    }
                    if (done)
                    {
                        state->pending = false;
                        JsonValue list = JsonValue::MakeArray();
                        for (const WindowShot& shot : shots)
                        {
                            list.Add(ShotJson(shot));
                        }
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"path", JsonValue::MakeString(shots[0].path));
                        out.Set(u8"shots", Move(list));
                        return out;
                    }
                    if (state->pumps > kCapturePumpLimit)
                    {
                        state->pending = false;
                        return Err(String(u8"the editor drew no frame in ten seconds - is its window "
                                          u8"minimised or hidden?"));
                    }
                    return ToolOutcome::NotFinished();
                }

                const Array<u32> windows = state->seams.windows();
                if (windows.IsEmpty())
                {
                    return Err(String(u8"the editor has no window that can draw a frame (minimised?)"));
                }
                const bool all = args.Get(u8"window").AsString().AsView() == StringView(u8"all");
                String path = args.Get(u8"path").AsString();
                if (path.IsEmpty())
                {
                    path = NewScreenshotPath(u8"editor");
                    if (path.IsEmpty())
                    {
                        return Err(String(u8"could not create <user-data>/screenshots"));
                    }
                }
                Array<WindowShotRequest> requests(editor::EditorRootAllocator());
                const usize count = all ? windows.Size() : 1;
                for (usize i = 0; i < count; ++i)
                {
                    WindowShotRequest request;
                    request.window = windows[i];
                    request.main = i == 0;
                    request.path = i == 0 ? path : SecondaryPath(path.AsView(), windows[i]);
                    requests.PushBack(Move(request));
                }
                state->seams.request(Span<const WindowShotRequest>(requests.Data(), requests.Size()));
                state->pending = true;
                state->pumps = 0;
                return ToolOutcome::NotFinished();
            });
    }
}
