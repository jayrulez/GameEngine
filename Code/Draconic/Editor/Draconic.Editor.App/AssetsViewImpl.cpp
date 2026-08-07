// Draconic::EditorApp - :assets_view partition.
//
// AssetsView: the Assets panel (asset-pipeline design §7) - source-DB-backed, the typed DB is
// the truth (Traktor's DatabaseView model, not a directory scan). Left = group tree; right =
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.app;

import draconic.foundation;
import draconic.content;
import draconic.fonts;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :editor_icons;
import :import_dialog;

using namespace draconic::foundation;

namespace draconic::editor::app
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
        if (m_context->Project() == nullptr || Context == nullptr)
        {
            return;
        }
        const String ext = draconic::editor::FileExtensionLower(path);
        draconic::editor::IFileImporter* importer = m_context->Importers().FindFor(ext.AsView());
        if (importer == nullptr)
        {
            String message(u8"No importer for '");
            message += draconic::editor::FileNameOf(path);
            message += u8"'.";
            m_context->Notify(draconic::editor::NoticeKind::Warning, message.AsView());
            return;
        }

        RefPtr<draconic::editor::ImportOptions> options = importer->CreateOptions();
        if (options.Get() == nullptr)
        {
            ExecuteImport(String(path), importer, {});
            return;
        }
        content::Group* group = (m_selectedGroup != nullptr)
                                    ? m_selectedGroup
                                    : m_context->Project()->SourceDb().RootGroup();
        auto dialog =
            MakeRef<ImportOptionsDialog>(DefaultAllocator(), path, group->Path().AsView(), options);
        AssetsView* self = this;
        dialog->OnImport = [self, file = String(path), importer,
                            opts = RefPtr<draconic::editor::ImportOptions>(options.Get())]()
        { self->ExecuteImport(file, importer, opts); };
        dialog->Show(Context);
    }

    void AssetsView::ExecuteImport(String path, draconic::editor::IFileImporter* importer,
                                   RefPtr<draconic::editor::ImportOptions> options)
    {
        if (m_context->Project() == nullptr)
        {
            return;
        }
        if (importer->WantsWorkerPrepare() && m_jobs != nullptr)
        {
            String title(u8"Importing ");
            title += draconic::editor::FileNameOf(path.AsView());
            auto* holder = DefaultAllocator().New<RefPtr<Object>>();
            AssetsView* self = this;
            m_jobs->Submit(title.AsView(),
                           Function<Status(draconic::editor::JobContext&)>{
                               [importer, path, holder](draconic::editor::JobContext& job) -> Status
                               {
                                   job.SetStep(u8"loading + decoding", 1, 2);
                                   *holder = importer->PrepareOnWorker(path.AsView());
                                   return (holder->Get() != nullptr)
                                              ? Status{}
                                              : Status{ErrorCode::InvalidArgument};
                               }},
                           Function<void(Status)>{
                               [self, path, importer, options, holder](Status result)
                               {
                                   RefPtr<Object> prepared = *holder;
                                   DefaultAllocator().Delete(holder);
                                   if (!result.IsOk())
                                   {
                                       String message(u8"Import failed: '");
                                       message += draconic::editor::FileNameOf(path.AsView());
                                       message += u8"' (see Console).";
                                       self->m_context->Notify(draconic::editor::NoticeKind::Error,
                                                               message.AsView());
                                       return;
                                   }
                                   self->CommitImport(path, importer, options, prepared);
                               }});
            return;
        }
        CommitImport(path, importer, options, {});
    }

    void AssetsView::CommitImport(String path, draconic::editor::IFileImporter* importer,
                                  RefPtr<draconic::editor::ImportOptions> options,
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
                              u8"Import queued until the current cook finishes.");
            return;
        }
        content::Group* group = (m_selectedGroup != nullptr)
                                    ? m_selectedGroup
                                    : m_context->Project()->SourceDb().RootGroup();
        auto deferred =
            MakeUnique<Array<draconic::editor::DeferredImportWrite>>(DefaultAllocator());
        Result<content::Instance*> imported =
            importer->Import(path.AsView(), *m_context->Project(), *group, options.Get(),
                             prepared.Get(), (m_jobs != nullptr) ? deferred.Get() : nullptr);
        if (!imported.HasValue() || imported.Value() == nullptr)
        {
            String message(u8"Import failed: '");
            message += draconic::editor::FileNameOf(path.AsView());
            message += u8"' (see Console).";
            m_context->Notify(draconic::editor::NoticeKind::Error, message.AsView());
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
        title += draconic::editor::FileNameOf(path.AsView());
        AssetsView* self = this;
        auto* writes = deferred.Release();
        const Guid primaryId = primary->Id();
        m_jobs->Submit(
            title.AsView(),
            Function<Status(draconic::editor::JobContext&)>{
                [writes, prepared](draconic::editor::JobContext& job) -> Status
                {
                    Status result{};
                    for (usize i = 0; i < writes->Size(); ++i)
                    {
                        draconic::editor::DeferredImportWrite& write = (*writes)[i];
                        job.SetStep(write.Label(), i + 1, writes->Size());
                        job.SetFraction(static_cast<f32>(i) / static_cast<f32>(writes->Size()));
                        const Status s = write.Execute();
                        if (!s.IsOk())
                        {
                            result = s;
                        }
                    }
                    (void)prepared; // keeps the decoded pixels alive for the views
                    return result;
                }},
            Function<void(Status)>{
                [self, writes, importer, options, primaryId](Status result)
                {
                    DefaultAllocator().Delete(writes);
                    content::Instance* primary =
                        (self->m_context->Project() != nullptr)
                            ? self->m_context->Project()->SourceDb().GetInstance(primaryId)
                            : nullptr;
                    if (!result.IsOk() || primary == nullptr)
                    {
                        self->m_context->Notify(draconic::editor::NoticeKind::Error,
                                                u8"Import data write FAILED (see Console).");
                        self->Rebuild();
                        return;
                    }
                    self->FinishImport(*primary, importer, options);
                }});
    }

    void AssetsView::FinishImport(content::Instance& primary,
                                  draconic::editor::IFileImporter* importer,
                                  const RefPtr<draconic::editor::ImportOptions>& options)
    {
        String message(u8"Imported '");
        message += primary.Name();
        message += u8"' (";
        message += importer->Label();
        message += u8").";
        m_context->Notify(draconic::editor::NoticeKind::Success, message.AsView());
        m_context->NotifyImported(primary, options.Get());
        // Cook the imported assets explicitly (scoped to the primary's group; the plan
        // skips anything clean). Auto-cook used to ride on the Sources/ watcher noticing
        // the provenance copy - a re-import of identical bytes skips that copy, so the
        // watcher never fires and the new instances sat uncooked until a manual cook.
        Array<Guid> ids;
        CollectInstanceIds(&primary.OwningGroup(), ids);
        m_cook->RequestCookFor(Move(ids), false);
        Rebuild();
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
        case draconic::editor::CookBadge::Cooked:
            color = Color{0.6f, 0.9f, 0.6f, 1.0f};
            break;
        case draconic::editor::CookBadge::Missing:
            color = Color{0.95f, 0.85f, 0.5f, 1.0f};
            break;
        case draconic::editor::CookBadge::Failed:
            color = Color{1.0f, 0.45f, 0.45f, 1.0f};
            break;
        case draconic::editor::CookBadge::NoBuilder:
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
        case draconic::editor::CookBadge::Cooked:
            text.Append(u8"  [cooked]");
            color = Color{0.6f, 0.9f, 0.6f, 1.0f};
            break;
        case draconic::editor::CookBadge::Missing:
            text.Append(u8"  [not cooked]");
            color = Color{0.95f, 0.85f, 0.5f, 1.0f};
            break;
        case draconic::editor::CookBadge::Failed:
            text.Append(u8"  [FAILED]");
            color = Color{1.0f, 0.45f, 0.45f, 1.0f};
            break;
        case draconic::editor::CookBadge::NoBuilder:
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

    void AssetsView::NavigateToBreadcrumb(i32 segment)
    {
        if (segment >= 0 && segment < static_cast<i32>(m_breadcrumbGroups.Size()))
        {
            SelectGroup(m_breadcrumbGroups[static_cast<usize>(segment)]);
        }
    }

    void AssetsView::SetGridMode(bool grid)
    {
        m_gridMode = grid;
        m_listToggle->IsChecked.SetValue(!grid);
        m_gridToggle->IsChecked.SetValue(grid);
        m_list->Visibility = grid ? ui::VisibilityValue::Gone : ui::VisibilityValue::Visible;
        m_grid->Visibility = grid ? ui::VisibilityValue::Visible : ui::VisibilityValue::Gone;
        Invalidate();
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
            auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
            menu->AddItem(u8"Open", [self, group]() { self->SelectGroup(group); });
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
            menu->AddItem(u8"Rename", [self, position]() { self->StartRenameDeferred(position); });
            menu->AddItem(IsGroupExportRoot(group) ? StringView(u8"Don't always export contents")
                                                   : StringView(u8"Always export contents"),
                          [self, group]() { self->ToggleGroupExportRoot(group); });
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

        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
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
        menu->AddItem(u8"Rename", [self, position]() { self->StartRenameDeferred(position); });
        menu->AddItem(u8"Duplicate", [self, id]() { self->DuplicateInstance(id); });
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
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());

        // Top-level creators, then categorized ones ("Primitives") in submenus, then the
        // group + cook actions.
        Array<StringView> categories;
        for (const draconic::editor::EditorContext::AssetCreator& creator : m_context->Creators())
        {
            if (creator.category.IsEmpty())
            {
                String label(u8"New ");
                label += creator.label;
                const auto* entry = &creator;
                menu->AddItem(label.AsView(),
                              [self, entry, target]()
                              {
                                  if (self->OnCreate)
                                  {
                                      self->OnCreate(*entry, target);
                                  }
                                  self->Rebuild();
                              });
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
        menu->AddItem(u8"New Group", [self, target]() { self->CreateGroupIn(target); });
        if (target != nullptr)
        {
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
        }
        if (target != nullptr && target->Parent() != nullptr)
        {
            menu->AddItem(u8"Rename Group",
                          [self, target]() { self->StartRenameGroupInTreeDeferred(target); });
            menu->AddItem(IsGroupExportRoot(target) ? StringView(u8"Don't always export contents")
                                                    : StringView(u8"Always export contents"),
                          [self, target]() { self->ToggleGroupExportRoot(target); });
            menu->AddItem(u8"Delete Group", [self, target]() { self->ConfirmDeleteGroup(target); });
        }
        if (!categories.IsEmpty())
        {
            menu->AddSeparator();
        }
        for (StringView category : categories)
        {
            ui::MenuItem* submenuItem = menu->AddSubmenu(category);
            auto* submenu = Cast<ui::ContextMenu>(submenuItem->Submenu.Get());
            if (submenu == nullptr)
            {
                continue;
            }
            for (const draconic::editor::EditorContext::AssetCreator& creator :
                 m_context->Creators())
            {
                if (creator.category.AsView() != category)
                {
                    continue;
                }
                const auto* entry = &creator;
                submenu->AddItem(creator.label.AsView(),
                                 [self, entry, target]()
                                 {
                                     if (self->OnCreate)
                                     {
                                         self->OnCreate(*entry, target);
                                     }
                                     self->Rebuild();
                                 });
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
        draconic::editor::EditorProject* project = m_context->Project();
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
        draconic::editor::EditorProject* project = m_context->Project();
        return project != nullptr && group != nullptr &&
               project->ExportRoots().HasGroup(group->Path().AsView());
    }

    void AssetsView::ToggleInstanceExportRoot(const Guid& id)
    {
        draconic::editor::EditorProject* project = m_context->Project();
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
        draconic::editor::EditorProject* project = m_context->Project();
        if (project == nullptr || group == nullptr)
        {
            return;
        }
        const bool nowRoot = project->ExportRoots().ToggleGroup(group->Path().AsView());
        AfterExportRootChange(nowRoot, group->Name(), true);
    }

    void AssetsView::AfterExportRootChange(bool nowRoot, StringView name, bool isGroup)
    {
        draconic::editor::EditorProject* project = m_context->Project();
        if (project == nullptr)
        {
            return;
        }
        if (Status s = project->SaveExportRoots(); !s.IsOk())
        {
            m_context->Notify(draconic::editor::NoticeKind::Error,
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
        m_context->Notify(draconic::editor::NoticeKind::Info, message.AsView());
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
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
            DRACONIC_LOG_INFO(u8"Assets", u8"created group '{}'", created->Path());
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
            m_context->Notify(draconic::editor::NoticeKind::Error,
                              u8"Duplicate FAILED (see console).");
            return;
        }
        DRACONIC_LOG_INFO(u8"Assets", u8"duplicated '{}' -> '{}'", src->Path(), copy->Path());
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
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
            m_context->Notify(draconic::editor::NoticeKind::Error,
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
        DRACONIC_LOG_INFO(u8"Assets", u8"renamed '{}' -> '{}'", oldPath, inst->Path());
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
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
            m_context->Notify(draconic::editor::NoticeKind::Error,
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
        DRACONIC_LOG_INFO(u8"Assets", u8"renamed group '{}' -> '{}'", oldPath, group->Path());
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
        RefPtr<ui::Dialog> dialog = ui::Dialog::Confirm(u8"Delete assets", message.AsView());
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
                              u8"Delete queued until the current cook finishes.");
            return;
        }
        if (m_context->Project() == nullptr)
        {
            return;
        }
        content::ContentDatabase& db = m_context->Project()->SourceDb();
        usize deleted = 0;
        for (const Guid& id : ids)
        {
            content::Instance* instance = db.GetInstance(id);
            if (instance == nullptr)
            {
                continue;
            }
            const String path = instance->Path();
            if (OnCloseInstancePage)
            {
                OnCloseInstancePage(id);
            }
            if (db.DeleteInstance(id).IsOk())
            {
                ++deleted;
                DRACONIC_LOG_INFO(u8"Assets", u8"deleted '{}'", path);
            }
            else
            {
                DRACONIC_LOG_WARNING(u8"Assets", u8"delete FAILED for '{}'", path);
            }
        }
        String message(u8"Deleted ");
        AppendCount(message, deleted);
        message += u8" asset(s).";
        m_context->SetStatus(message.AsView());
        ClearDefaultSceneIfGone();
        Rebuild(); // the next cook's plan sweeps the orphaned products
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
        RefPtr<ui::Dialog> dialog = ui::Dialog::Confirm(u8"Delete group", message.AsView());
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
            m_context->Notify(draconic::editor::NoticeKind::Info,
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
            DRACONIC_LOG_INFO(u8"Assets", u8"deleted group '{}' ({} asset(s))", path, ids.Size());
            String message(u8"Deleted group '");
            message += path;
            message += u8"'.";
            m_context->SetStatus(message.AsView());
        }
        else
        {
            DRACONIC_LOG_WARNING(u8"Assets", u8"delete FAILED for group '{}'", path);
            m_context->Notify(draconic::editor::NoticeKind::Error,
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
        DRACONIC_LOG_INFO(u8"Assets", u8"default scene was deleted - cleared it in the manifest");
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
