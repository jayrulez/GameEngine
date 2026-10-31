// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - EditorProjectOperations implementation: the cook wait, the two-phase import
// over the job service, and the cook-then-job export.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.app;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.importer;
import editor.core;
import foundation.mcp;
import editor.mcp;

using namespace foundation::core;
namespace content = foundation::content;
using editor::mcp::CookOutcome;
using editor::mcp::ExportOutcome;
using editor::mcp::ImportOutcome;
using editor::mcp::OperationStep;

namespace editor::app
{
    bool EditorProjectOperations::TimedOut(u64 startedTicks) const
    {
        return TicksToSeconds(GetTicks() - startedTicks) > m_seams.timeoutSeconds;
    }

    void EditorProjectOperations::BeginCook(CookWait& wait, bool force)
    {
        wait.active = true;
        wait.sinceRevision = m_seams.cook->Revision();
        wait.startedTicks = GetTicks();
        m_seams.cook->RequestCook(force); // remembered by the service if one is in flight
    }

    Result<bool, String> EditorProjectOperations::PollCook(CookWait& wait)
    {
        if (m_seams.cook->Revision() > wait.sinceRevision && m_seams.cook->IsIdle())
        {
            wait.active = false;
            return true;
        }
        if (TimedOut(wait.startedTicks))
        {
            wait.active = false;
            return Err(Format(u8"the cook did not finish within {} s - read log_read (category "
                              u8"Cook) for where it stands",
                              static_cast<i64>(m_seams.timeoutSeconds)));
        }
        return false;
    }

    CookOutcome EditorProjectOperations::SummaryOutcome() const
    {
        const editor::CookSummary& summary = m_seams.cook->LastCookSummary();
        CookOutcome outcome;
        outcome.planned = summary.planned;
        outcome.cooked = summary.cooked;
        outcome.failed = summary.failed;
        outcome.orphansSwept = summary.orphansSwept;
        outcome.upToDate = summary.upToDate;
        outcome.unbuildable = summary.unbuildable;
        return outcome;
    }

    OperationStep<CookOutcome> EditorProjectOperations::Cook(foundation::mcp::ToolCall& call,
                                                             bool force)
    {
        if (!m_seams.cook->IsReady())
        {
            return Err(String(u8"the editor has no cook service for the open project"));
        }
        CookWait& wait = CallState<CookWait>(call);
        if (!wait.active)
        {
            BeginCook(wait, force);
        }
        Result<bool, String> polled = PollCook(wait);
        if (!polled.HasValue())
        {
            return Err(Move(polled.Error()));
        }
        if (!polled.Value())
        {
            return Optional<CookOutcome>{};
        }
        return Optional<CookOutcome>(SummaryOutcome());
    }

    void EditorProjectOperations::SubmitImportFlush(ImportJob& job,
                                                    const editor::mcp::ImportRequest& request)
    {
        RefPtr<ImportShared> shared = job.shared;
        shared->jobDone = false;
        const String source = request.source;
        m_seams.jobs->Submit(
            Format(u8"Writing {}", pipeline::FileNameOf(source.AsView())).AsView(),
            Function<Status(editor::JobContext&)>{
                [shared, source](editor::JobContext& job) -> Status
                {
                    // Pure mount IO on the worker; the job lock keeps cooks out meanwhile.
                    Status result{};
                    const Stopwatch clock = Stopwatch::StartNew();
                    Array<pipeline::DeferredImportWrite>& writes = *shared->writes;
                    for (usize i = 0; i < writes.Size(); ++i)
                    {
                        job.SetStep(writes[i].Label(), i + 1, writes.Size());
                        job.SetFraction(static_cast<f32>(i) / static_cast<f32>(writes.Size()));
                        const Status written = writes[i].Execute();
                        if (!written.IsOk())
                        {
                            LOG_ERROR(u8"Import", u8"'{}': deferred write failed: '{}'",
                                      pipeline::FileNameOf(source.AsView()), writes[i].Label());
                            result = written;
                        }
                    }
                    shared->flushMs = static_cast<i64>(clock.Elapsed().AsMilliseconds());
                    return result;
                }},
            Function<void(Status)>{[shared](Status status)
                                   {
                                       shared->jobStatus = status;
                                       shared->jobDone = true;
                                   }});
    }

    OperationStep<ImportOutcome> EditorProjectOperations::Import(
        foundation::mcp::ToolCall& call, const editor::mcp::ImportRequest& request)
    {
        ImportJob& job = CallState<ImportJob>(call);
        // A failure or an answer ends the call, and its job state with it.
        const auto fail = [](String message) -> OperationStep<ImportOutcome>
        { return Err(Move(message)); };
        // Every write has landed and the import is complete, so the post-import effects run now.
        const auto finish = [this, &job, &request]() -> OperationStep<ImportOutcome>
        {
            ImportOutcome outcome = Move(job.outcome);
            if (m_seams.onImported)
            {
                if (content::Instance* primary =
                        m_seams.project->SourceDb().GetInstance(outcome.guid))
                {
                    m_seams.onImported(*primary, request.options.Get());
                }
            }
            return Optional<ImportOutcome>(Move(outcome));
        };
        if (job.phase == ImportJob::Phase::Idle)
        {
            job.startedTicks = GetTicks();
            job.shared = MakeRef<ImportShared>(*m_seams.allocator);
            job.shared->writes =
                MakeUnique<Array<pipeline::DeferredImportWrite>>(*m_seams.allocator);
            if (request.importer->WantsWorkerPrepare())
            {
                // Phase 1, the load, runs on the worker (the bulk of a mesh or texture import).
                job.phase = ImportJob::Phase::Preparing;
                RefPtr<ImportShared> shared = job.shared;
                pipeline::IFileImporter* importer = request.importer;
                const String source = request.source;
                IAllocator* allocator = m_seams.allocator;
                m_seams.jobs->Submit(
                    Format(u8"Reading {}", pipeline::FileNameOf(source.AsView())).AsView(),
                    Function<Status(editor::JobContext&)>{
                        [shared, importer, source, allocator](editor::JobContext& job) -> Status
                        {
                            job.SetStep(u8"loading + decoding", 1, 2);
                            const Stopwatch clock = Stopwatch::StartNew();
                            shared->prepared = importer->PrepareOnWorker(source.AsView(), *allocator);
                            shared->prepareMs = static_cast<i64>(clock.Elapsed().AsMilliseconds());
                            return shared->prepared.Get() != nullptr
                                       ? Status{}
                                       : Status{ErrorCode::InvalidArgument};
                        }},
                    Function<void(Status)>{[shared](Status status)
                                           {
                                               shared->jobStatus = status;
                                               shared->jobDone = true;
                                           }});
                return Optional<ImportOutcome>{};
            }
            job.phase = ImportJob::Phase::Placing;
        }
        if (job.phase == ImportJob::Phase::Preparing)
        {
            if (!job.shared->jobDone)
            {
                if (TimedOut(job.startedTicks))
                {
                    return fail(Format(u8"import of '{}': reading the file did not finish within "
                                       u8"{} s",
                                       request.source.AsView(),
                                       static_cast<i64>(m_seams.timeoutSeconds)));
                }
                return Optional<ImportOutcome>{};
            }
            if (!job.shared->jobStatus.IsOk())
            {
                return fail(Format(u8"import of '{}' failed: the file could not be read (see "
                                   u8"log_read, category Import)",
                                   request.source.AsView()));
            }
            job.phase = ImportJob::Phase::Placing;
        }
        if (job.phase == ImportJob::Phase::Placing)
        {
            // Phase 2, the cheap main-thread fan-out, never runs while a cook (or an export job)
            // reads the databases: the worker holds instance pointers snapshotted at plan time.
            if (m_seams.cook->MutationLocked())
            {
                if (TimedOut(job.startedTicks))
                {
                    return fail(Format(u8"import of '{}': the databases stayed locked by a cook "
                                       u8"or export for {} s",
                                       request.source.AsView(),
                                       static_cast<i64>(m_seams.timeoutSeconds)));
                }
                return Optional<ImportOutcome>{};
            }
            editor::EditorProject& project = *m_seams.project;
            content::Group* group = editor::mcp::detail::ResolveGroupPath(
                project.SourceDb().RootGroup(), request.groupPath.AsView());
            pipeline::ImportContext ctx{*m_seams.allocator, project.SourcesRoot().AsView()};
            ctx.onConflict = request.onConflict;
            ctx.ledgerPath = pipeline::ImportLedgerPath(project.CacheRoot().AsView());
            const Stopwatch clock = Stopwatch::StartNew();
            Result<content::Instance*> imported =
                request.importer->Import(request.source.AsView(), ctx, *group, request.options.Get(),
                                         job.shared->prepared.Get(), job.shared->writes.Get());
            job.mainMs = static_cast<i64>(clock.Elapsed().AsMilliseconds());
            if (!imported.HasValue() || imported.Value() == nullptr)
            {
                return fail(Format(u8"import of '{}' failed (error {}) - see log_read, category "
                                   u8"Import",
                                   request.source.AsView(),
                                   imported.HasValue() ? 0 : static_cast<i32>(imported.Error())));
            }
            content::Instance* instance = imported.Value();
            job.outcome.guid = instance->Id();
            job.outcome.name = String(instance->Name());
            job.outcome.type = String(instance->TypeName());
            job.outcome.typeNamespace = String(instance->TypeNamespace());
            job.outcome.importer = String(request.importer->Label());
            job.outcome.deferredWrites = job.shared->writes->Size();
            job.outcome.prepareMs = job.shared->prepareMs;
            job.outcome.mainMs = job.mainMs;
            if (job.shared->writes->IsEmpty())
            {
                return finish();
            }
            // Phase 3, the bulk stream writes, runs on the worker.
            job.phase = ImportJob::Phase::Flushing;
            SubmitImportFlush(job, request);
            return Optional<ImportOutcome>{};
        }
        // Flushing.
        if (!job.shared->jobDone)
        {
            if (TimedOut(job.startedTicks))
            {
                return fail(Format(u8"import of '{}': writing its data did not finish within {} s",
                                   request.source.AsView(),
                                   static_cast<i64>(m_seams.timeoutSeconds)));
            }
            return Optional<ImportOutcome>{};
        }
        if (!job.shared->jobStatus.IsOk())
        {
            return fail(Format(u8"import of '{}': a deferred write failed (see log_read, "
                               u8"category Import)",
                               request.source.AsView()));
        }
        job.outcome.flushMs = job.shared->flushMs;
        return finish();
    }

    void EditorProjectOperations::SubmitExportJob(ExportJob& job)
    {
        RefPtr<ExportShared> shared = job.shared;
        shared->jobDone = false;
        editor::EditorProject* project = m_seams.project;
        pipeline::BuilderRegistry* builders = m_seams.builders;
        const String toolDir = m_seams.hostToolDir;
        const String templatesRoot = m_seams.templatesRoot;
        const String dataRoot = m_seams.dataRoot;
        m_seams.jobs->Submit(
            Format(u8"Export {}", shared->preset.name.AsView()).AsView(),
            Function<Status(editor::JobContext&)>{
                [shared, project, builders, toolDir, templatesRoot, dataRoot](
                    editor::JobContext& ctx) -> Status
                {
                    foundation::vfs::NativeFileSystem toolFs(toolDir.AsView(),
                                                             editor::EditorRootAllocator());
                    foundation::vfs::NativeFileSystem rootFs(templatesRoot.AsView(),
                                                             editor::EditorRootAllocator());
                    editor::TemplateRegistry templates;
                    templates.Refresh(templatesRoot.AsView(), &rootFs, toolDir.AsView(), &toolFs);
                    const editor::ExportProgress onProgress = [&ctx](StringView step, f32 fraction)
                    {
                        ctx.SetStep(step);
                        ctx.SetFraction(fraction);
                    };
                    // The cook already ran through the cook service (cook=false); the scene
                    // streams and the reachable set were computed on the main thread.
                    return editor::ExportOne(
                        *project, shared->preset, templates, *builders, shared->outRoot.AsView(),
                        dataRoot.AsView(), /*rebuild=*/false, &shared->result, onProgress,
                        /*cook=*/false, &shared->sceneStreams, /*scanner=*/nullptr,
                        shared->reachableValid ? &shared->reachableRoots : nullptr);
                }},
            Function<void(Status)>{[shared](Status status)
                                   {
                                       shared->jobStatus = status;
                                       shared->jobDone = true;
                                   }});
    }

    OperationStep<ExportOutcome> EditorProjectOperations::Export(
        foundation::mcp::ToolCall& call, const editor::mcp::ExportRequest& request)
    {
        ExportJob& job = CallState<ExportJob>(call);
        // A failure or an answer ends the call, and its job state with it.
        const auto fail = [](String message) -> OperationStep<ExportOutcome>
        { return Err(Move(message)); };
        if (job.phase == ExportJob::Phase::Idle)
        {
            if (!m_seams.cook->IsReady())
            {
                return Err(String(u8"the editor has no cook service for the open project"));
            }
            job.startedTicks = GetTicks();
            job.shared = MakeRef<ExportShared>(*m_seams.allocator);
            job.shared->preset = request.preset;
            job.shared->outRoot = request.outRoot;
            // Cook first, through the cook service, as the Export menu does before its job.
            job.phase = ExportJob::Phase::Cooking;
            BeginCook(job.cook, request.rebuild);
        }
        if (job.phase == ExportJob::Phase::Cooking)
        {
            Result<bool, String> polled = PollCook(job.cook);
            if (!polled.HasValue())
            {
                return fail(Move(polled.Error()));
            }
            if (!polled.Value())
            {
                return Optional<ExportOutcome>{};
            }
            // The main-thread pre-pass: scene streams over the full manager set, and the
            // reachable closure when the preset prunes (both need the scene machinery only the
            // main thread may run). An unwired hook leaves that half out, as the menu does.
            editor::EditorProject& project = *m_seams.project;
            ExportShared& shared = *job.shared;
            if (m_seams.context != nullptr && m_seams.context->SceneStreamStager)
            {
                editor::CollectSceneStreams(*project.SourceDb().RootGroup(),
                                            m_seams.context->SceneStreamStager,
                                            shared.sceneStreams);
            }
            if (shared.preset.pruneToReachable && m_seams.context != nullptr &&
                m_seams.context->SceneRefScanner)
            {
                editor::EditorContext* context = m_seams.context;
                const editor::SceneReferenceScanner adapter =
                    [context](content::Instance& instance, content::ContentDatabase& db,
                              editor::SceneReferences& refs)
                { context->SceneRefScanner(instance, db, refs.resources, refs.prefabs); };
                const Array<editor::ExportRoot> seeds = editor::CollectExportRoots(project);
                shared.reachableRoots = editor::ExpandReachableRoots(project, seeds, adapter);
                shared.reachableValid = true;
            }
            job.phase = ExportJob::Phase::Running;
            SubmitExportJob(job);
            return Optional<ExportOutcome>{};
        }
        // Running.
        if (!job.shared->jobDone)
        {
            if (TimedOut(job.startedTicks))
            {
                return fail(Format(u8"export of preset '{}' did not finish within {} s",
                                   request.preset.name.AsView(),
                                   static_cast<i64>(m_seams.timeoutSeconds)));
            }
            return Optional<ExportOutcome>{};
        }
        if (!job.shared->jobStatus.IsOk())
        {
            return fail(Format(u8"export of preset '{}' failed - read log_read (category "
                               u8"Export/Cook) for the failing step",
                               request.preset.name.AsView()));
        }
        ExportOutcome outcome;
        outcome.result = Move(job.shared->result);
        return Optional<ExportOutcome>(Move(outcome));
    }

    OperationStep<editor::mcp::CreateOutcome>
    EditorProjectOperations::Create(foundation::mcp::ToolCall& call,
                                    const editor::mcp::CreateRequest& request)
    {
        if (m_seams.cook->MutationLocked())
        {
            CreateWait& wait = CallState<CreateWait>(call);
            if (wait.startedTicks == 0)
            {
                wait.startedTicks = GetTicks();
            }
            if (TimedOut(wait.startedTicks))
            {
                return Err(Format(u8"create of a {}: the databases stayed locked by a cook or "
                                  u8"export for {} s",
                                  request.creator->label.AsView(),
                                  static_cast<i64>(m_seams.timeoutSeconds)));
            }
            return Optional<editor::mcp::CreateOutcome>{};
        }
        Result<content::Instance*, String> created =
            editor::mcp::RunAssetCreation(*m_seams.project, request);
        if (!created.HasValue())
        {
            return Err(Move(created.Error()));
        }
        if (m_seams.onCreated)
        {
            m_seams.onCreated(*request.creator, *created.Value());
        }
        return Optional<editor::mcp::CreateOutcome>(editor::mcp::CreateOutcomeOf(*created.Value()));
    }

    OperationStep<bool> EditorProjectOperations::Delete(foundation::mcp::ToolCall& call, const Guid& id)
    {
        if (m_seams.cook->MutationLocked())
        {
            CreateWait& wait = CallState<CreateWait>(call);
            if (wait.startedTicks == 0)
            {
                wait.startedTicks = GetTicks();
            }
            if (TimedOut(wait.startedTicks))
            {
                return Err(Format(u8"delete of an asset: the databases stayed locked by a cook or export for {} s",
                                  static_cast<i64>(m_seams.timeoutSeconds)));
            }
            return Optional<bool>{};
        }
        const bool deleted =
            m_seams.onDelete ? m_seams.onDelete(id) : m_seams.project->SourceDb().DeleteInstance(id).IsOk();
        if (!deleted)
        {
            return Err(String(u8"could not delete the asset (log_read says why)"));
        }
        return Optional<bool>(true);
    }
}
