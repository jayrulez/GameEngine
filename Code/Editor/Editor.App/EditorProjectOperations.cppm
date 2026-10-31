// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_operations partition.
//
// EditorProjectOperations: the editor host's IProjectOperations. The cook goes through the cook
// service (requested, then awaited by revision). The import runs the editor's two-phase path: the
// worker's prepare and the deferred-write flush run on the job service, with the cheap
// main-thread fan-out between them, held while a cook reads the databases. The export cooks
// through the cook service and then runs the one export entry point on the job service. These are
// the paths the menus take, so the editor stays live while an agent's call waits. Every step is
// re-entered from the host's pump: the first call starts the work, later calls poll it, and a
// step that outlives the timeout answers with an error instead of waiting forever.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_operations;

import foundation.core;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import editor.core;
import foundation.mcp;
import editor.mcp;

using namespace foundation::core;

export namespace editor::app
{
    /// What the operations run on: the application's services for the open project. Every
    /// pointer is borrowed for the project's life; the strings are resolved on the main thread.
    struct EditorProjectOperationsSeams
    {
        IAllocator* allocator = nullptr;
        editor::EditorProject* project = nullptr;
        editor::EditorContext* context = nullptr; ///< SceneStreamStager / SceneRefScanner, if wired
        editor::EditorCookService* cook = nullptr;
        editor::EditorJobService* jobs = nullptr;
        pipeline::BuilderRegistry* builders = nullptr;
        String hostToolDir;   ///< the player and sidecars beside the executable (the host template)
        String templatesRoot; ///< the resolved templates root (resolving reads settings, so main thread only)
        String dataRoot;      ///< the engine data root (the shader cook reads <dataRoot>/Shaders)
        f64 timeoutSeconds = 600.0; ///< a step still running past this answers with an error
        /// Called after a creation for the editor effects File > New has (a first scene becomes
        /// the default, the browser updates, the cook runs).
        Function<void(const pipeline::AssetCreator&, foundation::content::Instance&)> onCreated;
        /// Called after an import for the editor effects an import from the Assets browser has
        /// (the import listeners, including the one that makes a model's prefab; the cook of what
        /// it made; the browser).
        Function<void(foundation::content::Instance& primary,
                      const pipeline::ImportOptions* options)>
            onImported;
        /// An agent's delete, done as the browser does it (the asset's page closed, the default
        /// scene and the browser kept up to date). Unset falls back to the database's delete.
        Function<bool(const Guid&)> onDelete;
    };

    class EditorProjectOperations final : public editor::mcp::IProjectOperations
    {
    public:
        explicit EditorProjectOperations(EditorProjectOperationsSeams seams)
            : m_seams(Move(seams))
        {
        }

        // Each call keeps its own progress in the call's state, so two agents' imports (or cooks,
        // or exports) can run side by side. A job still running when its call ends (its caller
        // left) keeps its own reference to the state it shares.
        [[nodiscard]] editor::mcp::OperationStep<editor::mcp::CookOutcome>
        Cook(foundation::mcp::ToolCall& call, bool force) override;
        [[nodiscard]] editor::mcp::OperationStep<editor::mcp::ImportOutcome>
        Import(foundation::mcp::ToolCall& call, const editor::mcp::ImportRequest& request) override;
        [[nodiscard]] editor::mcp::OperationStep<editor::mcp::ExportOutcome>
        Export(foundation::mcp::ToolCall& call, const editor::mcp::ExportRequest& request) override;
        /// Runs on the main thread, never while a cook or an export reads the databases (it waits,
        /// as an import's placement does); then applies the host's effects through onCreated.
        [[nodiscard]] editor::mcp::OperationStep<editor::mcp::CreateOutcome>
        Create(foundation::mcp::ToolCall& call, const editor::mcp::CreateRequest& request) override;
        /// Runs on the main thread, never while a cook or an export reads the databases (it waits,
        /// as a creation does); then deletes as the browser does, through onDelete.
        [[nodiscard]] editor::mcp::OperationStep<bool> Delete(foundation::mcp::ToolCall& call,
                                                              const Guid& id) override;

    private:
        /// A cook requested and awaited. It is finished once the service's revision has moved past
        /// the one seen at the request and the service is idle again (a request that arrived
        /// mid-cook is remembered by the service and runs one cook later).
        struct CookWait final : foundation::mcp::ToolCallState
        {
            bool active = false;
            u64 sinceRevision = 0;
            u64 startedTicks = 0;
        };
        /// The state a job shares with the lambdas it hands the job service: ref-counted so a
        /// job still running when these operations go away keeps its state alive.
        class ImportShared final : public RefCounted
        {
        public:
            RefPtr<Object> prepared; ///< the worker's payload, alive through the flush
            UniquePtr<Array<pipeline::DeferredImportWrite>> writes;
            i64 prepareMs = 0; ///< written by the worker before completion, read on the main thread after it
            i64 flushMs = 0;
            bool jobDone = false; ///< set on the main thread by the job's completion
            Status jobStatus;
        };
        struct ImportJob final : foundation::mcp::ToolCallState
        {
            enum class Phase : u8
            {
                Idle,
                Preparing,
                Placing,
                Flushing
            };
            Phase phase = Phase::Idle;
            u64 startedTicks = 0;
            i64 mainMs = 0;
            RefPtr<ImportShared> shared;
            editor::mcp::ImportOutcome outcome;
        };
        class ExportShared final : public RefCounted
        {
        public:
            editor::ExportPreset preset;
            String outRoot;
            HashMap<Guid, Array<byte>> sceneStreams;
            Array<Guid> reachableRoots;
            bool reachableValid = false;
            editor::ExportResult result;
            bool jobDone = false;
            Status jobStatus;
        };
        struct ExportJob final : foundation::mcp::ToolCallState
        {
            enum class Phase : u8
            {
                Idle,
                Cooking,
                Running
            };
            Phase phase = Phase::Idle;
            u64 startedTicks = 0;
            CookWait cook;
            RefPtr<ExportShared> shared;
        };

        void BeginCook(CookWait& wait, bool force);
        /// True = finished; false = still cooking; an error once the wait outlives the timeout.
        [[nodiscard]] Result<bool, String> PollCook(CookWait& wait);
        [[nodiscard]] editor::mcp::CookOutcome SummaryOutcome() const;
        [[nodiscard]] bool TimedOut(u64 startedTicks) const;
        /// A creation waiting on the cook gate, with the time it first waited.
        struct CreateWait final : foundation::mcp::ToolCallState
        {
            u64 startedTicks = 0;
        };

        /// The call's state of type T, created on its first entry.
        template <typename T>
        [[nodiscard]] T& CallState(foundation::mcp::ToolCall& call)
        {
            if (call.state.Get() == nullptr)
            {
                call.state = MakeUnique<T>(*m_seams.allocator);
            }
            return *call.State<T>();
        }

        void SubmitImportFlush(ImportJob& job, const editor::mcp::ImportRequest& request);
        void SubmitExportJob(ExportJob& job);

        EditorProjectOperationsSeams m_seams;
    };
}
