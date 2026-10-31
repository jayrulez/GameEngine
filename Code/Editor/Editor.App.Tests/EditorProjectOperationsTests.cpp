// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the editor host's operations over the real services on a scratch project. A
// cook is requested through the cook service and answers not-finished until its revision lands. An
// import runs the two-phase path (prepare on a worker, placement on the main thread, then the
// deferred flush on the job service) and lands its stream. An export cooks first and then runs the
// export job, whose failure (there is no template here) reaches the re-entered call as the tool's
// error. Every step is pumped the way the editor pumps: the services' Update, then the operation
// again.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import editor.core;
import foundation.mcp;
import editor.mcp;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace content = foundation::content;

namespace
{
    // A two-phase importer: the worker "reads" the file into a payload, the main thread places
    // the asset, and one deferred stream write carries the bulk.
    class TwoPhaseImporter final : public pipeline::IFileImporter
    {
    public:
        explicit TwoPhaseImporter(bool worker, bool failPlacement = false)
            : m_worker(worker), m_failPlacement(failPlacement)
        {
        }
        [[nodiscard]] StringView Label() const override { return u8"TwoPhase"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"two";
        }
        [[nodiscard]] bool WantsWorkerPrepare() const override { return m_worker; }
        [[nodiscard]] RefPtr<Object> PrepareOnWorker(StringView, IAllocator& allocator) override
        {
            ++prepares;
            return RefPtr<Object>(MakeRef<Object>(allocator).Get());
        }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath);
        }
        [[nodiscard]] Result<content::Instance*>
        Import(StringView, const pipeline::ImportContext&, content::Group& group,
               const pipeline::ImportOptions*, Object* prepared,
               Array<pipeline::DeferredImportWrite>* deferred) override
        {
            ++imports;
            sawPrepared = prepared != nullptr;
            if (m_failPlacement)
            {
                return Err(ErrorCode::InvalidArgument);
            }
            content::Instance* instance =
                group.CreateInstance(u8"Imported", pipeline::ImportOptions::StaticType());
            if (deferred != nullptr)
            {
                pipeline::DeferredImportWrite write;
                write.instance = instance;
                write.streamName = String(u8"bulk");
                write.owned.PushBack(byte{7});
                write.owned.PushBack(byte{9});
                deferred->PushBack(Move(write));
            }
            return instance;
        }
        u32 prepares = 0;
        u32 imports = 0;
        bool sawPrepared = false;

    private:
        bool m_worker;
        bool m_failPlacement;
    };

    // Everything an operations object runs on, over a scratch project.
    struct Bench
    {
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        pipeline::BuilderRegistry builders{DefaultAllocator()};
        EditorCookService cook;
        EditorJobService jobs{DefaultAllocator()};
        String dir;

        explicit Bench(StringView leaf) : dir(leaf)
        {
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Ops").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(project);
            cook.Initialize(*project, builders);
            // Wired as the application does: a running job holds the databases, as a cook does.
            EditorJobService* jobsPtr = &jobs;
            cook.ExternalMutationLock = [jobsPtr]() { return jobsPtr->IsBusy(); };
        }
        ~Bench()
        {
            cook.Shutdown();
            jobs.Shutdown();
            project = nullptr;
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
        }
        app::EditorProjectOperationsSeams Seams(f64 timeoutSeconds = 30.0)
        {
            app::EditorProjectOperationsSeams seams;
            seams.allocator = &DefaultAllocator();
            seams.project = project.Get();
            seams.context = &context;
            seams.cook = &cook;
            seams.jobs = &jobs;
            seams.builders = &builders;
            seams.timeoutSeconds = timeoutSeconds;
            return seams;
        }
        // One editor frame: the services' main-thread updates.
        void Pump()
        {
            cook.Update({});
            jobs.Update({});
            SleepMilliseconds(1);
        }
    };

    // Re-enters `step` after each pump until it answers, as the host's pump does; the count of
    // not-finished answers says how many frames the caller waited.
    template <typename T, typename Step>
    editor::mcp::OperationStep<T> Drive(Bench& bench, Step step, u32& waited)
    {
        waited = 0;
        foundation::mcp::ToolCall call; // one call, re-entered after each pump
        for (u32 i = 0; i < 5000; ++i)
        {
            editor::mcp::OperationStep<T> result = step(call);
            if (!result.HasValue() || result.Value().HasValue())
            {
                return result;
            }
            call.isReentry = true;
            ++waited;
            bench.Pump();
        }
        return Err(String(u8"the step never answered"));
    }
}

TEST_CASE("editor-operations: a cook rides the cook service - not finished until its revision "
          "lands, then the summary; and again after it")
{
    Bench bench(u8"mcp_ops_cook");
    app::EditorProjectOperations ops(bench.Seams());
    u32 waited = 0;
    editor::mcp::OperationStep<editor::mcp::CookOutcome> first =
        Drive<editor::mcp::CookOutcome>(bench, [&ops](foundation::mcp::ToolCall& call) { return ops.Cook(call, false); }, waited);
    REQUIRE(first.HasValue());
    REQUIRE(first.Value().HasValue());
    CHECK(waited >= 1u); // the first entry only made the request; the worker answered later
    CHECK(first.Value().Value().planned == 0u);
    CHECK(bench.cook.Revision() == 1u);

    // The state resets: a second cook starts a new wait rather than answering from the old.
    editor::mcp::OperationStep<editor::mcp::CookOutcome> second =
        Drive<editor::mcp::CookOutcome>(bench, [&ops](foundation::mcp::ToolCall& call) { return ops.Cook(call, true); }, waited);
    REQUIRE(second.HasValue());
    REQUIRE(second.Value().HasValue());
    CHECK(waited >= 1u);
    CHECK(bench.cook.Revision() == 2u);
}

TEST_CASE("editor-operations: an import runs the two-phase path - worker prepare, main-thread "
          "placement, the deferred flush on the job service - and lands its stream")
{
    Bench bench(u8"mcp_ops_import");
    app::EditorProjectOperationsSeams seams = bench.Seams();
    // The host's after-import effects (a model's prefab, the cook, the browser) run once per
    // finished import, over its primary asset, and never for a failed one.
    u32 afterImports = 0;
    Guid lastImported;
    seams.onImported = [&](content::Instance& primary, const pipeline::ImportOptions*)
    {
        ++afterImports;
        lastImported = primary.Id();
    };
    app::EditorProjectOperations ops(Move(seams));
    TwoPhaseImporter importer(/*worker=*/true);
    editor::mcp::ImportRequest request;
    request.source = String(u8"anything.two");
    request.importer = &importer;

    u32 waited = 0;
    editor::mcp::OperationStep<editor::mcp::ImportOutcome> step =
        Drive<editor::mcp::ImportOutcome>(bench, [&](foundation::mcp::ToolCall& call) { return ops.Import(call, request); }, waited);
    REQUIRE(step.HasValue());
    REQUIRE(step.Value().HasValue());
    const editor::mcp::ImportOutcome& outcome = step.Value().Value();
    CHECK(waited >= 2u); // the prepare job, then the flush job, each landing through a pump
    CHECK(importer.prepares == 1u);
    CHECK(importer.imports == 1u);
    CHECK(importer.sawPrepared);
    CHECK(outcome.name == u8"Imported");
    CHECK(outcome.importer == u8"TwoPhase");
    CHECK(outcome.deferredWrites == 1u);
    // The bulk write reached the instance's stream.
    content::Instance* instance = bench.project->SourceDb().GetInstance(outcome.guid);
    REQUIRE(instance != nullptr);
    UniquePtr<IStream> bulk = instance->ReadData(u8"bulk");
    REQUIRE(bulk);
    CHECK(bulk->Size() == 2);
    CHECK(afterImports == 1u);
    CHECK(lastImported == outcome.guid); // after the writes landed, over the primary asset

    // An inline importer (no worker prepare) places at once and still flushes on the job.
    TwoPhaseImporter inlineImporter(/*worker=*/false);
    request.importer = &inlineImporter;
    step = Drive<editor::mcp::ImportOutcome>(bench, [&](foundation::mcp::ToolCall& call) { return ops.Import(call, request); }, waited);
    REQUIRE(step.HasValue());
    REQUIRE(step.Value().HasValue());
    CHECK(inlineImporter.prepares == 0u);
    CHECK_FALSE(inlineImporter.sawPrepared);

    // A placement failure is the tool's error, and the state is clean for the next call.
    TwoPhaseImporter failing(/*worker=*/false, /*failPlacement=*/true);
    request.importer = &failing;
    step = Drive<editor::mcp::ImportOutcome>(bench, [&](foundation::mcp::ToolCall& call) { return ops.Import(call, request); }, waited);
    REQUIRE_FALSE(step.HasValue());
    CHECK(step.Error().AsView().StartsWith(u8"import of 'anything.two' failed"));
    CHECK(afterImports == 2u); // a failed import runs no after-import effects
    request.importer = &inlineImporter;
    step = Drive<editor::mcp::ImportOutcome>(bench, [&](foundation::mcp::ToolCall& call) { return ops.Import(call, request); }, waited);
    REQUIRE(step.HasValue());
    REQUIRE(step.Value().HasValue());
}

// Each import's progress belongs to its own call, so two clients' imports run side by side, each
// to its own answer. A call whose caller leaves ends, and its job keeps what it shares alive until
// the job is done.
TEST_CASE("editor-operations: two imports run side by side, each on its own call")
{
    Bench bench(u8"mcp_ops_two_imports");
    app::EditorProjectOperations ops(bench.Seams());
    TwoPhaseImporter importer(/*worker=*/true);
    editor::mcp::ImportRequest request;
    request.source = String(u8"anything.two");
    request.importer = &importer;

    foundation::mcp::ToolCall first;
    foundation::mcp::ToolCall second;
    Optional<editor::mcp::ImportOutcome> firstDone;
    Optional<editor::mcp::ImportOutcome> secondDone;
    for (u32 i = 0; i < 5000 && !(firstDone.HasValue() && secondDone.HasValue()); ++i)
    {
        if (!firstDone.HasValue())
        {
            editor::mcp::OperationStep<editor::mcp::ImportOutcome> step = ops.Import(first, request);
            REQUIRE(step.HasValue());
            firstDone = Move(step.Value());
            first.isReentry = true;
        }
        if (!secondDone.HasValue())
        {
            editor::mcp::OperationStep<editor::mcp::ImportOutcome> step = ops.Import(second, request);
            REQUIRE(step.HasValue());
            secondDone = Move(step.Value());
            second.isReentry = true;
        }
        bench.Pump();
    }
    REQUIRE(firstDone.HasValue());
    REQUIRE(secondDone.HasValue());
    // Each call ran its own prepare, placement and flush (the one source lands on one asset).
    CHECK(importer.prepares == 2u);
    CHECK(importer.imports == 2u);
    CHECK(firstDone.Value().deferredWrites == 1u);
    CHECK(secondDone.Value().deferredWrites == 1u);
    content::Instance* instance = bench.project->SourceDb().GetInstance(secondDone.Value().guid);
    REQUIRE(instance != nullptr);
    CHECK(instance->ReadData(u8"bulk").Get() != nullptr);

    // A call whose caller leaves mid-import ends: its state goes, the job finishes on its own.
    {
        foundation::mcp::ToolCall left;
        editor::mcp::OperationStep<editor::mcp::ImportOutcome> step = ops.Import(left, request);
        REQUIRE(step.HasValue());
        CHECK_FALSE(step.Value().HasValue()); // preparing on the worker
    }
    for (u32 i = 0; i < 50; ++i)
    {
        bench.Pump(); // the job lands with no call to read it, harmlessly
    }
}

TEST_CASE("editor-operations: an export cooks first, then runs the export job, and the job's "
          "failure reaches the re-entered call")
{
    Bench bench(u8"mcp_ops_export");
    app::EditorProjectOperations ops(bench.Seams());
    ExportPresetSet presets;
    DefaultExportPresets(presets);
    REQUIRE(presets.presets.Size() > 0u);
    editor::mcp::ExportRequest request;
    request.preset = presets.presets[0];
    request.outRoot = String(u8"mcp_ops_export/Dist");

    u32 waited = 0;
    editor::mcp::OperationStep<editor::mcp::ExportOutcome> step =
        Drive<editor::mcp::ExportOutcome>(bench, [&](foundation::mcp::ToolCall& call) { return ops.Export(call, request); }, waited);
    // No template is installed here, so the job fails, after the cook has landed and the job has run.
    REQUIRE_FALSE(step.HasValue());
    CHECK(step.Error().AsView().StartsWith(u8"export of preset '"));
    CHECK(waited >= 2u);
    CHECK(bench.cook.Revision() == 1u); // the export's cook went through the cook service
    CHECK_FALSE(bench.jobs.IsBusy());
}

// asset_create on the editor waits while a cook or an export holds the databases, then creates the
// asset under the exact name given (or refuses a taken name) and runs the host's after-create
// effects.
TEST_CASE("editor-operations: a creation waits for the cook gate, then creates and runs the "
          "host's effects")
{
    Bench bench(u8"mcp_ops_create");
    bool locked = true;
    bench.cook.ExternalMutationLock = [&locked]() { return locked; };
    usize effects = 0;
    app::EditorProjectOperationsSeams seams = bench.Seams();
    seams.onCreated = [&effects](const pipeline::AssetCreator&, content::Instance&) { ++effects; };
    app::EditorProjectOperations ops(Move(seams));

    pipeline::AssetCreator creator;
    creator.label = String(u8"Options");
    creator.type = &pipeline::ImportOptions::StaticType();
    creator.defaultGroup = String(u8"Settings");
    creator.run = [](const pipeline::AssetCreationContext& context)
    {
        pipeline::ImportOptions options;
        return pipeline::CreateWrittenInstance(context.Target(), context.NameOr(u8"Options"),
                                               pipeline::ImportOptions::StaticType(), options);
    };
    editor::mcp::CreateRequest request;
    request.creator = &creator;
    request.name = String(u8"Exact");

    // Held: not finished, nothing written.
    foundation::mcp::ToolCall held;
    editor::mcp::OperationStep<editor::mcp::CreateOutcome> step = ops.Create(held, request);
    REQUIRE(step.HasValue());
    CHECK_FALSE(step.Value().HasValue());
    CHECK(effects == 0u);

    // Released: created under the exact name in the creator's group, with the effects.
    locked = false;
    held.isReentry = true;
    step = ops.Create(held, request);
    REQUIRE(step.HasValue());
    REQUIRE(step.Value().HasValue());
    CHECK(step.Value().Value().path == u8"Settings/Exact");
    CHECK(effects == 1u);

    // The same exact name again is refused, and no effect runs.
    foundation::mcp::ToolCall again;
    step = ops.Create(again, request);
    REQUIRE_FALSE(step.HasValue());
    CHECK(step.Error().AsView().ContainsIgnoreCase(u8"already exists"));
    CHECK(effects == 1u);
}
