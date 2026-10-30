// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Mcp - :operations partition
//
// IProjectOperations: how a HOST runs the work behind asset_cook / asset_import / asset_create /
// project_export.
// The tools themselves are shared - their arguments, refusals and result shapes are the same on
// every host - but WHERE the work runs is the host's: the stdio host runs it inline on the
// calling thread (every step answers at once), the editor on its own background services (the
// cook service, the job service) with the tool re-entered each pump until they finish, so the
// editor never blocks on an agent's call. A step that is not finished is an EMPTY Optional; a
// refusal or failure is the error text the agent reads.
//
// Every operation is IDEMPOTENT ACROSS RE-ENTRIES: a not-finished tool is called again with
// the same arguments on the host's next pump, so the first call with a request starts the
// work and later calls with the same request poll it. An implementation keeps that state
// itself, and one operation of each kind is in flight at a time (the tool is one call).
module;
#include "Core/Prelude.h"

export module editor.mcp:operations;

import foundation.core;
import foundation.content;
import pipeline.core; // AssetCreator (asset_create)
import pipeline.importer;
import foundation.mcp; // ToolCall: each call's progress is its own
import editor.project;

using namespace foundation::core;

export namespace editor::mcp
{
    /// A step of an operation: finished with T, not finished yet (empty), or failed/refused
    /// with the text the agent reads.
    template <typename T>
    using OperationStep = Result<Optional<T>, String>;

    /// What asset_cook reports: the plan's split and the build's counts.
    struct CookOutcome
    {
        usize planned = 0;
        usize cooked = 0;
        usize failed = 0;
        usize orphansSwept = 0;
        usize upToDate = 0;
        usize unbuildable = 0;
    };

    /// asset_import's resolved request: the importer is routed by the tool (by extension), the
    /// group path is the agent's (slash-joined; empty = the root).
    struct ImportRequest
    {
        String source;
        String groupPath;
        pipeline::IFileImporter* importer = nullptr;
        /// The importer's options, its defaults with the call's toggles applied; null when the
        /// importer has none.
        RefPtr<pipeline::ImportOptions> options;
        /// What the import does when another file's asset of its type holds the name
        /// (pipeline::ImportConflictChoice): refused unless the call says.
        pipeline::ImportConflictChoice onConflict = pipeline::ImportConflictChoice::Refuse;
    };

    /// What asset_import reports: the created asset's identity and where the time went (the
    /// main-thread share is the number that must stay small in the editor).
    struct ImportOutcome
    {
        Guid guid;
        String name;
        String type;
        String typeNamespace;
        String importer;
        usize deferredWrites = 0;
        i64 prepareMs = 0;
        i64 mainMs = 0;
        i64 flushMs = 0;
    };

    /// project_export's resolved request: the preset the tool resolved by name, the output
    /// root, and whether to re-cook everything first.
    struct ExportRequest
    {
        editor::ExportPreset preset;
        String outRoot;
        bool rebuild = false;
    };

    /// What project_export reports: the one export entry point's result.
    struct ExportOutcome
    {
        editor::ExportResult result;
    };

    /// asset_create's resolved request: the creator the tool found, the group path (made when
    /// missing; empty is the creator's own folder) and the exact name (empty is the creator's
    /// own, made unique). The creator is borrowed from the host's registry.
    struct CreateRequest
    {
        const pipeline::AssetCreator* creator = nullptr;
        String groupPath;
        String name;
    };

    /// What asset_create reports: the new asset's identity and where it landed.
    struct CreateOutcome
    {
        Guid guid;
        String name;
        String type;
        String path;
    };

    /// Every operation takes its tool call: a host that answers over several pumps keeps the
    /// call's progress in the call's state, so two agents' operations run side by side and one
    /// whose caller left ends with its call. A host that answers at once ignores it.
    class IProjectOperations
    {
    public:
        virtual ~IProjectOperations() = default;
        /// The incremental cook over the open project (`force` = rebuild all).
        [[nodiscard]] virtual OperationStep<CookOutcome> Cook(foundation::mcp::ToolCall& call,
                                                              bool force) = 0;
        /// One OS file into the open project's source database (does not cook).
        [[nodiscard]] virtual OperationStep<ImportOutcome> Import(foundation::mcp::ToolCall& call,
                                                                  const ImportRequest& request) = 0;
        /// A shippable dist of the open project through the one export entry point.
        [[nodiscard]] virtual OperationStep<ExportOutcome> Export(foundation::mcp::ToolCall& call,
                                                                  const ExportRequest& request) = 0;
        /// One new asset from a creator (RunAssetCreation), and whatever the host does after a
        /// creation: the editor's cook request and default scene; nothing on the stdio host,
        /// whose agent cooks next.
        [[nodiscard]] virtual OperationStep<CreateOutcome> Create(foundation::mcp::ToolCall& call,
                                                                  const CreateRequest& request) = 0;
        /// One source asset removed (asset_delete): its envelope and data files, never the
        /// original file an import copied under Sources/. The host closes what has it open. The
        /// step's value is set once the asset is gone.
        [[nodiscard]] virtual OperationStep<bool> Delete(foundation::mcp::ToolCall& call, const Guid& id) = 0;
    };

    /// asset_create's work, the same on every host: the group resolved (made when missing), a
    /// taken name refused (an agent that names an asset means that name), the creator run.
    /// Main thread: it writes the source database.
    [[nodiscard]] inline Result<foundation::content::Instance*, String>
    RunAssetCreation(editor::EditorProject& project, const CreateRequest& request)
    {
        foundation::content::Group* root = project.SourceDb().RootGroup();
        foundation::content::Group* picked = nullptr;
        if (!request.groupPath.IsEmpty())
        {
            // A slash-joined path, each part made when missing.
            picked = root;
            usize start = 0;
            const StringView path = request.groupPath.AsView();
            for (usize i = 0; i <= path.Size(); ++i)
            {
                if (i < path.Size() && path[i] != utf8char('/'))
                {
                    continue;
                }
                const StringView part = path.SubStr(start, i - start);
                if (!part.IsEmpty())
                {
                    picked = picked->CreateGroup(part);
                }
                start = i + 1;
            }
        }
        if (!request.name.IsEmpty())
        {
            foundation::content::Group* target = request.creator->TargetFor(picked, root);
            if (target != nullptr && target->GetInstance(request.name.AsView()) != nullptr)
            {
                return Err(Format(u8"an asset named '{}' already exists in '{}'",
                                  request.name.AsView(), target->Path().AsView()));
            }
        }
        const String sourcesRoot = project.SourcesRoot();
        foundation::content::Instance* instance = request.creator->Create(
            picked, root, sourcesRoot.AsView(), request.name.AsView());
        if (instance == nullptr)
        {
            return Err(Format(u8"the {} creator made nothing (a write was refused)",
                              request.creator->label.AsView()));
        }
        return instance;
    }

    /// The outcome asset_create reports for `instance`.
    [[nodiscard]] inline CreateOutcome CreateOutcomeOf(const foundation::content::Instance& instance)
    {
        CreateOutcome outcome;
        outcome.guid = instance.Id();
        outcome.name = String(instance.Name());
        outcome.type = String(instance.TypeName());
        outcome.path = instance.Path();
        return outcome;
    }
}
