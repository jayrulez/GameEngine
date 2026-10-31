// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Play - :pie_tools partition (implementation).
module;
#include "Core/Prelude.h"

module editor.play;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolOutcome;

namespace editor
{
    namespace
    {
        constexpr StringView kPieArgument = u8"the PIE instance's id, as pie_list reports it "
                                            u8"(default: the primary, `game-page`)";

        /// The entry an agent asks for (pie_resolution): by its key ("preset:HD"), by its label as
        /// the dropdown shows it, in any case ("hd 1280x720"), or by a size ("1280x720", the first
        /// entry of that size). Empty when none matches. It lives here rather than inline in
        /// :game_resolution, because GCC 15 then fails to read the partition's importers back
        /// ("Bad file data").
        [[nodiscard]] Optional<usize> MatchGameResolution(Span<const GameResolutionChoice> choices, StringView asked)
        {
            for (usize i = 0; i < choices.Size(); ++i)
            {
                if (choices[i].key.AsView() == asked)
                {
                    return i;
                }
            }
            for (usize i = 0; i < choices.Size(); ++i)
            {
                const StringView label = choices[i].label.AsView();
                if (label.Size() == asked.Size() && label.ContainsIgnoreCase(asked))
                {
                    return i;
                }
            }
            for (usize i = 0; i < choices.Size(); ++i)
            {
                if (choices[i].width > 0 && Format(u8"{}x{}", choices[i].width, choices[i].height).AsView() == asked)
                {
                    return i;
                }
            }
            return {};
        }

        /// Frames: five minutes at 60 Hz, including the cook Play waits on; then pie_start gives
        /// up.
        constexpr u32 kStartPumpLimit = 18000;
        /// Frames: ten seconds at 60 Hz, then pie_screenshot gives up.
        constexpr u32 kCapturePumpLimit = 600;

        /// The state of one pie_start call in flight: the tab it started and the pumps it has
        /// waited. Two identical starts (two agents each opening a new instance) each keep their
        /// own.
        struct StartWait final : foundation::mcp::ToolCallState
        {
            EditorPage* page = nullptr; // borrowed; checked against the open pages before use
            u32 pumps = 0;
        };

        /// The state of one pie_screenshot call in flight.
        struct CaptureWait final : foundation::mcp::ToolCallState
        {
            EditorPage* page = nullptr; // borrowed; checked against the open pages before use
            u32 pumps = 0;
            /// Where this call's capture writes. A tab holds one request at a time, so a call whose
            /// request was replaced by another asks again once the other is done.
            String path;
        };

        /// Per host, so two captures of one instance never share a default name.
        struct CaptureSerial
        {
            u32 value = 0;
        };

        IPieInstancePage* PieOf(EditorPage* page) noexcept
        {
            return ServiceOf<IPieInstancePage>(page);
        }

        bool IsOpen(const EditorContext& context, const EditorPage* page)
        {
            for (const UniquePtr<EditorPage>& open : context.OpenPages())
            {
                if (open.Get() == page)
                {
                    return true;
                }
            }
            return false;
        }

        /// The open Game page with this PIE id, or null.
        EditorPage* FindPie(const EditorContext& context, StringView id)
        {
            for (const UniquePtr<EditorPage>& open : context.OpenPages())
            {
                IPieInstancePage* pie = PieOf(open.Get());
                if (pie != nullptr && pie->PieId() == id)
                {
                    return open.Get();
                }
            }
            return nullptr;
        }

        ToolOutcome Start(EditorContext& context, foundation::mcp::ToolCall& call,
                          const JsonValue& args)
        {
            if (StartWait* wait = call.State<StartWait>())
            {
                // Re-entered: this call, one pump later.
                EditorPage* page = wait->page;
                ++wait->pumps;
                if (!IsOpen(context, page))
                {
                    return Err(String(u8"the Game tab closed while starting"));
                }
                IPieInstancePage* pie = PieOf(page);
                if (pie->IsRunning() && pie->FrameCount() > 0)
                {
                    JsonValue out = PieStateJson(*pie);
                    out.Set(u8"alreadyRunning", JsonValue::MakeBool(false));
                    return out;
                }
                if (!pie->IsRunning() && !pie->IsStarting())
                {
                    return Err(Format(u8"PIE instance '{}' did not start (log_read says why)",
                                      pie->PieId()));
                }
                // A tab behind another never renders its first frame, so bring it to the front,
                // unless the front tab belongs to another start still waiting for its own first
                // frame (they take turns rather than fight).
                if (pie->IsRunning() && context.ActivePage() != page)
                {
                    IPieInstancePage* front =
                        context.ActivePage() != nullptr ? PieOf(context.ActivePage()) : nullptr;
                    if (front == nullptr || !front->IsRunning() || front->FrameCount() > 0)
                    {
                        context.RevealPage(page);
                    }
                }
                if (wait->pumps > kStartPumpLimit)
                {
                    return Err(Format(u8"PIE instance '{}' rendered no frame in five minutes - a "
                                      u8"cook still running, or its tab hidden (an editor window "
                                      u8"minimised)?",
                                      pie->PieId()));
                }
                return ToolOutcome::NotFinished();
            }

            const bool newInstance = args.Get(u8"newInstance").AsBool();
            if (!newInstance)
            {
                if (EditorPage* primary = FindPie(context, kPrimaryPieId))
                {
                    IPieInstancePage* pie = PieOf(primary);
                    if (pie->IsRunning())
                    {
                        JsonValue out = PieStateJson(*pie);
                        out.Set(u8"alreadyRunning", JsonValue::MakeBool(true));
                        return out;
                    }
                    if (pie->IsStarting())
                    {
                        // Another call started it: this one waits for the same first frame.
                        auto wait = MakeUnique<StartWait>(context.Allocator());
                        wait->page = primary;
                        call.state = Move(wait);
                        return ToolOutcome::NotFinished();
                    }
                }
            }

            // The editor's own Play path opens (or reveals) the tab; the new one is the Game
            // page that was not open before.
            Array<EditorPage*> before;
            for (const UniquePtr<EditorPage>& open : context.OpenPages())
            {
                if (PieOf(open.Get()) != nullptr)
                {
                    before.PushBack(open.Get());
                }
            }
            const StringView actionId = newInstance ? StringView(u8"game.playNewInstance")
                                                    : StringView(u8"game.play");
            const Status executed = context.Actions().Execute(actionId);
            if (executed.Code() == ErrorCode::NotSupported)
            {
                return Err(Format(u8"the editor refused '{}': it plays only with a project open",
                                  actionId));
            }
            if (!executed.IsOk())
            {
                return Err(Format(u8"the editor has no '{}' action", actionId));
            }
            EditorPage* target = nullptr;
            for (const UniquePtr<EditorPage>& open : context.OpenPages())
            {
                IPieInstancePage* pie = PieOf(open.Get());
                if (pie == nullptr)
                {
                    continue;
                }
                bool wasOpen = false;
                for (EditorPage* old : before)
                {
                    wasOpen = wasOpen || old == open.Get();
                }
                if (newInstance ? !wasOpen : pie->PieId() == kPrimaryPieId)
                {
                    target = open.Get();
                    break;
                }
            }
            if (target == nullptr)
            {
                return Err(String(u8"no Game tab opened: this editor build has no play-in-editor"));
            }
            PieOf(target)->Play();
            auto wait = MakeUnique<StartWait>(context.Allocator());
            wait->page = target;
            call.state = Move(wait);
            return ToolOutcome::NotFinished();
        }

        ToolOutcome Screenshot(EditorContext& context, CaptureSerial& serial,
                               foundation::mcp::ToolCall& call, const JsonValue& args)
        {
            if (CaptureWait* wait = call.State<CaptureWait>())
            {
                // Re-entered: this call, one pump later.
                ++wait->pumps;
                if (!IsOpen(context, wait->page))
                {
                    return Err(String(u8"the Game tab closed before a frame was captured"));
                }
                IPieInstancePage* pie = PieOf(wait->page);
                const ViewportCapture& capture = pie->LastViewportCapture();
                const bool ours = capture.path == wait->path;
                if (ours && capture.state == ViewportCaptureState::Written)
                {
                    JsonValue out = JsonValue::MakeObject();
                    out.Set(u8"pie", JsonValue::MakeString(String(pie->PieId())));
                    out.Set(u8"path", JsonValue::MakeString(capture.path));
                    WriteCaptureSize(out, capture);
                    return out;
                }
                if (ours && capture.state == ViewportCaptureState::Failed)
                {
                    return Err(Format(u8"the capture of PIE instance '{}' failed (log_read, "
                                      u8"category Screenshot, says why)",
                                      pie->PieId()));
                }
                if (!pie->IsRunning())
                {
                    return Err(Format(u8"PIE instance '{}' stopped before a frame was captured",
                                      pie->PieId()));
                }
                if (wait->pumps > kCapturePumpLimit)
                {
                    return Err(Format(u8"PIE instance '{}' rendered no frame in ten seconds - is "
                                      u8"its tab visible (an editor window minimised or hidden)?",
                                      pie->PieId()));
                }
                // Another call's request replaced this one: once that one is done, ask again.
                if (!ours && capture.state != ViewportCaptureState::Pending)
                {
                    pie->RequestViewportCapture(wait->path.AsView());
                }
                return ToolOutcome::NotFinished();
            }
            Result<EditorPage*, String> resolved = ResolvePie(context, args);
            if (!resolved.HasValue())
            {
                return Err(Move(resolved.Error()));
            }
            EditorPage* page = resolved.Value();
            IPieInstancePage* pie = PieOf(page);
            if (!pie->IsRunning())
            {
                return Err(Format(u8"PIE instance '{}' is not running (pie_start runs it)",
                                  pie->PieId()));
            }
            String path = args.Get(u8"path").AsString();
            if (path.IsEmpty())
            {
                const String dir = PathJoin(GetUserDataDirectory().AsView(), u8"screenshots");
                if (!CreateDirectories(dir.AsView()))
                {
                    return Err(Format(u8"could not create '{}'", dir.AsView()));
                }
                ++serial.value;
                path = PathJoin(dir.AsView(), Format(u8"{}-{}-{}.png", pie->PieId(), ProcessId(),
                                                     serial.value)
                                                  .AsView());
            }
            context.RevealPage(page); // bring it to the front: a background tab's viewport never renders
            pie->RequestViewportCapture(path.AsView());
            auto wait = MakeUnique<CaptureWait>(context.Allocator());
            wait->page = page;
            wait->path = Move(path);
            call.state = Move(wait);
            return ToolOutcome::NotFinished();
        }
    }

    JsonValue PieStateJson(const IPieInstancePage& pie)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"pie", JsonValue::MakeString(String(pie.PieId())));
        out.Set(u8"running", JsonValue::MakeBool(pie.IsRunning()));
        out.Set(u8"starting", JsonValue::MakeBool(pie.IsStarting()));
        out.Set(u8"scene", JsonValue::MakeString(String(pie.SceneName())));
        out.Set(u8"runTime", JsonValue::MakeNumber(pie.RunTime()));
        out.Set(u8"frames", JsonValue::MakeNumber(static_cast<f64>(pie.FrameCount())));
        JsonValue script = JsonValue::MakeObject();
        switch (pie.ScriptState())
        {
        case PieScriptState::None:
            script.Set(u8"state", JsonValue::MakeString(u8"none"));
            break;
        case PieScriptState::Running:
            script.Set(u8"state", JsonValue::MakeString(u8"running"));
            break;
        case PieScriptState::Faulted:
            script.Set(u8"state", JsonValue::MakeString(u8"faulted"));
            script.Set(u8"fault", JsonValue::MakeString(String(pie.ScriptFault())));
            break;
        }
        out.Set(u8"script", Move(script));
        return out;
    }

    Result<EditorPage*, String> ResolvePie(const EditorContext& context, const JsonValue& args)
    {
        const String asked = args.Get(u8"pie").AsString();
        const StringView id = asked.IsEmpty() ? kPrimaryPieId : asked.AsView();
        if (EditorPage* page = FindPie(context, id))
        {
            return page;
        }
        if (id == kPrimaryPieId)
        {
            return Err(String(u8"no Game tab is open (pie_start opens and runs one)"));
        }
        return Err(Format(u8"no PIE instance '{}' (pie_list names them)", id));
    }

    void RegisterPieTools(foundation::mcp::McpServer& server, EditorContext& context)
    {
        EditorContext* ctx = &context;

        server.RegisterTool(
            u8"pie_start",
            u8"Start playing the project in the editor (PIE): the primary Game tab, or with "
            u8"`newInstance` another tab running an instance of its own - a host and a client, "
            u8"say. Cooks first, as Play does, loads the default scene and the startup script, and "
            u8"answers once the instance's first frame has rendered, with its state as pie_state "
            u8"gives it; `pie` is the id every other PIE tool takes. A primary already running is "
            u8"answered as it is, with `alreadyRunning`. A run that fails to start (a default "
            u8"scene that does not load) is an error; log_read says why.",
            SchemaBuilder()
                .Boolean(u8"newInstance", u8"open another Game tab with an instance of its own "
                                          u8"(Play New Instance) instead of the primary")
                .Build(),
            ToolAnnotations::Creates(),
            [ctx](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            { return Start(*ctx, call, args); });

        server.RegisterTool(
            u8"pie_stop",
            u8"Stop one PIE instance's run (or with `all`, every one's); its tab stays open, and "
            u8"the others keep running. Returns the ids stopped.",
            SchemaBuilder()
                .Str(u8"pie", kPieArgument)
                .Boolean(u8"all", u8"stop every instance")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolOutcome
            {
                JsonValue stopped = JsonValue::MakeArray();
                const auto stop = [&stopped](IPieInstancePage& pie)
                {
                    if (pie.IsRunning() || pie.IsStarting())
                    {
                        stopped.Add(JsonValue::MakeString(String(pie.PieId())));
                    }
                    pie.Stop();
                };
                if (args.Get(u8"all").AsBool())
                {
                    for (const UniquePtr<EditorPage>& open : ctx->OpenPages())
                    {
                        if (IPieInstancePage* pie = PieOf(open.Get()))
                        {
                            stop(*pie);
                        }
                    }
                }
                else
                {
                    Result<EditorPage*, String> resolved = ResolvePie(*ctx, args);
                    if (!resolved.HasValue())
                    {
                        return Err(Move(resolved.Error()));
                    }
                    stop(*PieOf(resolved.Value()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"stopped", Move(stopped));
                return out;
            });

        server.RegisterTool(
            u8"pie_state",
            u8"One PIE instance's state: whether it is running (or starting, waiting on the cook), "
            u8"the scene it is in, `runTime`, the seconds of frames since it started (unscaled: it "
            u8"keeps going while the game pauses its scene at time scale 0, as behind a menu), the "
            u8"frames rendered since, and its startup script's state (`none`, `running`, or "
            u8"`faulted` with the reason).",
            SchemaBuilder().Str(u8"pie", kPieArgument).Build(), ToolAnnotations::ReadOnly(),
            [ctx](const JsonValue& args) -> ToolOutcome
            {
                Result<EditorPage*, String> resolved = ResolvePie(*ctx, args);
                if (!resolved.HasValue())
                {
                    return Err(Move(resolved.Error()));
                }
                return PieStateJson(*PieOf(resolved.Value()));
            });

        server.RegisterTool(
            u8"pie_list",
            u8"Every open PIE instance, each with its state as pie_state gives it: what the user "
            u8"started as well as what an agent did.",
            SchemaBuilder().Build(), ToolAnnotations::ReadOnly(),
            [ctx](const JsonValue&) -> ToolOutcome
            {
                JsonValue items = JsonValue::MakeArray();
                for (const UniquePtr<EditorPage>& open : ctx->OpenPages())
                {
                    if (IPieInstancePage* pie = PieOf(open.Get()))
                    {
                        items.Add(PieStateJson(*pie));
                    }
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(items.Count())));
                out.Set(u8"instances", Move(items));
                return out;
            });

        server.RegisterTool(
            u8"pie_resolution",
            u8"The resolution one Game tab draws the game at, as its dropdown sets it, and its "
            u8"Phone toggle (a phone held upright). Without `resolution` or `phone`, reads them. "
            u8"`resolution` chooses an entry by its key (`project`, `export:<preset>`, "
            u8"`preset:<name>`, `panel`), its label as the dropdown shows it (any case), or a size "
            u8"(`1280x720`, the first entry of that size); `phone` sets the toggle. A choice is the "
            u8"dropdown's own: shown there, kept for the project (every Game tab opens at it), "
            u8"applied at once, running or not. Returns {pie, resolution, phone, choices}, each "
            u8"entry {key, label, width, height} (0 x 0: the panel's size). An entry that matches "
            u8"nothing is an error listing the choices.",
            SchemaBuilder()
                .Str(u8"pie", kPieArgument)
                .Str(u8"resolution", u8"the entry to choose: a key, a label or a size like 1280x720")
                .Boolean(u8"phone", u8"preview a phone held upright (the Phone toggle)")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolOutcome
            {
                Result<EditorPage*, String> resolved = ResolvePie(*ctx, args);
                if (!resolved.HasValue())
                {
                    return Err(Move(resolved.Error()));
                }
                IPieInstancePage& pie = *PieOf(resolved.Value());
                const auto entry = [](const GameResolutionChoice& choice)
                {
                    JsonValue out = JsonValue::MakeObject();
                    out.Set(u8"key", JsonValue::MakeString(choice.key));
                    out.Set(u8"label", JsonValue::MakeString(choice.label));
                    out.Set(u8"width", JsonValue::MakeNumber(static_cast<f64>(choice.width)));
                    out.Set(u8"height", JsonValue::MakeNumber(static_cast<f64>(choice.height)));
                    return out;
                };
                const auto listing = [&](Span<const GameResolutionChoice> choices)
                {
                    JsonValue items = JsonValue::MakeArray();
                    for (const GameResolutionChoice& choice : choices)
                    {
                        items.Add(entry(choice));
                    }
                    return items;
                };
                const String asked = args.Get(u8"resolution").AsString();
                if (!asked.IsEmpty())
                {
                    const Span<const GameResolutionChoice> choices = pie.ResolutionChoices();
                    const Optional<usize> match = MatchGameResolution(choices, asked.AsView());
                    if (!match.HasValue())
                    {
                        String known;
                        for (const GameResolutionChoice& choice : choices)
                        {
                            known += Format(u8"{}{} ({})", known.IsEmpty() ? u8"" : u8", ", choice.key.AsView(),
                                            choice.label.AsView())
                                         .AsView();
                        }
                        return Err(Format(u8"no resolution '{}' in {}'s dropdown; it offers: {}", asked.AsView(),
                                          pie.PieId(), known.AsView()));
                    }
                    (void)pie.SelectResolution(choices[match.Value()].key.AsView());
                }
                if (args.Get(u8"phone").IsBool())
                {
                    pie.SelectPhonePreview(args.Get(u8"phone").AsBool());
                }
                const Span<const GameResolutionChoice> choices = pie.ResolutionChoices();
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"pie", JsonValue::MakeString(String(pie.PieId())));
                out.Set(u8"resolution", entry(choices[FindGameResolution(choices, pie.ResolutionKey())]));
                out.Set(u8"phone", JsonValue::MakeBool(pie.PhonePreview()));
                out.Set(u8"choices", listing(choices));
                return out;
            });

        auto serial = MakeUnique<CaptureSerial>(context.Allocator());
        CaptureSerial* serialPtr = serial.Get();
        server.RegisterTool(
            u8"pie_screenshot",
            u8"What one running PIE instance's Game tab renders, as a PNG: the game through its own "
            u8"camera, with its UI and overlays, without the letterbox bars, as the pixels the tab "
            u8"drew. Brings the tab to front (a hidden viewport never renders), waits for the next "
            u8"frame and the GPU, then returns {pie, path, width, height}; read the file. When the "
            u8"tab is smaller than the game's render resolution it draws the game scaled down, and "
            u8"the answer adds renderWidth, renderHeight and scale (width / renderWidth): the PNG is "
            u8"that scaled image, so soft text there is the scale, and pie_run's mouse positions "
            u8"(render-resolution pixels) are its pixels divided by scale. `path` is where to write "
            u8"(an existing directory; default: <user-data>/screenshots/<pie>-<pid>-<n>.png). "
            u8"Refused for a stopped instance; gives up after ten seconds without a rendered frame.",
            SchemaBuilder()
                .Str(u8"pie", kPieArgument)
                .Str(u8"path", u8"the PNG to write (default: a new file under <user-data>/screenshots)")
                .Build(),
            ToolAnnotations::Creates(),
            [ctx, serialPtr, keep = Move(serial)](foundation::mcp::ToolCall& call,
                                                   const JsonValue& args) -> ToolOutcome
            { return Screenshot(*ctx, *serialPtr, call, args); });

        RegisterPieRunTool(server, context);
    }

    void WriteCaptureSize(foundation::json::JsonValue& out, const ViewportCapture& capture)
    {
        using foundation::json::JsonValue;
        out.Set(u8"width", JsonValue::MakeNumber(static_cast<f64>(capture.width)));
        out.Set(u8"height", JsonValue::MakeNumber(static_cast<f64>(capture.height)));
        if (capture.renderWidth > 0 && capture.renderHeight > 0)
        {
            out.Set(u8"renderWidth", JsonValue::MakeNumber(static_cast<f64>(capture.renderWidth)));
            out.Set(u8"renderHeight", JsonValue::MakeNumber(static_cast<f64>(capture.renderHeight)));
            out.Set(u8"scale", JsonValue::MakeNumber(static_cast<f64>(capture.width) /
                                                     static_cast<f64>(capture.renderWidth)));
        }
    }
}
