// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :assets_view partition.
//
// AssetsView: the Assets panel - source-DB-backed, the typed DB is
// the truth (a database-view model, not a directory scan). Left = group tree; right =
// [breadcrumb | list/grid toggle] over [filter] over the selected group's content. The content
// area shows SUBGROUPS first (double-click descends; the breadcrumb climbs back up), then
// instances; a non-empty filter searches instances across ALL groups. Rows show the name
// (gold when favorited) with a trailing "Type [badge]" meta label - the badge is the cook
// state (cooked/missing/FAILED; builder-less types like scenes show none). Interactions:
//   - double-click: descend into a group / open the instance's editor page
//   - INLINE RENAME everywhere, same mechanics as the scene hierarchy (Sedulous browser
//     parity): slow-click the name / F2 / menu Rename edit in place - instances, subgroup
//     rows, and the group tree alike; double-click stays navigation (never starts an edit)
//   - right-click row: Open / Rename / Duplicate / Cook / Rebuild / Delete (multi-select
//     aware: Ctrl/Shift extend the selection; Delete acts on every selected instance);
//     group rows and tree groups get Open / Rename / Delete Group (recursive, confirmed)
//   - right-click background (list, grid, or the group TREE): New <creator>... / Cook All /
//     Rebuild All
//   - Delete always confirms through a dialog, closes any open page editing the instance
//     first (via OnCloseInstancePage), and logs what was removed.
// The view refreshes off three revisions: source-DB shape (tracked locally via Rebuild calls),
// the cook service's revision (badges after a cook), and the filter text.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

module editor.app;

import foundation.core;
import foundation.content;
import foundation.fonts;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core;
import pipeline.core; // AssetCreator (the New menus)
import foundation.settings;
import :editor_icons;
import :import_dialog;
import :confirm_dialog;
import :group_picker_dialog;
import :layout; // EditorAssetBrowserSettings (per-project list/grid view mode)

using namespace foundation::core;
namespace content = foundation::content;
namespace ui = foundation::ui;

namespace editor::app
{
    void AssetsView::Refresh()
    {
        if (m_cook->Revision() != m_cookRevision)
        {
            m_cookRevision = m_cook->Revision();
            RebuildList();
        }
    }

    void AssetsView::ImportFile(StringView path)
    {
        const String one(path);
        ImportFiles(Span<const String>{&one, 1});
    }

    void AssetsView::ImportFiles(Span<const String> paths)
    {
        if (m_context->Project() == nullptr || Context == nullptr)
        {
            return;
        }
        // Resolve each file's importer candidates; unclaimed files drop with a notice.
        Array<app::BatchImportDialog::FileEntry> files;
        for (const String& path : paths)
        {
            const String ext = pipeline::FileExtensionLower(path.AsView());
            Array<pipeline::IFileImporter*> matches =
                m_context->Importers().FindAllFor(ext.AsView());
            if (matches.IsEmpty())
            {
                String message(u8"No importer for '");
                message += pipeline::FileNameOf(path.AsView());
                message += u8"'.";
                m_context->Notify(editor::NoticeKind::Warning, message.AsView());
                continue;
            }
            app::BatchImportDialog::FileEntry entry;
            entry.path = path;
            entry.candidates = Move(matches);
            files.PushBack(Move(entry));
        }
        if (files.IsEmpty())
        {
            return;
        }
        // A single unambiguous file keeps the focused single-file review (prepare -> plan
        // dialog); everything else - several files, or any ambiguity - is ONE batch session
        // (the import-workflow ruling: one dialog per drop; the per-row importer dropdown
        // replaces the old modal-per-file chooser).
        if (files.Size() == 1 && files[0].candidates.Size() == 1)
        {
            ImportWith(files[0].path.AsView(), files[0].candidates[0]);
            return;
        }
        ShowBatchImport(Move(files));
    }

    void AssetsView::ShowBatchImport(Array<app::BatchImportDialog::FileEntry> files)
    {
        content::Group* group = (m_selectedGroup != nullptr)
                                    ? m_selectedGroup
                                    : m_context->Project()->SourceDb().RootGroup();
        m_importTargetGroup = group;

        for (app::BatchImportDialog::FileEntry& entry : files)
        {
            entry.options = entry.candidates[entry.importerIndex]->CreateOptions(MemoryAllocator());
            if (entry.options.Get() == nullptr)
            {
                // Bare selection carrier so option-less importers still honor renames.
                entry.options = RefPtr<pipeline::ImportOptions>(
                    MakeRef<pipeline::ImportOptions>(MemoryAllocator()).Get());
            }
        }
        auto dialog = MakeRef<app::BatchImportDialog>(MemoryAllocator(), group->Path().AsView(),
                                                      Move(files));
        AssetsView* self = this;
        app::BatchImportDialog* dlg = dialog.Get();
        const RefPtr<app::BatchImportDialog> keepAlive(dlg);

        // Inline describe for light importers (worker-prepare importers land through the
        // jobs below instead - describing them inline would stall the UI on a full parse).
        content::Group* describeGroup = group; // re-import memory reads the initial target
        dialog->DescribeFile = [describeGroup](app::BatchImportDialog::FileEntry& entry)
        {
            pipeline::IFileImporter* importer = entry.candidates[entry.importerIndex];
            if (!importer->WantsWorkerPrepare())
            {
                entry.plan =
                    importer->DescribeImport(entry.path.AsView(), entry.options.Get(), nullptr);
                pipeline::MergeStoredSelection(
                    entry.plan, importer->StoredSelection(*describeGroup, entry.path.AsView()));
                entry.described = true;
            }
        };
        dialog->DescribeAll(); // describes inline files AND refreshes the detail + Import gate

        // Worker prepares queue on the job service and stream into the open dialog.
        for (usize i = 0; i < dialog->Files().Size(); ++i)
        {
            app::BatchImportDialog::FileEntry& entry = dialog->Files()[i];
            pipeline::IFileImporter* importer = entry.candidates[entry.importerIndex];
            if (!importer->WantsWorkerPrepare() || m_jobs == nullptr)
            {
                continue;
            }
            String title(u8"Reading ");
            title += pipeline::FileNameOf(entry.path.AsView());
            auto* holder = MemoryAllocator().New<RefPtr<Object>>();
            m_jobs->Submit(
                title.AsView(),
                Function<Status(editor::JobContext&)>{
                    [self, importer, file = entry.path, holder](editor::JobContext& job) -> Status
                    {
                        job.SetStep(u8"loading + decoding", 1, 2);
                        *holder = importer->PrepareOnWorker(file.AsView(), self->MemoryAllocator());
                        return (holder->Get() != nullptr) ? Status{}
                                                          : Status{ErrorCode::InvalidArgument};
                    }},
                Function<void(Status)>{
                    [self, keepAlive, i, importer, holder](Status result)
                    {
                        RefPtr<Object> prepared = *holder;
                        self->MemoryAllocator().Delete(holder);
                        if (i >= keepAlive->Files().Size())
                        {
                            return;
                        }
                        app::BatchImportDialog::FileEntry& e = keepAlive->Files()[i];
                        // The user may have switched this row's importer while the job ran -
                        // a stale payload must not describe the wrong importer.
                        if (e.candidates[e.importerIndex] != importer)
                        {
                            return;
                        }
                        if (!result.IsOk())
                        {
                            e.enabled = false; // unreadable: drop it from the commit
                            String message(u8"Import failed: '");
                            message += pipeline::FileNameOf(e.path.AsView());
                            message += u8"' (see Console).";
                            self->m_context->Notify(editor::NoticeKind::Error, message.AsView());
                        }
                        else
                        {
                            e.prepared = prepared;
                            e.plan = importer->DescribeImport(e.path.AsView(), e.options.Get(),
                                                             prepared.Get());
                            if (self->m_importTargetGroup != nullptr)
                            {
                                pipeline::MergeStoredSelection(
                                    e.plan, importer->StoredSelection(
                                                *self->m_importTargetGroup, e.path.AsView()));
                            }
                            e.described = true;
                        }
                        keepAlive->OnFilePrepared(i);
                    }});
        }

        dialog->OnChangeDestination = [self, dlg]()
        {
            if (self->m_context->Project() == nullptr || self->Context == nullptr)
            {
                return;
            }
            content::Group* root = self->m_context->Project()->SourceDb().RootGroup();
            auto picker = MakeRef<GroupPickerDialog>(self->MemoryAllocator(),
                                                     StringView(u8"Select destination group"),
                                                     root, self->m_importTargetGroup);
            picker->OnPicked = [self, dlg](content::Group* g)
            {
                if (g != nullptr)
                {
                    self->m_importTargetGroup = g;
                    dlg->SetDestination(g->Path().AsView());
                }
            };
            picker->Show(self->Context);
        };
        dialog->OnImport = [self, dlg]()
        {
            for (app::BatchImportDialog::FileEntry& entry : dlg->Files())
            {
                if (!entry.enabled || !entry.described)
                {
                    continue;
                }
                if (entry.options.Get() != nullptr)
                {
                    entry.options->selection =
                        static_cast<pipeline::ImportPlan&&>(entry.plan);
                }
                self->CommitImport(entry.path, entry.candidates[entry.importerIndex],
                                   entry.options, entry.prepared);
            }
        };
        dialog->Show(Context);
    }

    void AssetsView::ImportWith(StringView path, pipeline::IFileImporter* importer)
    {
        if (m_context->Project() == nullptr || Context == nullptr || importer == nullptr)
        {
            return;
        }
        // Default this import to the active group (fresh each time so a cancelled prior import can't
        // leave a stale target); the dialog's Change... picker may retarget it before Import.
        content::Group* group = (m_selectedGroup != nullptr)
                                    ? m_selectedGroup
                                    : m_context->Project()->SourceDb().RootGroup();
        m_importTargetGroup = group;

        RefPtr<pipeline::ImportOptions> options = importer->CreateOptions(MemoryAllocator());

        // Review-capable importers (DescribeImport) invert the order: the slow parse runs
        // FIRST (worker), the dialog then lists every resource the import would create
        // (check/uncheck + rename), and commit reuses the prepared payload - no second load.
        if (importer->WantsWorkerPrepare() && m_jobs != nullptr)
        {
            if (options.Get() == nullptr)
            {
                // Bare selection carrier - the review dialog's renames travel on the base.
                options = RefPtr<pipeline::ImportOptions>(
                    MakeRef<pipeline::ImportOptions>(MemoryAllocator()).Get());
            }
            String title(u8"Reading ");
            title += pipeline::FileNameOf(path);
            auto* holder = MemoryAllocator().New<RefPtr<Object>>();
            AssetsView* self = this;
            m_jobs->Submit(
                title.AsView(),
                Function<Status(editor::JobContext&)>{
                    [self, importer, file = String(path), holder](editor::JobContext& job) -> Status
                    {
                        job.SetStep(u8"loading + decoding", 1, 2);
                        *holder = importer->PrepareOnWorker(file.AsView(), self->MemoryAllocator());
                        return (holder->Get() != nullptr) ? Status{}
                                                          : Status{ErrorCode::InvalidArgument};
                    }},
                Function<void(Status)>{
                    [self, file = String(path), importer, options, holder](Status result)
                    {
                        RefPtr<Object> prepared = *holder;
                        self->MemoryAllocator().Delete(holder);
                        if (!result.IsOk())
                        {
                            String message(u8"Import failed: '");
                            message += pipeline::FileNameOf(file.AsView());
                            message += u8"' (see Console).";
                            self->m_context->Notify(editor::NoticeKind::Error, message.AsView());
                            return;
                        }
                        self->ShowImportReview(file, importer, options, prepared);
                    }});
            return;
        }

        // Inline importers: describe here (cheap parse). Importers with NO options and NO
        // plan (script/UI stubs) still import immediately; everything else opens the review
        // dialog - per the one-dialog ruling, even a single texture gets its rename row.
        pipeline::ImportPlan plan = importer->DescribeImport(path, options.Get(), nullptr);
        if (options.Get() == nullptr && plan.IsEmpty())
        {
            ExecuteImport(String(path), importer, {});
            return;
        }
        pipeline::MergeStoredSelection(plan, importer->StoredSelection(*group, path));
        if (options.Get() == nullptr)
        {
            options = RefPtr<pipeline::ImportOptions>(
                MakeRef<pipeline::ImportOptions>(MemoryAllocator()).Get());
        }
        auto dialog = MakeRef<ImportOptionsDialog>(MemoryAllocator(), path,
                                                   group->Path().AsView(), options, Move(plan));
        AssetsView* self = this;
        ImportOptionsDialog* dlg = dialog.Get();
        dialog->OnChangeDestination = [self, dlg]() { self->ShowImportDestinationMenu(*dlg); };
        dialog->OnImport = [self, dlg, file = String(path), importer,
                            opts = RefPtr<pipeline::ImportOptions>(options.Get())]()
        {
            opts->selection = dlg->TakePlan();
            self->ExecuteImport(file, importer, opts);
        };
        dialog->Show(Context);
    }

    void AssetsView::ShowImportReview(const String& path, pipeline::IFileImporter* importer,
                                      RefPtr<pipeline::ImportOptions> options,
                                      RefPtr<Object> prepared)
    {
        if (m_context->Project() == nullptr || Context == nullptr)
        {
            return;
        }
        content::Group* group = (m_importTargetGroup != nullptr)
                                    ? m_importTargetGroup
                                    : m_context->Project()->SourceDb().RootGroup();
        pipeline::ImportPlan plan =
            importer->DescribeImport(path.AsView(), options.Get(), prepared.Get());
        // Re-import memory: a previous import of this source into this group seeds the plan.
        pipeline::MergeStoredSelection(plan, importer->StoredSelection(*group, path.AsView()));
        auto dialog =
            MakeRef<ImportOptionsDialog>(MemoryAllocator(), path.AsView(),
                                         group->Path().AsView(), options, Move(plan));
        AssetsView* self = this;
        ImportOptionsDialog* dlg = dialog.Get();
        dialog->OnChangeDestination = [self, dlg]() { self->ShowImportDestinationMenu(*dlg); };
        dialog->OnImport = [self, dlg, file = path, importer,
                            opts = RefPtr<pipeline::ImportOptions>(options.Get()), prepared]()
        {
            opts->selection = dlg->TakePlan();
            self->CommitImport(file, importer, opts, prepared);
        };
        dialog->Show(Context);
    }

    void AssetsView::ShowImportDestinationMenu(ImportOptionsDialog& dialog)
    {
        if (m_context->Project() == nullptr || Context == nullptr)
        {
            return;
        }
        content::Group* root = m_context->Project()->SourceDb().RootGroup();
        if (root == nullptr)
        {
            return;
        }
        // A proper group-TREE picker (the asset-picker's sibling), not a flat path menu - the
        // current destination is preselected.
        AssetsView* self = this;
        ImportOptionsDialog* dlg = &dialog;
        auto picker = MakeRef<GroupPickerDialog>(MemoryAllocator(),
                                                 StringView(u8"Select destination group"), root,
                                                 m_importTargetGroup);
        picker->OnPicked = [self, dlg](content::Group* g)
        {
            if (g != nullptr)
            {
                self->m_importTargetGroup = g;
                dlg->SetDestination(g->Path().AsView());
            }
        };
        picker->Show(Context);
    }

    void AssetsView::ExecuteImport(String path, pipeline::IFileImporter* importer,
                                   RefPtr<pipeline::ImportOptions> options)
    {
        if (m_context->Project() == nullptr)
        {
            return;
        }
        if (importer->WantsWorkerPrepare() && m_jobs != nullptr)
        {
            String title(u8"Importing ");
            title += pipeline::FileNameOf(path.AsView());
            auto* holder = MemoryAllocator().New<RefPtr<Object>>();
            AssetsView* self = this;
            m_jobs->Submit(title.AsView(),
                           Function<Status(editor::JobContext&)>{
                               [self, importer, path, holder](editor::JobContext& job) -> Status
                               {
                                   job.SetStep(u8"loading + decoding", 1, 2);
                                   *holder = importer->PrepareOnWorker(path.AsView(),
                                                                       self->MemoryAllocator());
                                   return (holder->Get() != nullptr)
                                              ? Status{}
                                              : Status{ErrorCode::InvalidArgument};
                               }},
                           Function<void(Status)>{
                               [self, path, importer, options, holder](Status result)
                               {
                                   RefPtr<Object> prepared = *holder;
                                   self->MemoryAllocator().Delete(holder);
                                   if (!result.IsOk())
                                   {
                                       String message(u8"Import failed: '");
                                       message += pipeline::FileNameOf(path.AsView());
                                       message += u8"' (see Console).";
                                       self->m_context->Notify(editor::NoticeKind::Error,
                                                               message.AsView());
                                       return;
                                   }
                                   self->CommitImport(path, importer, options, prepared);
                               }});
            return;
        }
        CommitImport(path, importer, options, {});
    }

    void AssetsView::CommitImport(String path, pipeline::IFileImporter* importer,
                                  RefPtr<pipeline::ImportOptions> options,
                                  RefPtr<Object> prepared)
    {
        if (m_context->Project() == nullptr)
        {
            return;
        }
        // A cook in flight reads instance pointers snapshotted at plan time - creating
        // instances now is a race. Queue the import; the cook service replays it when idle.
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            m_cook->RunWhenIdle(
                Function<void()>{[self, path, importer, options, prepared]()
                                 { self->CommitImport(path, importer, options, prepared); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"Import queued until the current cook finishes.");
            return;
        }
        // The user's chosen destination (the import dialog's Change... picker), else the active group.
        content::Group* group =
            (m_importTargetGroup != nullptr)
                ? m_importTargetGroup
                : ((m_selectedGroup != nullptr) ? m_selectedGroup
                                                : m_context->Project()->SourceDb().RootGroup());
        auto deferred =
            MakeUnique<Array<pipeline::DeferredImportWrite>>(MemoryAllocator());
        const Stopwatch mainClock = Stopwatch::StartNew();
        Result<content::Instance*> imported =
            importer->Import(path.AsView(),
                             pipeline::ImportContext{MemoryAllocator(), m_context->Project()->SourcesRoot()},
                             *group, options.Get(),
                             prepared.Get(), (m_jobs != nullptr) ? deferred.Get() : nullptr);
        // The number that must stay small: everything the import does on the UI thread. The
        // bulk (mesh conversion, LOD chains, serialization, file copies) is in the deferred
        // writes the worker flushes below.
        LOG_INFO(u8"Import", u8"'{}': {} ms on the main thread, {} deferred write(s) queued",
                 pipeline::FileNameOf(path.AsView()),
                 static_cast<i64>(mainClock.Elapsed().AsMilliseconds()), deferred->Size());
        if (!imported.HasValue() || imported.Value() == nullptr)
        {
            String message(u8"Import failed: '");
            message += pipeline::FileNameOf(path.AsView());
            message += u8"' (see Console).";
            m_context->Notify(editor::NoticeKind::Error, message.AsView());
            Rebuild();
            return;
        }

        content::Instance* primary = imported.Value();
        if (deferred->IsEmpty() || m_jobs == nullptr)
        {
            FinishImport(*primary, importer, options);
            return;
        }

        // Flush the BULK stream writes on the worker (pure mount IO; the job lock keeps
        // cooks out and queues deletes). The prepared payload stays alive - the views
        // borrow its decoded pixels.
        String title(u8"Writing ");
        title += pipeline::FileNameOf(path.AsView());
        AssetsView* self = this;
        auto* writes = deferred.Release();
        const Guid primaryId = primary->Id();
        m_jobs->Submit(
            title.AsView(),
            Function<Status(editor::JobContext&)>{
                [writes, prepared, path](editor::JobContext& job) -> Status
                {
                    Status result{};
                    const Stopwatch flushClock = Stopwatch::StartNew();
                    for (usize i = 0; i < writes->Size(); ++i)
                    {
                        pipeline::DeferredImportWrite& write = (*writes)[i];
                        job.SetStep(write.Label(), i + 1, writes->Size());
                        job.SetFraction(static_cast<f32>(i) / static_cast<f32>(writes->Size()));
                        const Status s = write.Execute();
                        if (!s.IsOk())
                        {
                            LOG_ERROR(u8"Import", u8"deferred write failed: '{}'",
                                      write.Label());
                            result = s;
                        }
                    }
                    (void)prepared; // keeps the decoded pixels alive for the views
                    LOG_INFO(u8"Import", u8"'{}': {} deferred write(s) flushed in {} ms (worker)",
                             pipeline::FileNameOf(path.AsView()), writes->Size(),
                             static_cast<i64>(flushClock.Elapsed().AsMilliseconds()));
                    return result;
                }},
            Function<void(Status)>{
                [self, writes, importer, options, primaryId](Status result)
                {
                    self->MemoryAllocator().Delete(writes);
                    content::Instance* primary =
                        (self->m_context->Project() != nullptr)
                            ? self->m_context->Project()->SourceDb().GetInstance(primaryId)
                            : nullptr;
                    if (!result.IsOk() || primary == nullptr)
                    {
                        self->m_context->Notify(editor::NoticeKind::Error,
                                                u8"Import data write FAILED (see Console).");
                        self->Rebuild();
                        return;
                    }
                    self->FinishImport(*primary, importer, options);
                }});
    }

    void AssetsView::FinishImport(content::Instance& primary,
                                  pipeline::IFileImporter* importer,
                                  const RefPtr<pipeline::ImportOptions>& options)
    {
        String message(u8"Imported '");
        message += primary.Name();
        message += u8"' (";
        message += importer->Label();
        message += u8").";
        m_context->Notify(editor::NoticeKind::Success, message.AsView());
        AfterImport(primary, options.Get());
    }

    void AssetsView::AfterImport(content::Instance& primary, const pipeline::ImportOptions* options)
    {
        m_context->NotifyImported(primary, options);
        // Cook the imported assets explicitly (scoped to the primary's group; the plan
        // skips anything clean). The Sources/ watcher triggers auto-cook off the provenance
        // copy, but a re-import of identical bytes skips that copy, so the watcher never fires
        // and the new instances would stay uncooked without this explicit cook.
        Array<Guid> ids;
        CollectInstanceIds(&primary.OwningGroup(), ids);
        m_cook->RequestCookFor(Move(ids), false);
        Rebuild();
    }

    void AssetsView::RefreshThumbnail(const Guid& id)
    {
        for (usize i = 0; i < m_rows.Size(); ++i)
        {
            if (m_rows[i].group == nullptr && m_rows[i].id == id)
            {
                // Both adapters share the row model; only the attached view holds active
                // (visible) item views, so the other notify is a no-op.
                m_listAdapter->NotifyRangeChanged(static_cast<i32>(i), 1);
                m_gridAdapter->NotifyRangeChanged(static_cast<i32>(i), 1);
                return;
            }
        }
    }

    void AssetsView::Rebuild()
    {
        m_groups.Clear();
        content::Group* root = (m_context->Project() != nullptr)
                                   ? m_context->Project()->SourceDb().RootGroup()
                                   : nullptr;
        if (root != nullptr)
        {
            AddGroupNode(root, 0);
        }
        // Re-validate the selection against the fresh snapshot (the group may be gone).
        bool selectionAlive = false;
        for (const GroupNode& node : m_groups)
        {
            if (node.group == m_selectedGroup)
            {
                selectionAlive = true;
                break;
            }
        }
        if (!selectionAlive)
        {
            m_selectedGroup = root;
        }

        m_tree->SetAdapter(m_treeAdapter.Get());
        ui::FlattenedTreeAdapter* flat = m_tree->FlatAdapter();
        for (usize i = 0; i < m_groups.Size(); ++i)
        {
            if (!m_groups[i].children.IsEmpty())
            {
                flat->Expand(static_cast<i32>(i));
            }
        }
        RebuildList();
    }

    void AssetsView::OnMeasure(ui::BoxConstraints constraints)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Measure(constraints);
        }
        MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
    }

    void AssetsView::OnLayout(f32, f32, f32 width, f32 height)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Layout(0, 0, width, height);
        }
    }

    void AssetsView::ConfigureNameLabel(NameLabel& label, AssetsView& owner)
    {
        label.DoubleClickToEdit.SetValue(false);
        label.ValidateRename = [](StringView name)
        {
            for (usize i = 0; i < name.Size(); ++i)
            {
                const utf8char c = name[i];
                if (c == utf8char('/') || c == utf8char('\\') || c == utf8char(':') ||
                    c == utf8char('*') || c == utf8char('?') || c == utf8char('"') ||
                    c == utf8char('<') || c == utf8char('>') || c == utf8char('|'))
                {
                    return false;
                }
            }
            return true;
        };
        AssetsView* self = &owner;
        NameLabel* raw = &label;
        label.OnRenameCommitted.Add([self, raw](ui::EditableLabel*, StringView newName)
                                    { self->ApplyRename(raw, newName); });
    }

    i32 AssetsView::AddGroupNode(content::Group* group, i32 depth)
    {
        const i32 nodeId = static_cast<i32>(m_groups.Size());
        GroupNode node;
        node.group = group;
        node.depth = depth;
        m_groups.PushBack(Move(node));
        for (content::Group* child : group->Groups())
        {
            const i32 childId = AddGroupNode(child, depth + 1);
            m_groups[static_cast<usize>(nodeId)].children.PushBack(childId);
        }
        return nodeId;
    }

    void AssetsView::SelectGroup(content::Group* group)
    {
        m_selectedGroup = group;
        // Group navigation replaces the search scope: a stale filter here reads as "my
        // group is empty" (the filter searches ALL groups, ignoring the selection).
        if (!m_filter.IsEmpty())
        {
            m_filter.Clear();
            m_filterEdit->SetText(u8"");
        }
        RebuildList();
    }

    void AssetsView::RebuildList()
    {
        m_rows.Clear();
        if (m_context->Project() != nullptr)
        {
            if (m_filter.IsEmpty())
            {
                if (m_selectedGroup != nullptr)
                {
                    for (content::Group* child : m_selectedGroup->Groups())
                    {
                        Row row;
                        row.group = child;
                        m_rows.PushBack(row);
                    }
                    for (content::Instance* instance : m_selectedGroup->Instances())
                    {
                        Row row;
                        row.id = instance->Id();
                        m_rows.PushBack(row);
                    }
                }
            }
            else
            {
                CollectFiltered(m_context->Project()->SourceDb().RootGroup());
            }
        }
        m_list->Selection.ClearSelection();
        m_grid->Selection.ClearSelection();
        m_list->NotifyDataChanged();
        m_gridAdapter->NotifyDataSetChanged();
        UpdateBreadcrumb();
    }

    void AssetsView::CollectFiltered(content::Group* group)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* instance : group->Instances())
        {
            if (MatchesFilter(instance->Name(), m_filter.AsView()))
            {
                Row row;
                row.id = instance->Id();
                m_rows.PushBack(row);
            }
        }
        for (content::Group* child : group->Groups())
        {
            CollectFiltered(child);
        }
    }

    bool AssetsView::MatchesFilter(StringView name, StringView filter)
    {
        if (filter.IsEmpty())
        {
            return true;
        }
        if (name.Size() < filter.Size())
        {
            return false;
        }
        auto lower = [](utf8char c)
        { return (c >= utf8char('A') && c <= utf8char('Z')) ? static_cast<utf8char>(c + 32) : c; };
        for (usize i = 0; i + filter.Size() <= name.Size(); ++i)
        {
            bool match = true;
            for (usize j = 0; j < filter.Size(); ++j)
            {
                if (lower(name[i + j]) != lower(filter[j]))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return true;
            }
        }
        return false;
    }

    const AssetsView::Row* AssetsView::RowAt(i32 position) const
    {
        if (position < 0 || position >= static_cast<i32>(m_rows.Size()))
        {
            return nullptr;
        }
        return &m_rows[static_cast<usize>(position)];
    }

    content::Instance* AssetsView::InstanceAt(i32 position)
    {
        const Row* row = RowAt(position);
        return (row != nullptr && row->group == nullptr) ? Resolve(row->id) : nullptr;
    }

    ui::Drawable* AssetsView::RowIcon(i32 position)
    {
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return nullptr;
        }
        EditorIcons& icons = EditorIcons::Get();
        if (row->group != nullptr)
        {
            return icons.folder.Get();
        }
        content::Instance* instance = Resolve(row->id);
        return (instance != nullptr) ? icons.ForAssetType(instance->TypeName()) : nullptr;
    }

    void AssetsView::RowName(i32 position, String& text, Color& color)
    {
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return;
        }
        if (row->group != nullptr)
        {
            text.Append(row->group->Name());
            color = Color{0.85f, 0.75f, 0.5f, 1.0f};
            return;
        }
        content::Instance* instance = Resolve(row->id);
        if (instance == nullptr)
        {
            return;
        }
        text.Append(instance->Name());
        switch (m_cook->BadgeFor(*instance))
        {
        case editor::CookBadge::Cooked:
            color = Color{0.6f, 0.9f, 0.6f, 1.0f};
            break;
        case editor::CookBadge::Missing:
            color = Color{0.95f, 0.85f, 0.5f, 1.0f};
            break;
        case editor::CookBadge::Failed:
            color = Color{1.0f, 0.45f, 0.45f, 1.0f};
            break;
        case editor::CookBadge::NoBuilder:
            break;
        }
    }

    void AssetsView::RowMeta(i32 position, String& text, Color& color)
    {
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return;
        }
        if (row->group != nullptr)
        {
            text.Append(u8"Group");
            return;
        }
        content::Instance* instance = Resolve(row->id);
        if (instance == nullptr)
        {
            return;
        }
        text.Append(instance->TypeName());
        switch (m_cook->BadgeFor(*instance))
        {
        case editor::CookBadge::Cooked:
            text.Append(u8"  [cooked]");
            color = Color{0.6f, 0.9f, 0.6f, 1.0f};
            break;
        case editor::CookBadge::Missing:
            text.Append(u8"  [not cooked]");
            color = Color{0.95f, 0.85f, 0.5f, 1.0f};
            break;
        case editor::CookBadge::Failed:
            text.Append(u8"  [FAILED]");
            color = Color{1.0f, 0.45f, 0.45f, 1.0f};
            break;
        case editor::CookBadge::NoBuilder:
            break;
        }
    }

    void AssetsView::ActivateRow(i32 position, i32 clickCount)
    {
        if (clickCount < 2)
        {
            return;
        }
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return;
        }
        if (row->group != nullptr)
        {
            SelectGroup(row->group);
            return;
        }
        if (content::Instance* instance = Resolve(row->id))
        {
            if (OnOpenInstance)
            {
                OnOpenInstance(*instance);
            }
        }
    }

    void AssetsView::UpdateBreadcrumb()
    {
        // Root -> selected group as clickable segments ("Content / models / fox").
        Array<content::Group*> chain;
        for (content::Group* g = m_selectedGroup; g != nullptr; g = g->Parent())
        {
            chain.PushBack(g);
        }
        m_breadcrumbGroups.Clear();
        Array<StringView> segments;
        for (usize i = chain.Size(); i > 0; --i)
        {
            content::Group* g = chain[i - 1];
            m_breadcrumbGroups.PushBack(g);
            segments.PushBack(g->Parent() == nullptr ? StringView(u8"Content") : g->Name());
        }
        m_breadcrumb->SetSegments(Span<StringView>{segments.Data(), segments.Size()});
    }

    void AssetsView::Reveal(const Guid& id)
    {
        content::Instance* instance = Resolve(id);
        if (instance == nullptr)
        {
            return;
        }
        SelectGroup(&instance->OwningGroup()); // navigate + rebuild the row list
        for (usize i = 0; i < m_rows.Size(); ++i)
        {
            if (m_rows[i].group == nullptr && m_rows[i].id == id)
            {
                const i32 position = static_cast<i32>(i);
                if (m_gridMode)
                {
                    m_grid->Selection.Select(position);
                    m_grid->ScrollToPosition(position);
                }
                else
                {
                    m_list->Selection.Select(position);
                    m_list->ScrollToPosition(position);
                }
                return;
            }
        }
    }

    void AssetsView::NavigateToBreadcrumb(i32 segment)
    {
        if (segment >= 0 && segment < static_cast<i32>(m_breadcrumbGroups.Size()))
        {
            SelectGroup(m_breadcrumbGroups[static_cast<usize>(segment)]);
        }
    }

    void AssetsView::SetGridMode(bool grid, bool persist)
    {
        m_gridMode = grid;
        m_viewToggles->SetGridMode(grid);
        m_list->Visibility = grid ? ui::VisibilityValue::Gone : ui::VisibilityValue::Visible;
        m_grid->Visibility = grid ? ui::VisibilityValue::Visible : ui::VisibilityValue::Gone;
        Invalidate();
        if (persist)
        {
            // Remember the choice per project so the browser reopens in the last-used view.
            if (foundation::settings::Settings* store = m_context->ProjectEditorSettings())
            {
                store->Section<EditorAssetBrowserSettings>().gridMode = grid;
                store->MarkChanged<EditorAssetBrowserSettings>();
                m_context->RequestProjectEditorSettingsSave();
            }
        }
    }

    void AssetsView::ApplySavedViewMode()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return; // no project store yet -> keep the default (list)
        }
        if (const EditorAssetBrowserSettings* section = store->Find<EditorAssetBrowserSettings>())
        {
            SetGridMode(section->gridMode, /*persist*/ false); // applying saved state must not re-save
        }
    }

    Array<Guid> AssetsView::SelectedInstanceIds(ui::SelectionModel* selection, i32 clicked)
    {
        Array<Guid> ids;
        auto push = [&](i32 position)
        {
            const Row* row = RowAt(position);
            if (row == nullptr || row->group != nullptr)
            {
                return;
            }
            for (const Guid& existing : ids)
            {
                if (existing == row->id)
                {
                    return;
                }
            }
            ids.PushBack(row->id);
        };
        if (selection != nullptr && selection->IsSelected(clicked))
        {
            for (i32 position : selection->SelectedPositions())
            {
                push(position);
            }
        }
        else
        {
            push(clicked);
        }
        return ids;
    }

    void AssetsView::ShowRowMenu(ui::View* anchor, ui::SelectionModel* selection, i32 position,
                                 f32 x, f32 y)
    {
        if (Context == nullptr)
        {
            return;
        }
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return;
        }
        AssetsView* self = this;

        // Group rows: navigate / rename in place / delete (recursive, confirmed).
        if (row->group != nullptr)
        {
            content::Group* group = row->group;
            auto menu = MakeRef<ui::ContextMenu>(MemoryAllocator());
            menu->AddItem(u8"Open", [self, group]() { self->SelectGroup(group); });
            menu->AddSeparator();
            menu->AddItem(u8"Rename", [self, position]() { self->StartRenameDeferred(position); });
            menu->AddItem(IsGroupExportRoot(group) ? StringView(u8"Don't always export contents")
                                                   : StringView(u8"Always export contents"),
                          [self, group]() { self->ToggleGroupExportRoot(group); });
            menu->AddSeparator();
            menu->AddItem(u8"Cook Group",
                          [self, group]()
                          {
                              Array<Guid> ids;
                              CollectInstanceIds(group, ids);
                              self->m_cook->RequestCookFor(Move(ids), false);
                          });
            menu->AddItem(u8"Rebuild Group",
                          [self, group]()
                          {
                              Array<Guid> ids;
                              CollectInstanceIds(group, ids);
                              self->m_cook->RequestCookFor(Move(ids), true);
                          });
            menu->AddSeparator();
            menu->AddItem(u8"Delete Group", [self, group]() { self->ConfirmDeleteGroup(group); });
            const Float2 screenPos = anchor->LocalToScreen(Float2{x, y});
            menu->Show(Context, screenPos.x, screenPos.y);
            return;
        }

        content::Instance* instance = Resolve(row->id);
        if (instance == nullptr)
        {
            return;
        }
        const Guid id = row->id;
        Array<Guid> targets = SelectedInstanceIds(selection, position);

        auto menu = MakeRef<ui::ContextMenu>(self->MemoryAllocator());
        menu->AddItem(u8"Open",
                      [self, id]()
                      {
                          if (content::Instance* inst = self->Resolve(id))
                          {
                              if (self->OnOpenInstance)
                              {
                                  self->OnOpenInstance(*inst);
                              }
                          }
                      });
        menu->AddSeparator();
        menu->AddItem(u8"Rename", [self, position]() { self->StartRenameDeferred(position); });
        menu->AddItem(u8"Duplicate", [self, id]() { self->DuplicateInstance(id); });
        menu->AddSeparator();
        // OS clipboard (not the editor's typed clipboard): the canonical UUID string / the
        // mount-relative content path, for pasting into scripts (Guid("...")) and docs.
        menu->AddItem(u8"Copy GUID",
                      [self, id]()
                      {
                          utf8char text[37];
                          id.ToChars(text);
                          (void)self->m_context->CopyText(
                              self->Context ? self->Context->Clipboard() : nullptr,
                              StringView(text, 36), u8"GUID");
                      });
        menu->AddItem(u8"Copy Path",
                      [self, id]()
                      {
                          if (content::Instance* inst = self->Resolve(id))
                          {
                              (void)self->m_context->CopyText(
                                  self->Context ? self->Context->Clipboard() : nullptr,
                                  inst->Path().AsView(), u8"path");
                          }
                      });
        menu->AddItem(m_context->IsFavorite(id) ? StringView(u8"Unpin favorite")
                                                : StringView(u8"Pin favorite"),
                      [self, id]()
                      {
                          self->m_context->ToggleFavorite(id);
                          self->RebuildList();
                      });
        menu->AddItem(IsInstanceExportRoot(id) ? StringView(u8"Remove from Always Export")
                                               : StringView(u8"Always Export"),
                      [self, id]() { self->ToggleInstanceExportRoot(id); });
        menu->AddSeparator();
        // Scoped: the selected assets + their dependency closure (Build > Cook All stays
        // the whole-project path) - huge scenes cook one asset/group at a time.
        menu->AddItem(u8"Cook",
                      [self, targets]()
                      {
                          Array<Guid> ids = targets;
                          self->m_cook->RequestCookFor(Move(ids), false);
                      });
        menu->AddItem(u8"Rebuild",
                      [self, targets]()
                      {
                          Array<Guid> ids = targets;
                          self->m_cook->RequestCookFor(Move(ids), true);
                      });
        menu->AddSeparator();
        String deleteLabel(u8"Delete");
        if (targets.Size() > 1)
        {
            deleteLabel += u8" ";
            AppendCount(deleteLabel, targets.Size());
            deleteLabel += u8" assets";
        }
        menu->AddItem(deleteLabel.AsView(), [self, targets]() { self->ConfirmDelete(targets); });
        const Float2 screenPos = anchor->LocalToScreen(Float2{x, y});
        menu->Show(Context, screenPos.x, screenPos.y);
    }

    void AssetsView::ShowBackgroundMenu(ui::View* anchor, f32 x, f32 y)
    {
        if (Context == nullptr)
        {
            return;
        }
        AssetsView* self = this;
        content::Group* target = m_selectedGroup; // creations land in the group we're in
        auto menu = MakeRef<ui::ContextMenu>(self->MemoryAllocator());

        // ONE "Create" submenu holds every asset creator: uncategorized items flat, then one
        // NESTED submenu per category - a flat list of every creator outgrew the screen.
        {
            ui::MenuItem* createItem = menu->AddSubmenu(u8"Create");
            auto* create = Cast<ui::ContextMenu>(createItem->Submenu.Get());
            if (create != nullptr)
            {
                const auto addCreator = [self, target](ui::ContextMenu& into,
                                                       const pipeline::AssetCreator&
                                                           creator)
                {
                    const auto* entry = &creator;
                    into.AddItem(creator.label.AsView(),
                                 [self, entry, target]()
                                 {
                                     if (self->OnCreate)
                                     {
                                         self->OnCreate(*entry, target);
                                     }
                                     self->Rebuild();
                                 });
                };
                Array<StringView> categories;
                for (const pipeline::AssetCreator& creator : m_context->Creators().All())
                {
                    if (creator.category.IsEmpty())
                    {
                        addCreator(*create, creator);
                        continue;
                    }
                    bool seen = false;
                    for (StringView c : categories)
                    {
                        if (c == creator.category.AsView())
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        categories.PushBack(creator.category.AsView());
                    }
                }
                if (!categories.IsEmpty())
                {
                    create->AddSeparator();
                }
                categories.Sort([](StringView a, StringView b) { return a.Compare(b) < 0; });
                for (StringView category : categories)
                {
                    ui::MenuItem* categoryItem = create->AddSubmenu(category);
                    auto* categoryMenu = Cast<ui::ContextMenu>(categoryItem->Submenu.Get());
                    if (categoryMenu == nullptr)
                    {
                        continue;
                    }
                    for (const pipeline::AssetCreator& creator : m_context->Creators().All())
                    {
                        if (creator.category.AsView() == category)
                        {
                            addCreator(*categoryMenu, creator);
                        }
                    }
                }
            }
        }
        menu->AddItem(u8"New Group", [self, target]() { self->CreateGroupIn(target); });
        // Browse for a file to import (the app opens the native file dialog + routes it to ImportFile,
        // which shows the importer chooser / options dialog, or warns when no importer is registered).
        menu->AddItem(u8"Import...", [self]()
                      { if (self->OnBrowseImport) self->OnBrowseImport(); });
        if (target != nullptr)
        {
            menu->AddSeparator();
            if (target->Parent() != nullptr)
            {
                menu->AddItem(u8"Rename Group",
                              [self, target]() { self->StartRenameGroupInTreeDeferred(target); });
                menu->AddItem(IsGroupExportRoot(target)
                                  ? StringView(u8"Don't always export contents")
                                  : StringView(u8"Always export contents"),
                              [self, target]() { self->ToggleGroupExportRoot(target); });
            }
            menu->AddItem(u8"Cook Group",
                          [self, target]()
                          {
                              Array<Guid> ids;
                              CollectInstanceIds(target, ids);
                              self->m_cook->RequestCookFor(Move(ids), false);
                          });
            menu->AddItem(u8"Rebuild Group",
                          [self, target]()
                          {
                              Array<Guid> ids;
                              CollectInstanceIds(target, ids);
                              self->m_cook->RequestCookFor(Move(ids), true);
                          });
            if (target->Parent() != nullptr)
            {
                menu->AddItem(u8"Delete Group",
                              [self, target]() { self->ConfirmDeleteGroup(target); });
            }
        }
        menu->AddSeparator();
        menu->AddItem(u8"Cook All", [self]() { self->m_cook->RequestCook(false); });
        menu->AddItem(u8"Rebuild All", [self]() { self->m_cook->RequestCook(true); });
        const Float2 screenPos = anchor->LocalToScreen(Float2{x, y});
        menu->Show(Context, screenPos.x, screenPos.y);
    }

    bool AssetsView::IsInstanceExportRoot(const Guid& id) const
    {
        editor::EditorProject* project = m_context->Project();
        return project != nullptr && project->ExportRoots().HasInstance(id);
    }

    bool AssetsView::IsRowExportRoot(i32 position)
    {
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return false;
        }
        return (row->group != nullptr) ? IsGroupExportRoot(row->group)
                                       : IsInstanceExportRoot(row->id);
    }

    bool AssetsView::IsRowFavorite(i32 position)
    {
        const Row* row = RowAt(position);
        return row != nullptr && row->group == nullptr && m_context->IsFavorite(row->id);
    }

    bool AssetsView::IsGroupExportRoot(content::Group* group) const
    {
        editor::EditorProject* project = m_context->Project();
        return project != nullptr && group != nullptr &&
               project->ExportRoots().HasGroup(group->Path().AsView());
    }

    void AssetsView::ToggleInstanceExportRoot(const Guid& id)
    {
        editor::EditorProject* project = m_context->Project();
        if (project == nullptr)
        {
            return;
        }
        const bool nowRoot = project->ExportRoots().ToggleInstance(id);
        content::Instance* inst = Resolve(id);
        AfterExportRootChange(nowRoot, (inst != nullptr) ? inst->Name() : StringView(u8"asset"),
                              false);
    }

    void AssetsView::ToggleGroupExportRoot(content::Group* group)
    {
        editor::EditorProject* project = m_context->Project();
        if (project == nullptr || group == nullptr)
        {
            return;
        }
        const bool nowRoot = project->ExportRoots().ToggleGroup(group->Path().AsView());
        AfterExportRootChange(nowRoot, group->Name(), true);
    }

    void AssetsView::AfterExportRootChange(bool nowRoot, StringView name, bool isGroup)
    {
        editor::EditorProject* project = m_context->Project();
        if (project == nullptr)
        {
            return;
        }
        if (Status s = project->SaveExportRoots(); !s.IsOk())
        {
            m_context->Notify(editor::NoticeKind::Error,
                              u8"Failed to save export roots (export_roots.xml)");
            return;
        }
        String message =
            nowRoot ? String(u8"Always Export: ") : String(u8"Removed from Always Export: ");
        message += name;
        if (isGroup && nowRoot)
        {
            message += u8" (contents)";
        }
        m_context->Notify(editor::NoticeKind::Info, message.AsView());
        RebuildList();
    }

    void AssetsView::CreateGroupIn(content::Group* parent)
    {
        // Cook gate: see ImportFile (structural DB mutation while the plan worker reads).
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            m_cook->RunWhenIdle(
                Function<void()>{[self, parent]() { self->CreateGroupIn(parent); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"New group queued until the current cook finishes.");
            return;
        }
        if (parent == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        const String name = parent->UniqueGroupName(u8"Group");
        content::Group* created = parent->CreateGroup(name.AsView());
        if (created != nullptr)
        {
            // CreateGroup is in-memory only (no disk write until an instance is committed), and the
            // DB is rebuilt by SCANNING folders on reopen - so an empty group would vanish. Materialize
            // it as a real directory now, so a user-created group persists even while still empty.
            const String groupPath = created->Path();
            if (auto* writable = m_context->Project()->SourceDb().Mount().AsWritable())
            {
                (void)writable->CreateDirectory(groupPath.AsView());
            }
            LOG_INFO(u8"Assets", u8"created group '{}'", groupPath);
            m_selectedGroup = created;
            Rebuild();
        }
    }

    void AssetsView::DuplicateInstance(const Guid& id)
    {
        content::Instance* src = Resolve(id);
        if (src == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        content::ContentDatabase& db = m_context->Project()->SourceDb();

        // "name.2", "name.3", ... in the source's own group (the source holds the base).
        const String name = src->OwningGroup().UniqueInstanceName(src->Name());
        content::Instance* copy = db.CloneInstance(id, name.AsView());
        if (copy == nullptr)
        {
            m_context->Notify(editor::NoticeKind::Error,
                              u8"Duplicate FAILED (see console).");
            return;
        }
        LOG_INFO(u8"Assets", u8"duplicated '{}' -> '{}'", src->Path(), copy->Path());
        String message(u8"Duplicated as '");
        message += copy->Name();
        message += u8"'.";
        m_context->SetStatus(message.AsView());
        Rebuild();
        m_cook->RequestCook(false); // builder-backed clones become pickable right away
    }

    void AssetsView::ApplyRename(NameLabel* label, StringView newName)
    {
        if (label->TargetGroup() != nullptr)
        {
            ApplyRenameGroup(label->TargetGroup(), newName);
        }
        else
        {
            ApplyRenameInstance(label->TargetId(), newName);
        }
    }

    void AssetsView::ApplyRenameInstance(const Guid& id, StringView name)
    {
        // Cook gate (renames move files + rewrite both DBs' entries).
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            m_cook->RunWhenIdle(
                Function<void()>{[self, id, renamed = String(name)]()
                                 { self->ApplyRenameInstance(id, renamed.AsView()); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"Rename queued until the current cook finishes.");
            return;
        }
        if (m_context->Project() == nullptr)
        {
            return;
        }
        content::Instance* inst = Resolve(id);
        if (inst == nullptr)
        {
            return;
        }
        const String oldPath = inst->Path();
        const Status renamed = m_context->Project()->SourceDb().RenameInstance(id, name);
        if (!renamed.IsOk())
        {
            m_context->Notify(editor::NoticeKind::Error,
                              renamed.Code() == ErrorCode::AlreadyExists
                                  ? StringView(u8"NOT renamed: name already taken.")
                                  : StringView(u8"Rename FAILED (see console)."));
            Rebuild(); // snap the label back to the real name
            return;
        }
        // Keep the cooked product's name in step (same guid; purely cosmetic -
        // everything binds by guid - but stale names in Cooked/ confuse).
        (void)m_context->Project()->CookedDb().RenameInstance(id, name);
        // The manifest's default scene is guid-authoritative; refresh the
        // human-readable path mirror. The path compare covers guid-less
        // manifests (and adopts the guid while at it).
        auto* project = m_context->Project();
        if (project->Settings().defaultSceneId == id || project->Settings().defaultScene == oldPath)
        {
            project->Settings().defaultSceneId = id;
            project->Settings().defaultScene = inst->Path();
            (void)project->SaveSettings();
        }
        LOG_INFO(u8"Assets", u8"renamed '{}' -> '{}'", oldPath, inst->Path());
        Rebuild();
    }

    void AssetsView::ApplyRenameGroup(content::Group* group, StringView name)
    {
        // Cook gate: see ApplyRenameInstance.
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            m_cook->RunWhenIdle(
                Function<void()>{[self, group, renamed = String(name)]()
                                 { self->ApplyRenameGroup(group, renamed.AsView()); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"Rename queued until the current cook finishes.");
            return;
        }
        if (m_context->Project() == nullptr)
        {
            return;
        }
        const String oldPath = group->Path();
        const Status renamed = m_context->Project()->SourceDb().RenameGroup(*group, name);
        if (!renamed.IsOk())
        {
            m_context->Notify(editor::NoticeKind::Error,
                              renamed.Code() == ErrorCode::AlreadyExists
                                  ? StringView(u8"NOT renamed: name already taken.")
                                  : StringView(u8"Rename FAILED (see console)."));
            Rebuild();
            return;
        }
        // Mirror in the cooked DB when a same-path group exists there.
        content::Group* cooked = m_context->Project()->CookedDb().RootGroup();
        usize start = 0;
        const StringView path = oldPath.AsView();
        for (usize i = 0; i <= path.Size() && cooked != nullptr; ++i)
        {
            if (i == path.Size() || path[i] == utf8char('/'))
            {
                if (i > start)
                {
                    cooked = cooked->GetGroup(path.SubStr(start, i - start));
                }
                start = i + 1;
            }
        }
        if (cooked != nullptr)
        {
            (void)m_context->Project()->CookedDb().RenameGroup(*cooked, name);
        }
        // Refresh the default scene's path mirror if it lived under the renamed
        // group (guid still resolves; the mirror is cosmetic but shouldn't lie).
        auto* project = m_context->Project();
        if (!project->Settings().defaultSceneId.IsNil())
        {
            if (content::Instance* ds =
                    project->SourceDb().GetInstance(project->Settings().defaultSceneId))
            {
                if (project->Settings().defaultScene != ds->Path())
                {
                    project->Settings().defaultScene = ds->Path();
                    (void)project->SaveSettings();
                }
            }
        }
        else
        {
            const StringView ds = project->Settings().defaultScene.AsView();
            if (ds.Size() > oldPath.Size() && ds.SubStr(0, oldPath.Size()) == oldPath.AsView() &&
                ds[oldPath.Size()] == utf8char('/'))
            {
                String updated(group->Path());
                updated.Append(ds.SubStr(oldPath.Size(), ds.Size() - oldPath.Size()));
                project->Settings().defaultScene = Move(updated);
                (void)project->SaveSettings();
            }
        }
        LOG_INFO(u8"Assets", u8"renamed group '{}' -> '{}'", oldPath, group->Path());
        Rebuild();
    }

    void AssetsView::StartRename(i32 position)
    {
        NameLabel* label = nullptr;
        if (m_gridMode)
        {
            m_grid->ScrollToPosition(position);
            if (auto* tile = Cast<ui::FlexLayout>(m_grid->GetActiveView(position)))
            {
                if (tile->ChildCount() >= 2)
                {
                    label = static_cast<NameLabel*>(tile->GetChildAt(1));
                }
            }
        }
        else
        {
            m_list->ScrollToPosition(position);
            if (auto* row = Cast<ui::FlexLayout>(m_list->GetActiveView(position)))
            {
                if (row->ChildCount() >= 2)
                {
                    label = static_cast<NameLabel*>(row->GetChildAt(1));
                }
            }
        }
        if (label != nullptr)
        {
            label->BeginEdit();
        }
    }

    void AssetsView::StartRenameDeferred(i32 position)
    {
        ui::UIContext* ctx = Context;
        if (ctx == nullptr)
        {
            return;
        }
        AssetsView* self = this;
        ctx->MutationQueueRef().QueueAction(Function<void()>{
            [self, ctx, position]()
            {
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[self, position]() { self->StartRename(position); }});
            }});
    }

    void AssetsView::StartRenameGroupInTree(content::Group* group)
    {
        ui::FlattenedTreeAdapter* flat = m_tree->FlatAdapter();
        if (flat == nullptr)
        {
            return;
        }
        for (i32 pos = 0; pos < flat->ItemCount(); ++pos)
        {
            const i32 nodeId = flat->GetNodeId(pos);
            if (nodeId >= 0 && nodeId < static_cast<i32>(m_groups.Size()) &&
                m_groups[static_cast<usize>(nodeId)].group == group)
            {
                m_tree->InternalListView()->ScrollToPosition(pos);
                if (auto* row =
                        static_cast<NameLabel*>(m_tree->InternalListView()->GetActiveView(pos)))
                {
                    row->BeginEdit();
                }
                return;
            }
        }
    }

    void AssetsView::StartRenameGroupInTreeDeferred(content::Group* group)
    {
        ui::UIContext* ctx = Context;
        if (ctx == nullptr)
        {
            return;
        }
        AssetsView* self = this;
        ctx->MutationQueueRef().QueueAction(Function<void()>{
            [self, ctx, group]()
            {
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[self, group]() { self->StartRenameGroupInTree(group); }});
            }});
    }

    void AssetsView::ConfirmDelete(Array<Guid> ids)
    {
        if (ids.IsEmpty() || Context == nullptr)
        {
            return;
        }
        String message;
        if (ids.Size() == 1)
        {
            content::Instance* instance = Resolve(ids[0]);
            if (instance == nullptr)
            {
                return;
            }
            message += u8"Delete '";
            message += instance->Name();
            message += u8"'? Its source file and cooked product go away; open pages close.";
        }
        else
        {
            message += u8"Delete ";
            AppendCount(message, ids.Size());
            message +=
                u8" assets? Their source files and cooked products go away; open pages close.";
        }

        AssetsView* self = this;
        RefPtr<ui::Dialog> dialog = ui::Dialog::Confirm(MemoryAllocator(), u8"Delete assets", message.AsView());
        dialog->OnClosed.Add(ui::Event<void(ui::Dialog*, ui::DialogResult)>::Handler{
            [self, ids](ui::Dialog*, ui::DialogResult result)
            {
                if (result != ui::DialogResult::OK)
                {
                    return;
                }
                // Deferred: page teardown + DB mutation never run mid-event-dispatch.
                ui::UIContext* ctx = self->Context;
                if (ctx == nullptr)
                {
                    return;
                }
                Array<Guid> targets = ids;
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[self, targets]() { self->DeleteInstances(targets); }});
            }});
        dialog->Show(Context);
    }

    void AssetsView::DeleteInstances(const Array<Guid>& ids)
    {
        // Cook gate: see ImportFile/DeleteGroupNow.
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            Array<Guid> copy = ids;
            m_cook->RunWhenIdle(
                Function<void()>{[self, copy = Move(copy)]() { self->DeleteInstances(copy); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"Delete queued until the current cook finishes.");
            return;
        }
        if (m_context->Project() == nullptr)
        {
            return;
        }
        usize deleted = 0;
        for (const Guid& id : ids)
        {
            deleted += DeleteOne(id) ? 1u : 0u;
        }
        String message(u8"Deleted ");
        AppendCount(message, deleted);
        message += u8" asset(s).";
        m_context->SetStatus(message.AsView());
        ClearDefaultSceneIfGone();
        Rebuild(); // the next cook's plan sweeps the orphaned products
    }

    bool AssetsView::DeleteForAgent(const Guid& id)
    {
        if (m_context->Project() == nullptr || !DeleteOne(id))
        {
            return false;
        }
        ClearDefaultSceneIfGone();
        Rebuild();
        return true;
    }

    bool AssetsView::DeleteOne(const Guid& id)
    {
        content::ContentDatabase& db = m_context->Project()->SourceDb();
        content::Instance* instance = db.GetInstance(id);
        if (instance == nullptr)
        {
            return false;
        }
        const String path = instance->Path();
        if (OnCloseInstancePage)
        {
            OnCloseInstancePage(id);
        }
        if (!db.DeleteInstance(id).IsOk())
        {
            LOG_WARNING(u8"Assets", u8"delete FAILED for '{}'", path);
            return false;
        }
        LOG_INFO(u8"Assets", u8"deleted '{}'", path);
        return true;
    }

    void AssetsView::OnRowKeyDown(ui::SelectionModel* selection, i32 position, ui::KeyEventArgs& e)
    {
        const Row* row = RowAt(position);
        if (row == nullptr)
        {
            return;
        }
        if (e.Key == ui::KeyCode::F2)
        {
            StartRename(position);
            e.Handled = true;
        }
        else if (e.Key == ui::KeyCode::Delete)
        {
            if (row->group != nullptr)
            {
                ConfirmDeleteGroup(row->group);
            }
            else
            {
                ConfirmDelete(SelectedInstanceIds(selection, position));
            }
            e.Handled = true;
        }
    }

    void AssetsView::ConfirmDeleteGroup(content::Group* group)
    {
        if (group == nullptr || group->Parent() == nullptr || Context == nullptr)
        {
            return;
        }
        usize assetCount = 0;
        CountInstances(group, assetCount);
        String message(u8"Delete group '");
        message += group->Name();
        message += u8"' and ALL its contents (";
        AppendCount(message, assetCount);
        message += u8" asset(s))? Source files and cooked products go away; open pages close.";

        AssetsView* self = this;
        RefPtr<ui::Dialog> dialog = ui::Dialog::Confirm(MemoryAllocator(), u8"Delete group", message.AsView());
        dialog->OnClosed.Add(ui::Event<void(ui::Dialog*, ui::DialogResult)>::Handler{
            [self, group](ui::Dialog*, ui::DialogResult result)
            {
                if (result != ui::DialogResult::OK)
                {
                    return;
                }
                ui::UIContext* ctx = self->Context;
                if (ctx == nullptr)
                {
                    return;
                }
                // Deferred: page teardown + DB mutation never run mid-event-dispatch.
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[self, group]() { self->DeleteGroupNow(group); }});
            }});
        dialog->Show(Context);
    }

    void AssetsView::DeleteGroupNow(content::Group* group)
    {
        if (m_context->Project() == nullptr)
        {
            return;
        }
        // Same cook gate as ImportFile (deleting instances mid-cook dangles the worker's
        // snapshotted pointers).
        if (m_cook->MutationLocked())
        {
            AssetsView* self = this;
            m_cook->RunWhenIdle(Function<void()>{[self, group]() { self->DeleteGroupNow(group); }});
            m_context->Notify(editor::NoticeKind::Info,
                              u8"Delete queued until the current cook finishes.");
            return;
        }
        Array<Guid> ids;
        CollectInstanceIds(group, ids);
        if (OnCloseInstancePage)
        {
            for (const Guid& id : ids)
            {
                OnCloseInstancePage(id);
            }
        }
        // Navigate away BEFORE the pointers die.
        for (content::Group* g = m_selectedGroup; g != nullptr; g = g->Parent())
        {
            if (g == group)
            {
                m_selectedGroup = group->Parent();
                break;
            }
        }
        const String path = group->Path();
        if (m_context->Project()->SourceDb().DeleteGroup(*group).IsOk())
        {
            LOG_INFO(u8"Assets", u8"deleted group '{}' ({} asset(s))", path, ids.Size());
            String message(u8"Deleted group '");
            message += path;
            message += u8"'.";
            m_context->SetStatus(message.AsView());
        }
        else
        {
            LOG_WARNING(u8"Assets", u8"delete FAILED for group '{}'", path);
            m_context->Notify(editor::NoticeKind::Error,
                              u8"Delete group FAILED (see console).");
        }
        ClearDefaultSceneIfGone();
        Rebuild(); // the next cook's plan sweeps the orphaned products
    }

    void AssetsView::CountInstances(content::Group* group, usize& count)
    {
        count += group->Instances().Size();
        for (content::Group* child : group->Groups())
        {
            CountInstances(child, count);
        }
    }

    void AssetsView::CollectInstanceIds(content::Group* group, Array<Guid>& out)
    {
        for (content::Instance* instance : group->Instances())
        {
            out.PushBack(instance->Id());
        }
        for (content::Group* child : group->Groups())
        {
            CollectInstanceIds(child, out);
        }
    }

    void AssetsView::ClearDefaultSceneIfGone()
    {
        auto* project = m_context->Project();
        if (project == nullptr)
        {
            return;
        }
        if (project->Settings().defaultSceneId.IsNil() &&
            project->Settings().defaultScene.IsEmpty())
        {
            return;
        }
        const bool resolves =
            !project->Settings().defaultSceneId.IsNil()
                ? project->SourceDb().GetInstance(project->Settings().defaultSceneId) != nullptr
                : project->SourceDb().GetInstance(project->Settings().defaultScene.AsView()) !=
                      nullptr;
        if (resolves)
        {
            return;
        }
        project->Settings().defaultSceneId = Guid{};
        project->Settings().defaultScene = String();
        (void)project->SaveSettings();
        LOG_INFO(u8"Assets", u8"default scene was deleted - cleared it in the manifest");
    }

    void AssetsView::AppendCount(String& out, usize value)
    {
        utf8char digits[20];
        usize n = 0;
        do
        {
            digits[n++] = static_cast<utf8char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (n > 0)
        {
            out.PushBack(digits[--n]);
        }
    }

    content::Instance* AssetsView::Resolve(const Guid& id)
    {
        return (m_context->Project() != nullptr) ? m_context->Project()->SourceDb().GetInstance(id)
                                                 : nullptr;
    }
}
