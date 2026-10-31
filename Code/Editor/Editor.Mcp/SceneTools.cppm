// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :scene_tools partition
//
// The scene and prefab MCP tools: scene_read, scene_write, scene_validate, prefab_read and
// prefab_write over the XML text sources. The files are the truth and the editor is not
// involved, so an agent can author scenes headlessly through the same "scene" data stream the
// editor saves.
//
// Validation is complete: the scratch Scene carries the full engine manager set (the
// Engine.Composition root, the same per-domain functions the subsystems inject through), so
// component payloads are validated field by field through their real managers, just as the
// editor and the cooked-scene reader parse them. A record whose type resolves to no manager is a
// genuinely unknown type; the reader skips it with a warning that is captured and reported.
// Writes validate first and refuse with the reasons; a passing write stores the scene as the
// editor saves it (scene::SceneSourceText over the validated scratch), so a generated file reads
// like a saved one.

module;
#include "Core/Prelude.h"

export module editor.mcp:scene_tools;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import foundation.xml.serialization;
import foundation.mcp;
import editor.project;
import engine.composition; // AddAllSceneManagers: the full manager set for the validate scratch
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;
namespace scene = foundation::scene;

namespace editor::mcp::detail
{
    // Captures Scene-category log lines emitted while parsing (the reader warns and skips
    // unknown component types instead of failing, which is what the agent needs to hear).
    class SceneLogCapture final : public ILogSink
    {
    public:
        Array<String> lines;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (category == StringView(u8"Scene") && level >= LogLevel::Warning)
            {
                lines.PushBack(String(message));
            }
        }
    };

    struct SceneParseReport
    {
        bool valid = false;
        String error;           // empty when valid
        Array<String> warnings; // skipped or unknown component records, and the like
        usize entityCount = 0;
        usize rootCount = 0;
        /// The prefab each prefab instance names, one per instance in stream order. Its entities
        /// are not in entityCount: they spawn from the prefab at load.
        Array<Guid> prefabIds;
        String sceneName;
    };

    // Parse `xml` as a scene stream into a scratch Scene carrying the full engine manager set
    // (Engine.Composition), so component payloads validate through their real managers. Only a
    // genuinely unknown component type is skipped (a warning, captured above). With `sourceText`,
    // a valid scene also comes back as the source text the editor would save for it (a scene with
    // its settings, a prefab without): what a write stores.
    inline SceneParseReport ParseSceneXml(StringView xml, String* sourceText = nullptr,
                                          bool includeSettings = true)
    {
        SceneParseReport report;
        SceneLogCapture capture;
        GlobalLogger().AddSink(&capture);

        MemoryStream stream;
        (void)stream.Write(xml.Data(), xml.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);

        scene::Scene scratch{editor::EditorRootAllocator()};
        engine::AddAllSceneManagers(scratch);
        Result<Array<byte>> transcoded =
            scene::TranscodeSceneStreamToBinary(stream, scratch, /*includeSettings=*/true);
        GlobalLogger().RemoveSink(&capture);

        report.warnings = Move(capture.lines);
        if (!transcoded.HasValue())
        {
            report.error = Format(u8"the scene stream did not parse (error {}) - fix the XML "
                                  u8"and validate again",
                                  static_cast<i32>(transcoded.Error()));
            return report;
        }
        if (sourceText != nullptr)
        {
            Result<String> text = scene::SceneSourceText(scratch, includeSettings);
            if (!text.HasValue())
            {
                report.error = Format(u8"the scene parsed but did not serialize (error {})",
                                      static_cast<i32>(text.Error()));
                return report;
            }
            *sourceText = Move(text.Value());
        }
        report.valid = true;
        report.sceneName = String(scratch.Name());
        report.entityCount = scratch.EntityCount();
        // The instances park unresolved: the parse has no database to spawn them from.
        scratch.ForEachPendingPrefabInstance([&report](scene::Scene::PendingPrefabInstance& pending)
                                             { report.prefabIds.PushBack(pending.prefabId); });
        for (scene::EntityHandle root = scratch.GetFirstRoot(); root.IsAssigned();
             root = scratch.GetNextSibling(root))
        {
            ++report.rootCount;
        }
        return report;
    }

    inline JsonValue ReportToJson(const SceneParseReport& report)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"valid", JsonValue::MakeBool(report.valid));
        if (!report.valid)
        {
            out.Set(u8"error", JsonValue::MakeString(report.error));
        }
        JsonValue warnings = JsonValue::MakeArray();
        for (const String& w : report.warnings)
        {
            warnings.Add(JsonValue::MakeString(w));
        }
        out.Set(u8"warnings", Move(warnings));
        if (report.valid)
        {
            out.Set(u8"sceneName", JsonValue::MakeString(report.sceneName));
            out.Set(u8"entityCount",
                    JsonValue::MakeNumber(static_cast<f64>(report.entityCount)));
            out.Set(u8"rootCount", JsonValue::MakeNumber(static_cast<f64>(report.rootCount)));
            out.Set(u8"prefabInstances",
                    JsonValue::MakeNumber(static_cast<f64>(report.prefabIds.Size())));
        }
        // Tells the caller what was checked: component payloads validate through the full engine
        // manager set (Engine.Composition), and warnings list any genuinely unknown component
        // types.
        out.Set(u8"componentValidation", JsonValue::MakeString(u8"full"));
        return out;
    }

    // A prefab instance whose prefab is not a prefab of the open project is skipped at load, its
    // entities never appearing: a warning, as an unknown component type is.
    inline void CheckPrefabs(editor::EditorProject* project, SceneParseReport& report)
    {
        if (project == nullptr)
        {
            return;
        }
        const StringView prefabType(
            reinterpret_cast<const utf8char*>(scene::PrefabDocument::StaticType().name));
        for (usize i = 0; i < report.prefabIds.Size(); ++i)
        {
            const Guid& id = report.prefabIds[i];
            content::Instance* instance = project->SourceDb().GetInstance(id);
            if (instance == nullptr || instance->TypeName() != prefabType)
            {
                utf8char text[37];
                id.ToChars(text);
                report.warnings.PushBack(Format(u8"prefab instance {}: {} is not a prefab in this "
                                                u8"project (asset_list: type {}) - it would be "
                                                u8"skipped at load",
                                                i, StringView(text, 36), prefabType));
            }
        }
    }

    // Resolve {guid} to an instance of the wanted document type; a wrong-type hit returns the
    // redirect message so the agent uses the sibling tool.
    inline content::Instance* ResolveDocument(editor::EditorProject* project, StringView guidText,
                                              StringView wantedType, StringView siblingTool,
                                              String& error)
    {
        if (project == nullptr)
        {
            error = String(u8"no project is open - call project_open first");
            return nullptr;
        }
        Guid id;
        if (!Guid::TryParse(guidText, id))
        {
            error = Format(u8"'{}' is not a valid guid", guidText);
            return nullptr;
        }
        content::Instance* instance = project->SourceDb().GetInstance(id);
        if (instance == nullptr)
        {
            error = Format(u8"no asset with guid {} in the open project", guidText);
            return nullptr;
        }
        if (instance->TypeName() != wantedType)
        {
            error = Format(u8"'{}' is a {}, not a {} - use {} instead", instance->Name(),
                           instance->TypeName(), wantedType, siblingTool);
            return nullptr;
        }
        return instance;
    }

    inline Result<String> ReadSceneStream(content::Instance& instance)
    {
        UniquePtr<IStream> stream = instance.ReadData(u8"scene");
        if (stream.Get() == nullptr)
        {
            return foundation::core::Err(ErrorCode::NotFound);
        }
        const i64 size = stream->Size();
        String text;
        if (size > 0)
        {
            Array<utf8char> buffer;
            buffer.Resize(static_cast<usize>(size));
            if (stream->Read(buffer.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
            {
                return foundation::core::Err(ErrorCode::Internal);
            }
            text = String(StringView(buffer.Data(), buffer.Size()));
        }
        return text;
    }
}

export namespace editor::mcp
{
    /// Registers scene_read, scene_write, scene_validate, prefab_read and prefab_write.
    inline void RegisterSceneTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        const auto makeRead = [s, &server](StringView tool, StringView wantedType,
                                           StringView sibling, StringView kind)
        {
            server.RegisterTool(
                String(tool),
                Format(u8"Read a {}'s XML source (the exact text the editor saves and the cook "
                       u8"consumes). Returns {{name, xml}}. Read this before editing; write "
                       u8"changes back with {}_write.",
                       kind, kind),
                SchemaBuilder().Str(u8"guid", Format(u8"the {} asset's guid", kind).AsView(), true)
                    .Build(),
                    foundation::mcp::ToolAnnotations::ReadOnly(),
                [s, wantedType = String(wantedType), sibling = String(sibling)](
                    const JsonValue& args) -> ToolResult
                {
                    String error;
                    content::Instance* instance = detail::ResolveDocument(
                        s->project, args.Get(u8"guid").AsString().AsView(),
                        wantedType.AsView(), sibling.AsView(), error);
                    if (instance == nullptr)
                    {
                        return Err(Move(error));
                    }
                    Result<String> xml = detail::ReadSceneStream(*instance);
                    if (!xml.HasValue())
                    {
                        return Err(Format(u8"'{}' has no scene stream (never saved?)",
                                          instance->Name()));
                    }
                    JsonValue out = JsonValue::MakeObject();
                    out.Set(u8"name", JsonValue::MakeString(String(instance->Name())));
                    out.Set(u8"xml", JsonValue::MakeString(xml.Value()));
                    return out;
                });
        };
        makeRead(u8"scene_read", u8"SceneDocument", u8"prefab_read", u8"scene");
        makeRead(u8"prefab_read", u8"PrefabDocument", u8"scene_read", u8"prefab");

        server.RegisterTool(
            u8"scene_validate",
            u8"Validate scene/prefab XML without writing anything - use this as the validation "
            u8"loop when authoring scenes. Pass `xml` (raw text) OR `guid` (validate the stored "
            u8"stream). Returns {valid, error?, warnings[], sceneName, entityCount, rootCount, "
            u8"prefabInstances}: entityCount is the scene's own entities; a prefab instance's "
            u8"spawn from its prefab at load, and one whose prefab is not in the project is a "
            u8"warning. "
            u8"Validation is FULL: the stream is parsed by the engine's own reader through every "
            u8"component manager and settings system, so framing, hierarchy and every component "
            u8"payload are checked; only a genuinely unknown component type surfaces as a warning "
            u8"(its records would be skipped on load). The format to author against is "
            u8"docs://generated/SceneSchema.json with docs://generated/SceneExample.scene.xml.",
            SchemaBuilder()
                .Str(u8"xml", u8"scene XML text to validate", false)
                .Str(u8"guid", u8"a scene/prefab asset guid whose stored stream to validate",
                     false)
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue& args) -> ToolResult
            {
                String xml = args.Get(u8"xml").AsString();
                const String guidText = args.Get(u8"guid").AsString();
                if (xml.IsEmpty() == guidText.IsEmpty())
                {
                    return Err(String(
                        u8"pass exactly one of `xml` (text to validate) or `guid` (stored "
                        u8"stream to validate)"));
                }
                if (!guidText.IsEmpty())
                {
                    if (s->project == nullptr)
                    {
                        return Err(String(u8"no project is open - call project_open first"));
                    }
                    Guid id;
                    if (!Guid::TryParse(guidText.AsView(), id))
                    {
                        return Err(Format(u8"'{}' is not a valid guid", guidText.AsView()));
                    }
                    content::Instance* instance = s->project->SourceDb().GetInstance(id);
                    if (instance == nullptr)
                    {
                        return Err(Format(u8"no asset with guid {} in the open project",
                                          guidText.AsView()));
                    }
                    Result<String> stored = detail::ReadSceneStream(*instance);
                    if (!stored.HasValue())
                    {
                        return Err(Format(u8"'{}' has no scene stream to validate",
                                          instance->Name()));
                    }
                    xml = stored.Value();
                }
                detail::SceneParseReport report = detail::ParseSceneXml(xml.AsView());
                detail::CheckPrefabs(s->project, report);
                return detail::ReportToJson(report);
            });

        const auto makeWrite = [s, &server](StringView tool, StringView wantedType,
                                            StringView sibling, StringView kind,
                                            bool singleRoot)
        {
            server.RegisterTool(
                String(tool),
                Format(u8"Write a {}'s XML source. VALIDATES FIRST and refuses with the reasons "
                       u8"on failure (nothing is written then). Pass `guid` to overwrite an "
                       u8"existing {} OR `name` (+ optional `group` path) to create a new one. "
                       u8"On success the scene is stored as the asset's source of truth exactly as the "
                       u8"editor saves it (its layout and number formatting), however it was passed.",
                       kind, kind),
                SchemaBuilder()
                    .Str(u8"xml", Format(u8"the {} XML text to store", kind).AsView(), true)
                    .Str(u8"guid", Format(u8"an existing {} asset to overwrite", kind).AsView(),
                         false)
                    .Str(u8"name", u8"name for a NEW asset (when no guid)", false)
                    .Str(u8"group", u8"slash-joined group path for a NEW asset (default root)",
                         false)
                    .Build(),
                    foundation::mcp::ToolAnnotations::Overwrites(),
                [s, wantedType = String(wantedType), sibling = String(sibling),
                 singleRoot](const JsonValue& args) -> ToolResult
                {
                    if (s->project == nullptr)
                    {
                        return Err(String(u8"no project is open - call project_open first"));
                    }
                    const String xml = args.Get(u8"xml").AsString();
                    if (xml.IsEmpty())
                    {
                        return Err(String(u8"`xml` is required and was empty"));
                    }
                    // What is stored is the scene as the editor saves it, however the caller
                    // laid it out or wrote its numbers (a script's one line, 1.0 for 1), so the
                    // file reads like one the editor saved and a later save rewrites nothing.
                    String stored;
                    const bool isScene = wantedType.AsView() == StringView(u8"SceneDocument");
                    detail::SceneParseReport report =
                        detail::ParseSceneXml(xml.AsView(), &stored, /*includeSettings=*/isScene);
                    detail::CheckPrefabs(s->project, report);
                    if (!report.valid)
                    {
                        JsonValue refusal = detail::ReportToJson(report);
                        return Err(Format(u8"refused - the XML did not validate: {}. Full "
                                          u8"report: {}",
                                          report.error.AsView(),
                                          refusal.ToString().AsView()));
                    }
                    if (singleRoot && report.rootCount != 1)
                    {
                        return Err(Format(
                            u8"refused - a prefab must have exactly ONE root entity (found "
                            u8"{}); re-parent the extra roots under one root first",
                            report.rootCount));
                    }

                    const String guidText = args.Get(u8"guid").AsString();
                    content::Instance* instance = nullptr;
                    if (!guidText.IsEmpty())
                    {
                        String error;
                        instance = detail::ResolveDocument(s->project, guidText.AsView(),
                                                           wantedType.AsView(), sibling.AsView(),
                                                           error);
                        if (instance == nullptr)
                        {
                            return Err(Move(error));
                        }
                    }
                    else
                    {
                        const String name = args.Get(u8"name").AsString();
                        if (name.IsEmpty())
                        {
                            return Err(String(
                                u8"pass `guid` (overwrite existing) or `name` (create new)"));
                        }
                        content::Group* group = detail::ResolveGroupPath(
                            s->project->SourceDb().RootGroup(),
                            args.Get(u8"group").AsString().AsView());
                        const TypeInfo* docType =
                            wantedType.AsView() == StringView(u8"SceneDocument")
                                ? &scene::SceneDocument::StaticType()
                                : &scene::PrefabDocument::StaticType();
                        instance = group->CreateInstance(name.AsView(), *docType);
                        if (instance == nullptr)
                        {
                            return Err(Format(u8"could not create '{}' (name already taken in "
                                              u8"that group?)",
                                              name.AsView()));
                        }
                    }

                    // Primary document (name discovery) and the XML stream.
                    if (wantedType.AsView() == StringView(u8"SceneDocument"))
                    {
                        scene::SceneDocument doc;
                        doc.name = report.sceneName.IsEmpty() ? String(instance->Name())
                                                              : report.sceneName;
                        if (!instance->WriteObject(doc).IsOk())
                        {
                            return Err(String(u8"failed writing the scene document envelope"));
                        }
                    }
                    else
                    {
                        scene::PrefabDocument doc;
                        doc.name = report.sceneName.IsEmpty() ? String(instance->Name())
                                                              : report.sceneName;
                        if (!instance->WriteObject(doc).IsOk())
                        {
                            return Err(String(u8"failed writing the prefab document envelope"));
                        }
                    }
                    const Status wrote = instance->WriteData(
                        u8"scene",
                        Span<const byte>{reinterpret_cast<const byte*>(stored.CStr()), stored.Size()},
                        content::StreamEncoding::Text);
                    if (!wrote.IsOk())
                    {
                        return Err(String(u8"failed writing the scene stream to disk"));
                    }
                    if (s->onAssetWritten)
                    {
                        s->onAssetWritten(instance->Id());
                    }

                    JsonValue out = detail::ReportToJson(report);
                    out.Set(u8"guid", detail::GuidToJson(instance->Id()));
                    out.Set(u8"name", JsonValue::MakeString(String(instance->Name())));
                    out.Set(u8"written", JsonValue::MakeBool(true));
                    return out;
                });
        };
        makeWrite(u8"scene_write", u8"SceneDocument", u8"prefab_write", u8"scene",
                  /*singleRoot=*/false);
        makeWrite(u8"prefab_write", u8"PrefabDocument", u8"scene_write", u8"prefab",
                  /*singleRoot=*/true);
    }
}
