// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - `editor.mcp`
//
// The project MCP tool contribution over foundation.mcp, built on the headless EditorProject (VFS +
// manifest; no UI): project_info + the asset tools here, the scene / script / health / export /
// log tools in the partitions, and at the end RegisterEngineTools - the ONE list of the engine
// surface every host serves (the stdio host and the editor host compose through it, so they
// cannot drift; kEngineToolCount is its tripwire). project_create / project_open are the stdio
// host's own additions (RegisterProjectOpenTools): the editor's project is the editor's. A
// ProjectSession points every tool at the current project without owning it.

module;
#include "Core/Prelude.h"

export module editor.mcp;
export import :session;
export import :operations;
export import :asset_create;
export import :asset_data;
export import :project_settings;
export import :reflected_fields;
export import :export_presets;
export import :asset_delete;
export import :scene_tools;
export import :asset_uses;
export import :project_health;
export import :log_tools;
export import :resources;
export import :script_validate;
export import :script_create;
export import :project_export;
export import :scene_reference;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import foundation.mcp.reflection;
import foundation.mcp.script;
import pipeline.core;
import pipeline.importer;
import pipeline.cook;
import editor.project;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;
namespace vfs = foundation::vfs;

namespace editor::mcp::detail
{
    // The two content databases a project exposes, as an enum choice list.
    inline Array<String> DatabaseChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"source"));
        choices.PushBack(String(u8"cooked"));
        return choices;
    }

    // Recursively append every instance under `group` (depth-first) to `out` as {guid,name,type,
    // typeNamespace,group}. `path` is the slash-joined group path ("" at the root).
    inline void CollectAssets(content::Group* group, const String& path, JsonValue& out)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* inst : group->Instances())
        {
            JsonValue e = JsonValue::MakeObject();
            e.Set(u8"guid", GuidToJson(inst->Id()));
            e.Set(u8"name", JsonValue::MakeString(String(inst->Name())));
            e.Set(u8"type", JsonValue::MakeString(String(inst->TypeName())));
            e.Set(u8"typeNamespace", JsonValue::MakeString(String(inst->TypeNamespace())));
            if (!path.IsEmpty())
            {
                e.Set(u8"group", JsonValue::MakeString(path));
            }
            out.Add(Move(e));
        }
        for (content::Group* sub : group->Groups())
        {
            const String childPath =
                path.IsEmpty() ? String(sub->Name()) : Format(u8"{}/{}", path.AsView(), sub->Name());
            CollectAssets(sub, childPath, out);
        }
    }
}

export namespace editor::mcp
{
    // Registers project_create / project_open / project_info against `server`, backed by `session`.
    // Registers project_create / project_open - the STDIO host's additions. What project_open
    // opens is stored in `owner` and the session is pointed at it; the editor host, whose project
    // is the editor's own, registers neither.
    // `creators` and `dataRoot` let project_create seed the new project's starter content as the
    // editor's New Project does (SeedStarterContent: the default UI font, the sky, the primitives);
    // a host that passes none creates the bare project.
    inline void RegisterProjectOpenTools(foundation::mcp::McpServer& server,
                                         ProjectSession& session, ProjectOwner& owner,
                                         const pipeline::AssetCreatorRegistry* creators = nullptr,
                                         StringView dataRoot = {})
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;
        ProjectOwner* o = &owner;
        const String seedRoot(dataRoot);

        server.RegisterTool(
            u8"project_create",
            u8"Scaffold a new project (Project.xml manifest + the standard directory layout) at a "
            u8"directory, seeded with the editor's starter content (Roboto as the default UI font, "
            u8"the default sky, the cube, sphere and plane). Does not open it - call project_open "
            u8"next.",
            SchemaBuilder()
                .Str(u8"directory", u8"path to create the project at", true)
                .Str(u8"name", u8"the project's display name", true)
                .Build(),
                foundation::mcp::ToolAnnotations::Creates(),
            [creators, seedRoot](const JsonValue& args) -> ToolResult
            {
                const String directory = args.Get(u8"directory").AsString();
                const String name = args.Get(u8"name").AsString();
                const Status st = editor::EditorProject::Create(editor::EditorRootAllocator(), directory.AsView(), name.AsView());
                if (!st.IsOk())
                {
                    return Err(Format(u8"could not create project at '{}' (error {})",
                                      directory.AsView(), static_cast<i32>(st.Code())));
                }
                // The starter content, as the editor's New Project seeds it: without it a project
                // made here had no default UI font, and its exported game showed no text.
                bool seeded = false;
                if (creators != nullptr && !seedRoot.IsEmpty())
                {
                    UniquePtr<editor::EditorProject> project =
                        editor::EditorProject::Open(editor::EditorRootAllocator(), directory.AsView());
                    if (project)
                    {
                        editor::SeedStarterContent(editor::EditorRootAllocator(), *project, *creators,
                                                   seedRoot.AsView());
                        seeded = project->SaveSettings().IsOk();
                    }
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"created", JsonValue::MakeBool(true));
                out.Set(u8"directory", JsonValue::MakeString(directory));
                out.Set(u8"seeded", JsonValue::MakeBool(seeded));
                return out;
            });

        server.RegisterTool(
            u8"project_open",
            u8"Open a project (mounts its source + cooked content databases) as the session's "
            u8"current project.",
            SchemaBuilder().Str(u8"directory", u8"the project directory", true).Build(),
            foundation::mcp::ToolAnnotations::Rebuilds(),
            [s, o](const JsonValue& args) -> ToolResult
            {
                const String directory = args.Get(u8"directory").AsString();
                UniquePtr<editor::EditorProject> opened =
                    editor::EditorProject::Open(editor::EditorRootAllocator(), directory.AsView());
                if (!opened)
                {
                    return Err(Format(u8"could not open project at '{}' (missing or unreadable "
                                      u8"Project.xml?)",
                                      directory.AsView()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"name", JsonValue::MakeString(String(opened->Name())));
                out.Set(u8"directory", JsonValue::MakeString(String(opened->Directory())));
                s->project = nullptr; // the previous project dies with its owner slot
                o->project = Move(opened);
                s->project = o->project.Get();
                return out;
            });
    }

    // Registers project_info against `server` - the open project's identity, part of the
    // surface every host serves.
    inline void RegisterProjectInfoTool(foundation::mcp::McpServer& server,
                                        ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"project_info",
            u8"Details about the currently-open project: name, directory, sources root, and its "
            u8"`settings`, what the editor's Project Settings dialog edits - the default scene, "
            u8"startup script, default input map, bus layout, UI theme, loading screen and UI font "
            u8"(each {guid, path}, or null when unset), the native module and the MSAA samples. "
            u8"project_settings_set changes them.",
            SchemaBuilder().Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue& /*args*/) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"name", JsonValue::MakeString(String(s->project->Name())));
                out.Set(u8"directory", JsonValue::MakeString(String(s->project->Directory())));
                out.Set(u8"sourcesRoot", JsonValue::MakeString(s->project->SourcesRoot()));
                out.Set(u8"settings", detail::ProjectSettingsJson(*s->project));
                return out;
            });
    }

    // Registers asset_list / asset_info against `server`, reading the open project's content
    // databases. Register alongside the project tools (same ProjectSession).
    inline void RegisterAssetTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        const auto pickDb = [](ProjectSession* sess, const JsonValue& args) -> content::ContentDatabase&
        {
            return args.Get(u8"database").AsString() == StringView(u8"cooked")
                       ? sess->project->CookedDb()
                       : sess->project->SourceDb();
        };

        server.RegisterTool(
            u8"asset_list",
            u8"List the assets in the open project's content database (guid, name, type, group).",
            SchemaBuilder()
                .Enum(u8"database", detail::DatabaseChoices(),
                      u8"which content database (default: source)")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, pickDb](const JsonValue& args) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                JsonValue assets = JsonValue::MakeArray();
                detail::CollectAssets(pickDb(s, args).RootGroup(), String(), assets);
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(assets.Count())));
                out.Set(u8"assets", Move(assets));
                return out;
            });

        server.RegisterTool(
            u8"asset_info",
            u8"Details about one asset in the open project, by guid.",
            SchemaBuilder()
                .Str(u8"guid", u8"the asset guid (canonical 8-4-4-4-12 form)", true)
                .Enum(u8"database", detail::DatabaseChoices(),
                      u8"which content database (default: source)")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, pickDb](const JsonValue& args) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String guidText = args.Get(u8"guid").AsString();
                Guid id;
                if (!Guid::TryParse(guidText.AsView(), id))
                {
                    return Err(Format(u8"invalid guid '{}'", guidText.AsView()));
                }
                content::Instance* inst = pickDb(s, args).GetInstance(id);
                if (inst == nullptr)
                {
                    return Err(Format(u8"no asset with guid '{}'", guidText.AsView()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(inst->Id()));
                out.Set(u8"name", JsonValue::MakeString(String(inst->Name())));
                out.Set(u8"type", JsonValue::MakeString(String(inst->TypeName())));
                out.Set(u8"typeNamespace", JsonValue::MakeString(String(inst->TypeNamespace())));
                return out;
            });
    }

    namespace detail
    {
        /// asset_import's `options`: the importer's toggles by their labels.
        inline JsonValue ImportOptionsSchema()
        {
            JsonValue property = JsonValue::MakeObject();
            property.Set(u8"type", JsonValue::MakeString(u8"object"));
            property.Set(u8"description",
                         JsonValue::MakeString(
                             u8"the importer's options, as the import dialog's checkboxes: "
                             u8"{\"<toggle>\": true|false}, the toggle named by its label, case and "
                             u8"spaces ignored (a model's \"Generate collision\", say); an unknown "
                             u8"one is refused with the importer's list, unnamed toggles keep their "
                             u8"defaults, and the result lists every toggle's value"));
            // A map of names the tool resolves itself (it refuses one it does not know, with its
            // own list): the server's schema check takes any key here.
            property.Set(u8"additionalProperties", JsonValue::MakeBool(true));
            return property;
        }

        /// A toggle's label against a caller's key: case and spaces ignored, so "Generate
        /// collision", "generate collision" and "generateCollision" all name it.
        inline bool SameToggleName(StringView label, StringView key)
        {
            const auto squeeze = [](StringView text)
            {
                String out;
                for (const utf8char c : text)
                {
                    if (c != u8' ')
                    {
                        out += static_cast<utf8char>((c >= u8'A' && c <= u8'Z') ? c + 32 : c);
                    }
                }
                return out;
            };
            return squeeze(label) == squeeze(key);
        }
    }

    // Registers asset_import / asset_cook - the WRITE side. Routing (which importer, by
    // extension), refusals and the result shapes are here, shared by every host; the work runs
    // through the host's IProjectOperations - inline on the stdio host, the editor's cook and
    // job services otherwise, the tool re-entered each pump until they finish. `importers` is
    // the host's registry (the same set the editor's drag-drop routes through).
    inline void RegisterAssetWriteTools(foundation::mcp::McpServer& server, ProjectSession& session,
                                        pipeline::ImporterRegistry& importers,
                                        IProjectOperations& operations)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolOutcome;
        ProjectSession* s = &session;
        pipeline::ImporterRegistry* imp = &importers;
        IProjectOperations* ops = &operations;

        server.RegisterTool(
            u8"asset_import",
            u8"Import an OS file into the open project: copy it under Sources/ and create the typed "
            u8"Asset in the source database (routed by extension). When several importers claim "
            u8"the extension the first is used and the result names the alternatives under "
            u8"`alsoClaimableBy`; pass `importer` to choose. Does not cook - call asset_cook next.",
            SchemaBuilder()
                .Str(u8"source", u8"absolute path to the file to import", true)
                .Str(u8"group", u8"source-DB group path to place it in (slash-joined; default root)")
                .Str(u8"importer", u8"which importer to use when several claim the extension, by "
                                   u8"label (the result of an unhinted import lists them)")
                .Property(u8"options", detail::ImportOptionsSchema())
                .Build(),
                foundation::mcp::ToolAnnotations::Creates(),
            [s, imp, ops](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                ImportRequest request;
                request.source = args.Get(u8"source").AsString();
                request.groupPath = args.Get(u8"group").AsString();
                const String ext = pipeline::FileExtensionLower(request.source.AsView());
                // Every claimant, as the interactive path asks (FindFor's first claimant
                // always won: `.png` is claimed by the texture, image and heightfield
                // importers). No hint = the first, with the alternatives in the result.
                const Array<pipeline::IFileImporter*> claimants = imp->FindAllFor(ext.AsView());
                if (claimants.IsEmpty())
                {
                    return Err(Format(u8"no importer registered for '.{}' files", ext.AsView()));
                }
                const String hint = args.Get(u8"importer").AsString();
                for (pipeline::IFileImporter* candidate : claimants)
                {
                    if (hint.IsEmpty() || candidate->Label() == hint.AsView())
                    {
                        request.importer = candidate;
                        break;
                    }
                }
                if (request.importer == nullptr)
                {
                    String refusal = Format(u8"no importer '{}' claims '.{}'; the claimants are:",
                                            hint.AsView(), ext.AsView());
                    for (pipeline::IFileImporter* candidate : claimants)
                    {
                        refusal += u8" ";
                        refusal += candidate->Label();
                    }
                    return Err(Move(refusal));
                }
                JsonValue alsoClaimableBy = JsonValue::MakeArray();
                for (pipeline::IFileImporter* candidate : claimants)
                {
                    if (candidate != request.importer)
                    {
                        alsoClaimableBy.Add(JsonValue::MakeString(String(candidate->Label())));
                    }
                }
                // The options: the importer's defaults, then the call's toggles by label.
                request.options = request.importer->CreateOptions(editor::EditorRootAllocator());
                Array<pipeline::ImportOptions::Toggle> toggles;
                if (request.options.Get() != nullptr)
                {
                    toggles = request.options->Toggles();
                }
                if (args.Has(u8"options"))
                {
                    const JsonValue asked = args.Get(u8"options");
                    if (!asked.IsObject())
                    {
                        return Err(String(u8"`options` takes an object of toggles: "
                                          u8"{\"Generate collision\": true}"));
                    }
                    for (i64 i = 0; i < asked.Count(); ++i)
                    {
                        const String key = asked.KeyAt(i);
                        pipeline::ImportOptions::Toggle* found = nullptr;
                        for (pipeline::ImportOptions::Toggle& toggle : toggles)
                        {
                            if (detail::SameToggleName(toggle.label, key.AsView()))
                            {
                                found = &toggle;
                            }
                        }
                        if (found == nullptr)
                        {
                            String refusal = Format(u8"the {} importer has no option '{}'; its "
                                                    u8"options are: ",
                                                    request.importer->Label(), key.AsView());
                            if (toggles.IsEmpty())
                            {
                                refusal += u8"none";
                            }
                            for (usize t = 0; t < toggles.Size(); ++t)
                            {
                                if (t > 0)
                                {
                                    refusal += u8", ";
                                }
                                refusal += toggles[t].label;
                            }
                            return Err(Move(refusal));
                        }
                        const JsonValue value = asked.Get(key);
                        if (!value.IsBool())
                        {
                            return Err(Format(u8"option '{}' takes true or false", key.AsView()));
                        }
                        *found->value = value.AsBool();
                    }
                }
                OperationStep<ImportOutcome> step = ops->Import(call, request);
                if (!step.HasValue())
                {
                    return Err(Move(step.Error()));
                }
                if (!step.Value().HasValue())
                {
                    return ToolOutcome::NotFinished(); // the host's import is still running
                }
                const ImportOutcome& done = step.Value().Value();
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"prepareMs", JsonValue::MakeNumber(static_cast<f64>(done.prepareMs)));
                out.Set(u8"mainMs", JsonValue::MakeNumber(static_cast<f64>(done.mainMs)));
                out.Set(u8"flushMs", JsonValue::MakeNumber(static_cast<f64>(done.flushMs)));
                out.Set(u8"deferredWrites",
                        JsonValue::MakeNumber(static_cast<f64>(done.deferredWrites)));
                out.Set(u8"guid", detail::GuidToJson(done.guid));
                out.Set(u8"name", JsonValue::MakeString(done.name));
                out.Set(u8"type", JsonValue::MakeString(done.type));
                out.Set(u8"typeNamespace", JsonValue::MakeString(done.typeNamespace));
                out.Set(u8"importer", JsonValue::MakeString(done.importer));
                out.Set(u8"alsoClaimableBy", Move(alsoClaimableBy));
                if (!toggles.IsEmpty())
                {
                    JsonValue applied = JsonValue::MakeObject();
                    for (const pipeline::ImportOptions::Toggle& toggle : toggles)
                    {
                        applied.Set(String(toggle.label), JsonValue::MakeBool(*toggle.value));
                    }
                    out.Set(u8"options", Move(applied));
                }
                return out;
            });

        server.RegisterTool(
            u8"asset_cook",
            u8"Run the incremental cook over the open project: plan the dirty set and build it into "
            u8"the cooked database. Returns the cook stats (planned/cooked/failed/orphans).",
            SchemaBuilder()
                .Boolean(u8"force", u8"re-cook every buildable asset regardless of cleanliness")
                .Build(),
                foundation::mcp::ToolAnnotations::Rebuilds(),
            [s, ops](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                OperationStep<CookOutcome> step = ops->Cook(call, args.Get(u8"force").AsBool());
                if (!step.HasValue())
                {
                    return Err(Move(step.Error()));
                }
                if (!step.Value().HasValue())
                {
                    return ToolOutcome::NotFinished(); // the host's cook is still running
                }
                const CookOutcome& done = step.Value().Value();
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"planned", JsonValue::MakeNumber(static_cast<f64>(done.planned)));
                out.Set(u8"cooked", JsonValue::MakeNumber(static_cast<f64>(done.cooked)));
                out.Set(u8"failed", JsonValue::MakeNumber(static_cast<f64>(done.failed)));
                out.Set(u8"orphansSwept",
                        JsonValue::MakeNumber(static_cast<f64>(done.orphansSwept)));
                out.Set(u8"upToDate", JsonValue::MakeNumber(static_cast<f64>(done.upToDate)));
                out.Set(u8"unbuildable",
                        JsonValue::MakeNumber(static_cast<f64>(done.unbuildable)));
                return out;
            });
    }

    // The stdio host's operations: everything runs on the calling thread and every step
    // answers at once. The cook is the driver's plan + execute over second mounts on Sources/ +
    // Cache/ with its own job system (so its timings mean what the editor's would); the import
    // is the editor's two-phase path run inline (worker prepare, main-thread fan-out, the
    // deferred flush - timed apart, so the tool reports what the editor's UI thread would have
    // paid); the export is RunExportInline. `hostToolDir` and `dataRoot` are the export's.
    class InlineProjectOperations final : public IProjectOperations
    {
    public:
        InlineProjectOperations(ProjectSession& session, pipeline::BuilderRegistry& builders,
                                String hostToolDir, String dataRoot)
            : m_session(&session), m_builders(&builders), m_hostToolDir(Move(hostToolDir)),
              m_dataRoot(Move(dataRoot))
        {
        }

        [[nodiscard]] OperationStep<CookOutcome> Cook(foundation::mcp::ToolCall&, bool force) override
        {
            editor::EditorProject& project = *m_session->project;
            const String sourcesRoot = project.SourcesRoot();
            const String cacheRoot = project.CacheRoot();
            vfs::NativeFileSystem sourcesMount(sourcesRoot.AsView(), editor::EditorRootAllocator());
            vfs::NativeFileSystem cacheMount(cacheRoot.AsView(), editor::EditorRootAllocator());
            JobSystem jobs(editor::EditorRootAllocator());
            pipeline::CookDriver driver(editor::EditorRootAllocator(), project.SourceDb(),
                                        project.CookedDb(), *m_builders, &sourcesMount,
                                        &cacheMount, &jobs);
            pipeline::CookPlan plan = driver.Plan(force);
            const pipeline::CookStats stats = driver.Execute(plan);
            CookOutcome outcome;
            outcome.planned = plan.dirty.Size();
            outcome.cooked = stats.cooked;
            outcome.failed = stats.failed;
            outcome.orphansSwept = stats.orphansSwept;
            outcome.upToDate = plan.upToDate;
            outcome.unbuildable = plan.unbuildable;
            return Optional<CookOutcome>(outcome);
        }

        /// After an import has landed: what the host's pipeline makes of it, a model's prefab
        /// among them (pipeline::AfterImport), as the editor's import does. Unset, an import
        /// creates only what its importer makes.
        Function<void(content::Instance&, const pipeline::ImportOptions*)> onImported;

        [[nodiscard]] OperationStep<ImportOutcome> Import(foundation::mcp::ToolCall&,
                                                          const ImportRequest& request) override
        {
            editor::EditorProject& project = *m_session->project;
            content::Group* group = detail::ResolveGroupPath(project.SourceDb().RootGroup(),
                                                             request.groupPath.AsView());
            pipeline::ImportContext ctx{editor::EditorRootAllocator(), project.SourcesRoot().AsView()};
            pipeline::IFileImporter& importer = *request.importer;
            const Stopwatch prepareClock = Stopwatch::StartNew();
            RefPtr<Object> prepared =
                importer.WantsWorkerPrepare()
                    ? importer.PrepareOnWorker(request.source.AsView(), editor::EditorRootAllocator())
                    : RefPtr<Object>{};
            ImportOutcome outcome;
            outcome.prepareMs = static_cast<i64>(prepareClock.Elapsed().AsMilliseconds());
            Array<pipeline::DeferredImportWrite> deferred;
            const Stopwatch mainClock = Stopwatch::StartNew();
            Result<content::Instance*> imported =
                importer.Import(request.source.AsView(), ctx, *group, request.options.Get(),
                                prepared.Get(), &deferred);
            outcome.mainMs = static_cast<i64>(mainClock.Elapsed().AsMilliseconds());
            if (!imported.HasValue())
            {
                return Err(Format(u8"import of '{}' failed (error {})", request.source.AsView(),
                                  static_cast<i32>(imported.Error())));
            }
            const Stopwatch flushClock = Stopwatch::StartNew();
            for (pipeline::DeferredImportWrite& write : deferred)
            {
                if (!write.Execute().IsOk())
                {
                    return Err(Format(u8"import of '{}': deferred write '{}' failed",
                                      request.source.AsView(), write.Label()));
                }
            }
            outcome.flushMs = static_cast<i64>(flushClock.Elapsed().AsMilliseconds());
            outcome.deferredWrites = deferred.Size();
            content::Instance* inst = imported.Value();
            outcome.guid = inst->Id();
            outcome.name = String(inst->Name());
            outcome.type = String(inst->TypeName());
            outcome.typeNamespace = String(inst->TypeNamespace());
            outcome.importer = String(importer.Label());
            if (onImported)
            {
                onImported(*inst, request.options.Get());
            }
            return Optional<ImportOutcome>(Move(outcome));
        }

        [[nodiscard]] OperationStep<ExportOutcome> Export(foundation::mcp::ToolCall&,
                                                          const ExportRequest& request) override
        {
            return RunExportInline(*m_session, *m_builders, m_hostToolDir.AsView(),
                                   m_dataRoot.AsView(), request);
        }

        /// Creates at once; nothing follows it here: the agent cooks next.
        [[nodiscard]] OperationStep<CreateOutcome> Create(foundation::mcp::ToolCall&,
                                                          const CreateRequest& request) override
        {
            Result<content::Instance*, String> created =
                RunAssetCreation(*m_session->project, request);
            if (!created.HasValue())
            {
                return Err(Move(created.Error()));
            }
            return Optional<CreateOutcome>(CreateOutcomeOf(*created.Value()));
        }

        /// Deletes at once: no pages here, and the next cook sweeps the orphaned product.
        [[nodiscard]] OperationStep<bool> Delete(foundation::mcp::ToolCall&, const Guid& id) override
        {
            if (!m_session->project->SourceDb().DeleteInstance(id).IsOk())
            {
                return Err(String(u8"could not delete the asset (log_read says why)"));
            }
            return Optional<bool>(true);
        }

    private:
        ProjectSession* m_session;
        pipeline::BuilderRegistry* m_builders;
        String m_hostToolDir;
        String m_dataRoot;
    };
}

export namespace editor::mcp
{
    // Paths the shared surface needs that only a host can discover, each host its own way (the
    // stdio host walks up from its executable, the editor knows its data root). An empty
    // knownIssues path leaves known_issues erring with guidance; an empty shippingDocsDir
    // registers no docs:// resources.
    struct EngineToolPaths
    {
        String knownIssues;     ///< the curated KnownIssues.md (Documentation/Shipping)
        String shippingDocsDir; ///< the curated shipping docs directory (docs://<name>)
    };

    // The number of tools RegisterEngineTools registers. A new engine tool bumps this
    // DELIBERATELY; a lost registration then fails the test loudly (the Pipeline::Registration
    // pattern). host_info and the stdio host's project_create / project_open are NOT in it -
    // each host registers its own.
    inline constexpr usize kEngineToolCount = 30;

    // Every *.md in `docsDir` as a read-only `docs://<FileName>` resource: the CURATED,
    // distribution-facing docs set (internal design/spec/process docs are never exposed).
    // Readers re-read the file per request, so edits are live without restarting the host.
    inline void RegisterShippingDocResources(foundation::mcp::McpServer& server,
                                             StringView docsDir)
    {
        Array<String> names;
        (void)ListDirectory(
            docsDir,
            [](void* ctx, StringView name, bool isDirectory)
            {
                if (!isDirectory && name.EndsWith(u8".md"))
                {
                    static_cast<Array<String>*>(ctx)->PushBack(String(name));
                }
            },
            &names);
        // By name, not by the filesystem's order: resources/list reads the same everywhere.
        names.Sort([](const String& a, const String& b) { return a.AsView().Compare(b.AsView()) < 0; });
        for (const String& name : names)
        {
            const String path = PathJoin(docsDir, name.AsView());
            server.RegisterResource(
                Format(u8"docs://{}", name.AsView()), name, String(u8"text/markdown"),
                Format(u8"engine documentation: {} (curated, distribution-facing)", name.AsView()),
                [path]() -> Result<String, String>
                {
                    FileStream stream(path.AsView(), FileMode::Read);
                    if (!stream.IsValid())
                    {
                        return Err(Format(u8"could not read '{}'", path.AsView()));
                    }
                    const i64 size = stream.Size();
                    Array<byte> bytes;
                    bytes.Resize(static_cast<usize>(size));
                    if (stream.Read(bytes.Data(), bytes.Size()) != static_cast<u64>(size))
                    {
                        return Err(Format(u8"could not read '{}'", path.AsView()));
                    }
                    return String(StringView(reinterpret_cast<const utf8char*>(bytes.Data()),
                                             bytes.Size()));
                });
        }
    }

    // The engine tool surface EVERY MCP host serves, listed ONCE: reflection (type_list /
    // type_info), script_api, project_info / project_settings_set, export_presets /
    // export_preset_set, the asset tools (list / info / import / cook / uses /
    // creators / create / data read and write),
    // project_health, the log tools (log_read / log_write / known_issues), the scene and prefab
    // tools, script_validate / script_create, project_export, and the docs:// + project://
    // resources. A host adds what only it can serve on top (the stdio host: project_create /
    // project_open; the editor: its live tools) and its own host_info. `operations` is how
    // THIS host runs the cook / import / export behind their tools (IProjectOperations).
    inline void RegisterEngineTools(foundation::mcp::McpServer& server, ProjectSession& session,
                                    pipeline::BuilderRegistry& builders,
                                    pipeline::ImporterRegistry& importers,
                                    const pipeline::AssetCreatorRegistry& creators,
                                    editor::EditorLogBuffer& logBuffer,
                                    const EngineToolPaths& paths, IProjectOperations& operations)
    {
        foundation::mcp::RegisterReflectionTools(server);
        foundation::mcp::RegisterScriptTools(server);
        RegisterProjectInfoTool(server, session);
        RegisterProjectSettingsTool(server, session);
        RegisterExportPresetTools(server, session, GetExecutableDirectory()); // the host's own player

        RegisterAssetTools(server, session);
        RegisterAssetWriteTools(server, session, importers, operations);
        RegisterAssetCreateTools(server, session, creators, operations);
        RegisterAssetDataTools(server, session);
        RegisterAssetUsesTool(server, session, builders);
        RegisterAssetDeleteTool(server, session, builders, operations);
        RegisterProjectHealthTool(server, session, builders);
        RegisterLogTools(server, logBuffer, paths.knownIssues);
        RegisterSceneTools(server, session);
        // The scene format reference, generated NOW from this host's own registrations (the
        // engine composition's managers, settings systems and factory descriptions, plus its
        // builders) and served live: docs://generated/* beside the shipping docs,
        // component_schema for one entry. Every host answers the same for the same build.
        {
            const SceneReference reference =
                GenerateSceneReference(editor::EditorRootAllocator(), builders);
            RegisterComponentSchemaTool(server, reference);
            if (!paths.shippingDocsDir.IsEmpty())
            {
                RegisterShippingDocResources(server, paths.shippingDocsDir.AsView());
            }
            RegisterSceneReferenceResources(server, reference);
        }
        RegisterProjectResources(server, session);
        RegisterScriptValidateTool(server);
        RegisterScriptCreateTool(server, session);
        RegisterProjectExportTool(server, session, operations);
    }
}

export namespace editor::mcp
{
    // Locates the curated shipping docs for a host's EngineToolPaths, walking UP from each
    // start directory in turn (a host passes its executable's directory, then its working
    // directory) and checking the distribution layout first (KnownIssues.md staged beside the
    // executable) and the engine checkout second (Documentation/Shipping/). The first hit
    // wins per field; a field left empty means not found - known_issues then errs with
    // guidance and no docs:// resources register. Same walk for both hosts, so they agree.
    inline void LocateShippingDocs(Span<const String> starts, EngineToolPaths& paths)
    {
        for (const String& start : starts)
        {
            for (StringView dir = start.AsView(); !dir.IsEmpty(); dir = PathParent(dir))
            {
                if (paths.knownIssues.IsEmpty())
                {
                    const String staged = PathJoin(dir, u8"KnownIssues.md");
                    if (FileExists(staged.AsView()))
                    {
                        paths.knownIssues = staged;
                    }
                }
                const String shipping = PathJoin(PathJoin(dir, u8"Documentation"), u8"Shipping");
                if (DirectoryExists(shipping.AsView()))
                {
                    if (paths.shippingDocsDir.IsEmpty())
                    {
                        paths.shippingDocsDir = shipping;
                    }
                    if (paths.knownIssues.IsEmpty())
                    {
                        const String checkout = PathJoin(shipping.AsView(), u8"KnownIssues.md");
                        if (FileExists(checkout.AsView()))
                        {
                            paths.knownIssues = checkout;
                        }
                    }
                }
                if (!paths.knownIssues.IsEmpty() && !paths.shippingDocsDir.IsEmpty())
                {
                    return;
                }
            }
        }
    }
}
