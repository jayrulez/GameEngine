// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_import partition
//
// Importing files into the project: one import of one or many files (a drop, Import...) into one
// group. Each file gets the importers that accept its extension, with the first selected; a file
// no importer accepts is reported and left out. Each file also gets its options and a review plan:
// what the import would create, with each resource kept or dropped and named. A light importer
// describes the file at once; a heavy one reads it on a worker first. The previous import's
// choices for the file in that group are laid over the plan. The import waits, saying why, until
// every kept file is read and described; then each is imported (its bulk writes flushed on a
// worker), the editor is notified, and the group's assets are cooked. Nothing changes the
// databases while a cook reads them, so an import waits for a running cook and says so. A name
// another asset holds (pipeline::FindImportClash) is reported on the file: an asset of another
// type must be renamed, while one of the same type from another file is replaced or kept
// alongside, as the user chooses. The import window is the view over this.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:asset_import;

import foundation.core;
import foundation.content;
import editor.core;
import pipeline.importer;
import :group_picker_dialog; // GroupPicker::ShownName

using namespace foundation::core;

export namespace editor::app
{
    namespace content = foundation::content;

    class AssetImport
    {
    public:
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr; // null: nothing waits (a test)
            editor::EditorJobService* jobs = nullptr;  // null: everything on this thread (a test)
        };

        /// One file of an import.
        struct File
        {
            enum class State : u8
            {
                Reading, ///< a heavy importer reads it on a worker
                Ready,   ///< described: its plan shows what the import would create
                Failed,  ///< it could not be read
            };
            String path;
            Array<pipeline::IFileImporter*> importers; ///< the importers that accept its extension
            usize importer = 0;                        ///< the one chosen
            RefPtr<pipeline::ImportOptions> options;   ///< its options (a bare one when it has none)
            RefPtr<Object> prepared;                   ///< what a worker read, for the import
            pipeline::ImportPlan plan;                 ///< what it would create, with what is kept and the names
            bool kept = true;                          ///< included in the import
            /// What the import does when another file's asset of the same type holds its name
            /// (ImportClash); Refuse until the user chooses.
            pipeline::ImportConflictChoice onConflict = pipeline::ImportConflictChoice::Refuse;
            State state = State::Reading;
            u64 generation = 0;                        ///< a read for an importer that has since been replaced is discarded

            [[nodiscard]] pipeline::IFileImporter* Importer() const { return importer < importers.Size() ? importers[importer] : nullptr; }
            [[nodiscard]] StringView Name() const { return pipeline::FileNameOf(path.AsView()); }
        };

        /// One import: its files and the group they go in. The window holds it while it is shown.
        class Session : public RefCounted
        {
        public:
            Array<File> files;
            content::Group* group = nullptr;
            String ledgerPath; ///< the project's import ledger (pipeline::ImportLedgerPath)
            /// Called when a file's state or plan changes (a worker's read arrived), so its view
            /// can show it again.
            Function<void(usize)> onChanged;
            bool closed = false; // the window has closed: a read that arrives later is discarded
        };

        explicit AssetImport(Services services) : m_services(Move(services)) {}

        /// Starts an import of `paths` into `into`. Each file gets the importers that accept it (a
        /// file none accepts is reported and left out) and is described or starts being read.
        /// Null when no file is left.
        RefPtr<Session> Begin(Span<const String> paths, content::Group& into)
        {
            RefPtr<Session> session = MakeRef<Session>(Context().Importers().Allocator());
            session->group = &into;
            if (Project() != nullptr)
                session->ledgerPath = pipeline::ImportLedgerPath(Project()->CacheRoot().AsView());
            for (const String& path : paths)
            {
                Array<pipeline::IFileImporter*> importers = Context().Importers().FindAllFor(pipeline::FileExtensionLower(path.AsView()).AsView());
                if (importers.IsEmpty())
                {
                    Context().Notify(editor::NoticeKind::Warning, Format(u8"Nothing imports {}.", pipeline::FileNameOf(path.AsView())).AsView());
                    continue;
                }
                File file;
                file.path = path;
                file.importers = Move(importers);
                session->files.PushBack(Move(file));
            }
            if (session->files.IsEmpty())
                return {};
            for (usize i = 0; i < session->files.Size(); ++i)
                Describe(*session, i);
            return session;
        }

        /// Whether `session` needs no review: a single file whose importer has no options and
        /// plans nothing (a script, a UI document), which is imported at once.
        [[nodiscard]] static bool NeedsNoReview(const Session& session)
        {
            if (session.files.Size() != 1)
                return false;
            const File& file = session.files[0];
            return file.state == File::State::Ready && file.plan.IsEmpty() && file.options && file.options->Toggles().IsEmpty() &&
                   file.importers.Size() == 1 && pipeline::ImportClashAllows(ClashOf(session, file), pipeline::ImportConflictChoice::Refuse);
        }

        /// The name clash a file's import would meet in the session's group, under its plan's
        /// names (none while it is being read).
        [[nodiscard]] static pipeline::ImportClash ClashOf(const Session& session, const File& file)
        {
            if (file.state != File::State::Ready || file.Importer() == nullptr || session.group == nullptr)
                return {};
            return file.Importer()->FindClash(*session.group, file.path.AsView(), file.plan, session.ledgerPath.AsView());
        }

        /// Chooses another importer for a file: its options start fresh and it is described again.
        void SetImporter(Session& session, usize file, usize importer)
        {
            if (file >= session.files.Size() || importer >= session.files[file].importers.Size() || session.files[file].importer == importer)
                return;
            session.files[file].importer = importer;
            Describe(session, file);
        }
        /// Moves the import to another group, laying a previous import's choices there over each plan.
        void SetGroup(Session& session, content::Group& group)
        {
            session.group = &group;
            for (File& file : session.files)
                if (file.state == File::State::Ready && file.Importer() != nullptr)
                    pipeline::MergeStoredSelection(file.plan, file.Importer()->StoredSelection(group, file.path.AsView()));
        }

        /// Whether the import can run now; when not, why (nothing kept, a file still being read).
        [[nodiscard]] static bool CanImport(const Session& session, String& why)
        {
            usize kept = 0;
            for (const File& file : session.files)
            {
                if (!file.kept)
                    continue;
                ++kept;
                if (file.state == File::State::Reading)
                {
                    why = Format(u8"Reading {}...", file.Name());
                    return false;
                }
                if (file.state == File::State::Failed)
                {
                    why = Format(u8"{} could not be read: leave it out.", file.Name());
                    return false;
                }
                const pipeline::ImportClash clash = ClashOf(session, file);
                if (!pipeline::ImportClashAllows(clash, file.onConflict))
                {
                    why = pipeline::DescribeImportClash(clash);
                    return false;
                }
            }
            if (kept == 0)
            {
                why = String(u8"No file is kept.");
                return false;
            }
            why = session.files.Size() == 1 ? Format(u8"Into {}.", GroupPicker::ShownName(*session.group))
                                            : Format(u8"{} of {} files into {}.", kept, session.files.Size(), GroupPicker::ShownName(*session.group));
            return true;
        }

        /// Imports every kept file with its plan as chosen; false (with the reason reported) when
        /// it cannot yet.
        bool Commit(Session& session)
        {
            String why;
            if (!CanImport(session, why))
                return false;
            for (File& file : session.files)
            {
                if (!file.kept)
                    continue;
                file.options->selection = file.plan;
                ImportOne(file.path, file.Importer(), file.options, file.prepared, *session.group, file.onConflict, session.ledgerPath);
            }
            return true;
        }

    private:
        [[nodiscard]] editor::EditorContext& Context() const { return *m_services.context; }
        [[nodiscard]] editor::EditorProject* Project() const { return m_services.context->Project(); }

        // Describes a file at once (a light importer), or reads it on a worker first (a heavy one).
        void Describe(Session& session, usize index)
        {
            File& file = session.files[index];
            pipeline::IFileImporter* importer = file.Importer();
            file.options = importer->CreateOptions(Context().Importers().Allocator());
            if (!file.options)
                file.options = MakeRef<pipeline::ImportOptions>(Context().Importers().Allocator()); // a bare one carries the plan's choices
            file.prepared = {};
            file.plan = {};
            const u64 generation = ++file.generation;
            if (!importer->WantsWorkerPrepare() || m_services.jobs == nullptr)
            {
                file.plan = importer->DescribeImport(file.path.AsView(), file.options.Get(), nullptr);
                pipeline::MergeStoredSelection(file.plan, importer->StoredSelection(*session.group, file.path.AsView()));
                file.state = File::State::Ready;
                return;
            }
            file.state = File::State::Reading;
            RefPtr<Session> keep(&session);
            auto* holder = Context().Importers().Allocator().New<RefPtr<Object>>();
            IAllocator* allocator = &Context().Importers().Allocator();
            m_services.jobs->Submit(Format(u8"Reading {}", file.Name()).AsView(),
                                    [importer, path = file.path, holder, allocator](editor::JobContext& job) -> Status
                                    {
                                        job.SetStep(u8"loading and decoding", 1, 2);
                                        *holder = importer->PrepareOnWorker(path.AsView(), *allocator);
                                        return holder->Get() != nullptr ? Status{} : Status{ErrorCode::InvalidArgument};
                                    },
                                    [this, keep, index, generation, holder, allocator](Status result)
                                    {
                                        RefPtr<Object> prepared = *holder;
                                        allocator->Delete(holder);
                                        if (keep->closed || index >= keep->files.Size() || keep->files[index].generation != generation)
                                            return; // the window closed, or another importer was chosen meanwhile
                                        File& read = keep->files[index];
                                        if (!result.IsOk())
                                        {
                                            read.state = File::State::Failed;
                                            read.kept = false;
                                            Context().Notify(editor::NoticeKind::Error, Format(u8"{} could not be read (see the Console).", read.Name()).AsView());
                                        }
                                        else
                                        {
                                            read.prepared = prepared;
                                            read.plan = read.Importer()->DescribeImport(read.path.AsView(), read.options.Get(), prepared.Get());
                                            pipeline::MergeStoredSelection(read.plan, read.Importer()->StoredSelection(*keep->group, read.path.AsView()));
                                            read.state = File::State::Ready;
                                        }
                                        if (keep->onChanged)
                                            keep->onChanged(index);
                                    });
        }

        // Imports one file into the group, waiting for a running cook; its bulk writes are flushed
        // on a worker.
        void ImportOne(String path, pipeline::IFileImporter* importer, RefPtr<pipeline::ImportOptions> options, RefPtr<Object> prepared, content::Group& group,
                       pipeline::ImportConflictChoice onConflict, String ledgerPath)
        {
            if (Project() == nullptr)
                return;
            content::Group* into = &group;
            if (m_services.cook != nullptr && m_services.cook->MutationLocked())
            {
                m_services.cook->RunWhenIdle([this, path, importer, options, prepared, into, onConflict, ledgerPath]()
                                             { ImportOne(path, importer, options, prepared, *into, onConflict, ledgerPath); });
                Context().Notify(editor::NoticeKind::Info, u8"The import waits for the cook to finish.");
                return;
            }
            IAllocator& allocator = Context().Importers().Allocator();
            IAllocator* owner = &allocator;
            auto deferred = MakeUnique<Array<pipeline::DeferredImportWrite>>(allocator);
            const Stopwatch clock = Stopwatch::StartNew();
            pipeline::ImportContext context{allocator, Project()->SourcesRoot()};
            context.onConflict = onConflict;
            context.ledgerPath = ledgerPath;
            Result<content::Instance*> imported =
                importer->Import(path.AsView(), context, group, options.Get(), prepared.Get(),
                                 m_services.jobs != nullptr ? deferred.Get() : nullptr);
            LOG_INFO(u8"Import", u8"'{}': {} ms on the main thread, {} deferred write(s)", pipeline::FileNameOf(path.AsView()),
                     static_cast<i64>(clock.Elapsed().AsMilliseconds()), deferred->Size());
            if (!imported.HasValue() || imported.Value() == nullptr)
            {
                Context().Notify(editor::NoticeKind::Error, Format(u8"{} could not be imported (see the Console).", pipeline::FileNameOf(path.AsView())).AsView());
                return;
            }
            content::Instance* primary = imported.Value();
            if (deferred->IsEmpty() || m_services.jobs == nullptr)
            {
                Finish(*primary, *importer, options);
                return;
            }
            // Run the bulk writes on a worker (pure file IO; the job keeps cooks out), keeping the
            // prepared payload alive for the writes that borrow from it.
            auto* writes = deferred.Release();
            const Guid primaryId = primary->Id();
            m_services.jobs->Submit(Format(u8"Writing {}", pipeline::FileNameOf(path.AsView())).AsView(),
                                    [writes, prepared](editor::JobContext& job) -> Status
                                    {
                                        Status result{};
                                        for (usize i = 0; i < writes->Size(); ++i)
                                        {
                                            pipeline::DeferredImportWrite& write = (*writes)[i];
                                            job.SetStep(write.Label(), i + 1, writes->Size());
                                            job.SetFraction(static_cast<f32>(i) / static_cast<f32>(writes->Size()));
                                            if (const Status s = write.Execute(); !s.IsOk())
                                            {
                                                LOG_ERROR(u8"Import", u8"a deferred write failed: '{}'", write.Label());
                                                result = s;
                                            }
                                        }
                                        (void)prepared;
                                        return result;
                                    },
                                    [this, writes, importer, options, primaryId, owner](Status result)
                                    {
                                        owner->Delete(writes);
                                        content::Instance* written = Project() != nullptr ? Project()->SourceDb().GetInstance(primaryId) : nullptr;
                                        if (!result.IsOk() || written == nullptr)
                                        {
                                            Context().Notify(editor::NoticeKind::Error, u8"The import's data could not be written (see the Console).");
                                            return;
                                        }
                                        Finish(*written, *importer, options);
                                    });
        }
        // Reports the import, notifies the editor (its pages, an agent) and cooks the group's
        // assets. A re-import of the same bytes copies nothing a file watcher would notice.
        void Finish(content::Instance& primary, pipeline::IFileImporter& importer, const RefPtr<pipeline::ImportOptions>& options)
        {
            Context().Notify(editor::NoticeKind::Success, Format(u8"Imported {} ({}).", primary.Name(), importer.Label()).AsView());
            Context().NotifyImported(primary, options.Get());
            if (m_services.cook == nullptr)
                return;
            Array<Guid> ids;
            for (const content::Instance* instance : primary.OwningGroup().Instances())
                ids.PushBack(instance->Id());
            m_services.cook->RequestCookFor(Move(ids), false);
        }

        Services m_services;
    };
}
