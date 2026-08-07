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

export module draconic.editor.app:assets_view;

import draconic.foundation;
import draconic.content;
import draconic.fonts;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :editor_icons;
import :import_dialog;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;
    namespace content = draconic::content;

    class AssetsView final : public ui::ViewGroup
    {
        DRACONIC_OBJECT(AssetsView, ui::ViewGroup)
    public:
        /// Open an instance's editor page (wired by the application).
        Function<void(content::Instance&)> OnOpenInstance;
        /// Create an asset via a registry creator (wired by the application - it also opens it).
        /// `group` = the group the menu was invoked for (creations land there).
        Function<void(const draconic::editor::EditorContext::AssetCreator&, content::Group*)>
            OnCreate;
        /// Close any open editor page for this instance BEFORE it is deleted (wired by the
        /// application; called from a mutation-queue action, so synchronous teardown is safe).
        Function<void(const Guid&)> OnCloseInstancePage;

        AssetsView(draconic::editor::EditorContext& context,
                   draconic::editor::EditorCookService& cook,
                   draconic::editor::EditorJobService* jobs = nullptr)
            : m_context(&context), m_cook(&cook), m_jobs(jobs)
        {
            auto split = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
            split->SetSplitRatio(0.3f);

            // Left: the group tree.
            m_treeAdapter = MakeUnique<TreeAdapter>(DefaultAllocator(), *this);
            m_tree = MakeRef<ui::TreeView>(DefaultAllocator());
            m_tree->SetItemHeight(22.0f);
            m_tree->SetAdapter(m_treeAdapter.Get());
            {
                AssetsView* self = this;
                m_tree->OnItemClick.Add(
                    [self](ui::TreeView::ItemClickInfo info)
                    {
                        if (info.NodeId >= 0 &&
                            info.NodeId < static_cast<i32>(self->m_groups.Size()))
                        {
                            self->SelectGroup(
                                self->m_groups[static_cast<usize>(info.NodeId)].group);
                        }
                    });
                m_tree->OnItemRightClick.Add(
                    [self](i32 nodeId, f32 x, f32 y)
                    {
                        if (nodeId >= 0 && nodeId < static_cast<i32>(self->m_groups.Size()))
                        {
                            self->SelectGroup(self->m_groups[static_cast<usize>(nodeId)].group);
                        }
                        self->ShowBackgroundMenu(self->m_tree.Get(), x, y);
                    });
                m_tree->OnItemKeyDown.Add(
                    [self](i32 nodeId, ui::KeyEventArgs& e)
                    {
                        if (nodeId < 0 || nodeId >= static_cast<i32>(self->m_groups.Size()))
                        {
                            return;
                        }
                        content::Group* group = self->m_groups[static_cast<usize>(nodeId)].group;
                        if (group->Parent() == nullptr)
                        {
                            return;
                        } // the root: no rename/delete
                        if (e.Key == ui::KeyCode::F2)
                        {
                            self->StartRenameGroupInTree(group);
                            e.Handled = true;
                        }
                        else if (e.Key == ui::KeyCode::Delete)
                        {
                            self->ConfirmDeleteGroup(group);
                            e.Handled = true;
                        }
                    });
            }

            // Right: [breadcrumb | view toggle] over [filter] over [list | grid].
            auto right = MakeRef<ui::FlexLayout>(DefaultAllocator());
            right->Direction = ui::Orientation::Vertical;
            right->Padding =
                ui::Thickness{6, 4}; // inset the content off the edge (like the group tree)
            right->Spacing = 4;      // separate the header row, filter, and content
            {
                AssetsView* self = this;

                auto header = MakeRef<ui::FlexLayout>(DefaultAllocator());
                header->Direction = ui::Orientation::Horizontal;
                header->Spacing = 4;
                m_breadcrumb = MakeRef<ui::toolkit::BreadcrumbBar>(DefaultAllocator());
                m_breadcrumb->OnSegmentClicked.Add(
                    ui::Event<void(ui::toolkit::BreadcrumbBar*, i32)>::Handler{
                        [self](ui::toolkit::BreadcrumbBar*, i32 segment)
                        { self->NavigateToBreadcrumb(segment); }});
                {
                    auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    grow->Grow = 1.0f;
                    grow->Height = ui::SizeSpec::Match();
                    header->AddView(m_breadcrumb.Get(), grow);
                }
                m_listToggle = MakeRef<ui::ToggleButton>(DefaultAllocator(), StringView(u8"List"));
                m_gridToggle = MakeRef<ui::ToggleButton>(DefaultAllocator(), StringView(u8"Grid"));
                m_listToggle->IsChecked.SetValue(true);
                m_listToggle->OnClick.Add([self](ui::ButtonBase*) { self->SetGridMode(false); });
                m_gridToggle->OnClick.Add([self](ui::ButtonBase*) { self->SetGridMode(true); });
                header->AddView(m_listToggle.Get());
                header->AddView(m_gridToggle.Get());
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(26));
                    right->AddView(header.Get(), lp);
                }
            }
            m_filterEdit = MakeRef<ui::EditText>(DefaultAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter all assets...");
            {
                AssetsView* self = this;
                m_filterEdit->OnTextChanged.Add(
                    [self](ui::EditText* edit)
                    {
                        self->m_filter = String(edit->Text());
                        self->RebuildList();
                    });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                right->AddView(m_filterEdit.Get(), lp);
            }
            m_listAdapter = MakeUnique<ListAdapter>(DefaultAllocator(), *this);
            m_gridAdapter = MakeUnique<GridAdapter>(DefaultAllocator(), *this);
            m_list = MakeRef<ui::ListView>(DefaultAllocator());
            m_list->ItemHeight.SetValue(22.0f);
            m_list->Selection.Mode = ui::SelectionMode::Multiple;
            m_list->SetAdapter(m_listAdapter.Get());
            m_grid = MakeRef<ui::GridView>(DefaultAllocator());
            m_grid->CellWidth.SetValue(96.0f);
            m_grid->CellHeight.SetValue(84.0f);
            m_grid->Selection.Mode = ui::SelectionMode::Multiple;
            m_grid->Visibility = ui::VisibilityValue::Gone;
            m_grid->SetAdapter(m_gridAdapter.Get());
            {
                AssetsView* self = this;
                m_list->OnItemClicked.Add([self](i32 position, i32 clickCount, f32, f32)
                                          { self->ActivateRow(position, clickCount); });
                m_list->OnItemRightClicked.Add(
                    [self](i32 position, f32 x, f32 y)
                    {
                        self->ShowRowMenu(self->m_list.Get(), &self->m_list->Selection, position, x,
                                          y);
                    });
                m_list->OnBackgroundRightClicked.Add(
                    [self](f32 x, f32 y) { self->ShowBackgroundMenu(self->m_list.Get(), x, y); });
                m_grid->OnItemClicked.Add([self](i32 position, i32 clickCount, f32, f32)
                                          { self->ActivateRow(position, clickCount); });
                m_grid->OnItemRightClicked.Add(
                    [self](i32 position, f32 x, f32 y)
                    {
                        self->ShowRowMenu(self->m_grid.Get(), &self->m_grid->Selection, position, x,
                                          y);
                    });
                m_grid->OnBackgroundRightClicked.Add(
                    [self](f32 x, f32 y) { self->ShowBackgroundMenu(self->m_grid.Get(), x, y); });
                m_list->OnItemKeyDown.Add(
                    [self](i32 position, ui::KeyEventArgs& e)
                    { self->OnRowKeyDown(&self->m_list->Selection, position, e); });
                m_grid->OnItemKeyDown.Add(
                    [self](i32 position, ui::KeyEventArgs& e)
                    { self->OnRowKeyDown(&self->m_grid->Selection, position, e); });
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                grow->Width = ui::SizeSpec::Match();
                right->AddView(m_list.Get(), grow);
                auto grow2 = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow2->Grow = 1.0f;
                grow2->Width = ui::SizeSpec::Match();
                right->AddView(m_grid.Get(), grow2);
            }

            split->SetPanes(m_tree.Get(), right.Get());
            AddView(split.Get());
            Rebuild();
        }

        ~AssetsView() override
        {
            m_tree->SetAdapter(nullptr);
            m_list->SetAdapter(nullptr);
            m_grid->SetAdapter(nullptr);
        }

        /// Per-frame: refresh badges after a cook finishes (DB shape changes call Rebuild()).
        void Refresh();

        /// Import an OS file (drag-dropped onto the editor) into the selected group via the
        /// registered importers. An importer with options gets the pre-import dialog first;
        /// the actual import runs in ExecuteImport.
        void ImportFile(StringView path);

        /// Runs the import (post-dialog). Slow importers (models) split: the parse/decode
        /// runs on the JOB worker so the UI stays live (with the status-bar progress), and
        /// only the fast DB fan-out lands back on the main thread in CommitImport.
        void ExecuteImport(String path, draconic::editor::IFileImporter* importer,
                           RefPtr<draconic::editor::ImportOptions> options);

        /// The main-thread tail: DB fan-out (+ the build-lock re-check, so a cook that
        /// started while the dialog/worker was busy still queues instead of racing).
        void CommitImport(String path, draconic::editor::IFileImporter* importer,
                          RefPtr<draconic::editor::ImportOptions> options, RefPtr<Object> prepared);

        void FinishImport(content::Instance& primary, draconic::editor::IFileImporter* importer,
                          const RefPtr<draconic::editor::ImportOptions>& options);

        /// Full rebuild: group tree + list (project open/close, create/delete/import).
        void Rebuild();

        // Fill the available space.
        void OnMeasure(ui::BoxConstraints constraints) override;
        void OnLayout(f32, f32, f32 width, f32 height) override;

    private:
        struct GroupNode
        {
            content::Group* group = nullptr;
            i32 depth = 0;
            Array<i32> children;
        };

        // One content-area row: a SUBGROUP of the selected group (group != null) or an instance.
        struct Row
        {
            Guid id;                         // instance identity (safe across deletes)
            content::Group* group = nullptr; // non-null => a navigable subgroup row
        };

        // The name element of every row/tile/tree-node: an EditableLabel that knows WHAT it
        // names, so one commit handler routes to the instance or group rename. Slow-click /
        // F2 / menu Rename edit in place; double-click is deliberately NOT an edit trigger
        // (it navigates); single clicks pass through to the list's selection.
        // A list-row / grid-tile container that overlays small status dots in its top-right corner
        // (the Sedulous registry-badge idiom: proper visual badges, not name-color tints). Dots draw
        // in local space after the children, so they sit on top of the icon in both list and grid.
        // Multiple dots stack right-to-left in a fixed priority order (export outermost). Set per-bind.
        class AssetCell final : public ui::FlexLayout
        {
        public:
            bool ShowExportBadge = false;   // green: an "Always Export" root
            bool ShowFavoriteBadge = false; // gold: a pinned favorite
            void OnDraw(ui::UIDrawContext& ctx) override
            {
                ui::FlexLayout::OnDraw(ctx); // draw children (icon + name [+ meta])
                f32 x = Width() - 7.0f;
                const auto dot = [&](Color color)
                {
                    ctx.VG().FillCircle(Float2{x, 7.0f}, 3.0f, color);
                    x -= 8.0f;
                };
                if (ShowExportBadge)
                {
                    dot(Color{80.0f / 255.0f, 180.0f / 255.0f, 80.0f / 255.0f, 1.0f});
                }
                if (ShowFavoriteBadge)
                {
                    dot(Color{242.0f / 255.0f, 204.0f / 255.0f, 89.0f / 255.0f, 1.0f});
                }
            }
        };

        class NameLabel final : public ui::EditableLabel
        {
        public:
            void BindTarget(const Guid& id, content::Group* group)
            {
                m_id = id;
                m_group = group;
            }
            [[nodiscard]] const Guid& TargetId() const noexcept { return m_id; }
            [[nodiscard]] content::Group* TargetGroup() const noexcept { return m_group; }

        private:
            Guid m_id;
            content::Group* m_group = nullptr;
        };

        // Shared setup: names are filenames/directory names, so filesystem-hostile
        // characters never commit; double-click stays navigation.
        static void ConfigureNameLabel(NameLabel& label, AssetsView& owner);

        // === adapters ===

        class TreeAdapter final : public ui::ITreeAdapter
        {
        public:
            explicit TreeAdapter(AssetsView& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 RootCount() const override
            {
                return m_owner->m_groups.IsEmpty() ? 0 : 1;
            }
            [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
            {
                if (nodeId == -1)
                {
                    return RootCount();
                }
                return InRange(nodeId) ? static_cast<i32>(Node(nodeId).children.Size()) : 0;
            }
            [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
            {
                if (parentId == -1)
                {
                    return childIndex == 0 && !m_owner->m_groups.IsEmpty() ? 0 : -1;
                }
                if (!InRange(parentId))
                {
                    return -1;
                }
                const Array<i32>& kids = Node(parentId).children;
                return (childIndex >= 0 && childIndex < static_cast<i32>(kids.Size()))
                           ? kids[static_cast<usize>(childIndex)]
                           : -1;
            }
            [[nodiscard]] i32 GetDepth(i32 nodeId) const override
            {
                return InRange(nodeId) ? Node(nodeId).depth : 0;
            }
            [[nodiscard]] bool HasChildren(i32 nodeId) const override
            {
                return GetChildCount(nodeId) > 0;
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<NameLabel>(DefaultAllocator());
                row->FontSize.SetValue(13.0f);
                ConfigureNameLabel(*row, *m_owner);
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 nodeId, i32 depth, bool) override
            {
                if (!InRange(nodeId))
                {
                    return;
                }
                auto* row = static_cast<NameLabel*>(view);
                const GroupNode& node = Node(nodeId);
                const bool isRoot = node.group->Parent() == nullptr;
                row->BindTarget(Guid{}, node.group);
                row->SetText(isRoot ? StringView(u8"Content") : node.group->Name());
                row->SlowClickToEdit.SetValue(!isRoot); // the root is not renamable
                row->TextOffsetX.SetValue(static_cast<f32>(depth + 1) * 18.0f);
            }

        private:
            [[nodiscard]] bool InRange(i32 nodeId) const
            {
                return nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_groups.Size());
            }
            [[nodiscard]] const GroupNode& Node(i32 nodeId) const
            {
                return m_owner->m_groups[static_cast<usize>(nodeId)];
            }
            AssetsView* m_owner;
        };

        class ListAdapter final : public ui::ListAdapterBase
        {
        public:
            explicit ListAdapter(AssetsView& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_rows.Size());
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<AssetCell>(DefaultAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6;
                row->Padding = ui::Thickness{4, 3};
                auto iconView = MakeRef<ui::DrawableView>(DefaultAllocator());
                iconView->DesiredWidth.SetValue(Optional<f32>(16.0f));
                iconView->DesiredHeight.SetValue(Optional<f32>(16.0f));
                row->AddView(iconView.Get());
                auto name = MakeRef<NameLabel>(DefaultAllocator());
                name->FontSize.SetValue(13.0f);
                ConfigureNameLabel(*name, *m_owner);
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                row->AddView(name.Get(), grow);
                auto meta = MakeRef<ui::Label>(DefaultAllocator());
                meta->FontSize.SetValue(13.0f);
                row->AddView(meta.Get());
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 position) override
            {
                auto* row = Cast<AssetCell>(view);
                if (row == nullptr || row->ChildCount() < 3)
                {
                    return;
                }
                row->ShowExportBadge = m_owner->IsRowExportRoot(position);
                row->ShowFavoriteBadge = m_owner->IsRowFavorite(position);
                auto* iconView = Cast<ui::DrawableView>(row->GetChildAt(0));
                auto* name = static_cast<NameLabel*>(row->GetChildAt(1));
                auto* meta = Cast<ui::Label>(row->GetChildAt(2));
                if (iconView == nullptr || name == nullptr || meta == nullptr)
                {
                    return;
                }
                iconView->Drawable = ui::DrawablePtr(m_owner->RowIcon(position));
                const Row* target = m_owner->RowAt(position);
                name->BindTarget(target != nullptr ? target->id : Guid{},
                                 target != nullptr ? target->group : nullptr);
                String text;
                Color color{0.85f, 0.85f, 0.85f, 1.0f};
                m_owner->RowName(position, text, color);
                name->SetText(text.AsView());
                name->TextColor.SetValue(Optional<Color>(color));
                String metaText;
                Color metaColor{0.55f, 0.58f, 0.65f, 1.0f};
                m_owner->RowMeta(position, metaText, metaColor);
                meta->SetText(metaText.AsView());
                meta->TextColor.SetValue(Optional<Color>(metaColor));
            }

        private:
            AssetsView* m_owner;
        };

        // Grid tiles: the same rows as the list, name under a (future-thumbnail) type block.
        class GridAdapter final : public ui::ListAdapterBase
        {
        public:
            explicit GridAdapter(AssetsView& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_rows.Size());
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto tile = MakeRef<AssetCell>(DefaultAllocator());
                tile->Direction = ui::Orientation::Vertical;
                tile->Padding = ui::Thickness{4, 4};
                auto iconRow = MakeRef<ui::FlexLayout>(DefaultAllocator());
                iconRow->Direction = ui::Orientation::Horizontal;
                iconRow->JustifyContent = ui::Justify::Center;
                // Cross-axis default is Stretch - without Center the icon fills the row's
                // height (taller than 40) and the glyph draws vertically stretched.
                iconRow->AlignItems = ui::Align::Center;
                auto iconView = MakeRef<ui::DrawableView>(DefaultAllocator());
                iconView->DesiredWidth.SetValue(Optional<f32>(40.0f));
                iconView->DesiredHeight.SetValue(Optional<f32>(40.0f));
                iconRow->AddView(iconView.Get());
                auto name = MakeRef<NameLabel>(DefaultAllocator());
                name->FontSize.SetValue(12.0f);
                name->HAlign.SetValue(draconic::fonts::TextAlignment::Center);
                name->Ellipsis.SetValue(
                    true); // long asset names truncate with "..." instead of overflowing the tile
                ConfigureNameLabel(*name, *m_owner);
                {
                    auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    grow->Grow = 1.0f;
                    grow->Width = ui::SizeSpec::Match();
                    tile->AddView(iconRow.Get(), grow);
                }
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(18));
                    tile->AddView(name.Get(), lp);
                }
                return RefPtr<ui::View>(tile.Get());
            }
            void BindView(ui::View* view, i32 position) override
            {
                auto* tile = Cast<AssetCell>(view);
                if (tile == nullptr || tile->ChildCount() < 2)
                {
                    return;
                }
                tile->ShowExportBadge = m_owner->IsRowExportRoot(position);
                tile->ShowFavoriteBadge = m_owner->IsRowFavorite(position);
                auto* iconRow = Cast<ui::FlexLayout>(tile->GetChildAt(0));
                auto* name = static_cast<NameLabel*>(tile->GetChildAt(1));
                if (iconRow == nullptr || iconRow->ChildCount() < 1 || name == nullptr)
                {
                    return;
                }
                auto* iconView = Cast<ui::DrawableView>(iconRow->GetChildAt(0));
                const Row* row = m_owner->RowAt(position);
                if (iconView == nullptr || row == nullptr)
                {
                    return;
                }
                iconView->Drawable = ui::DrawablePtr(m_owner->RowIcon(position));
                name->BindTarget(row->id, row->group);
                // The tile has no meta label: the name color carries the COOK status (favorite +
                // export show as corner dots) and stays PURE (it is the inline-rename edit text).
                String text;
                Color color{0.85f, 0.85f, 0.85f, 1.0f};
                m_owner->RowName(position, text, color);
                name->SetText(text.AsView());
                name->TextColor.SetValue(Optional<Color>(color));
            }

        private:
            AssetsView* m_owner;
        };

        // === model ===

        i32 AddGroupNode(content::Group* group, i32 depth);

        void SelectGroup(content::Group* group);

        void RebuildList();

        void CollectFiltered(content::Group* group);

        [[nodiscard]] static bool MatchesFilter(StringView name, StringView filter);

        [[nodiscard]] const Row* RowAt(i32 position) const;

        [[nodiscard]] content::Instance* InstanceAt(i32 position);

        [[nodiscard]] ui::Drawable* RowIcon(i32 position);

        // The name text stays PURE (it doubles as the inline-rename edit text). Favorite +
        // export status are shown by the AssetCell corner dots (gold / green), so the name color
        // is free to always carry the COOK status - even for favorited items.
        void RowName(i32 position, String& text, Color& color);

        // The trailing meta label (list mode only): type + cook badge, badge-colored.
        void RowMeta(i32 position, String& text, Color& color);

        // === navigation ===

        void ActivateRow(i32 position, i32 clickCount);

        void UpdateBreadcrumb();

        void NavigateToBreadcrumb(i32 segment);

        void SetGridMode(bool grid);

        // === menus ===

        // Selected INSTANCE ids in the active view (group rows never join the selection set for
        // destructive actions; `clicked` is always included).
        [[nodiscard]] Array<Guid> SelectedInstanceIds(ui::SelectionModel* selection, i32 clicked);

        void ShowRowMenu(ui::View* anchor, ui::SelectionModel* selection, i32 position, f32 x,
                         f32 y);

        void ShowBackgroundMenu(ui::View* anchor, f32 x, f32 y);

        // === "Always Export" roots (docs/design/export-reachability.md §2) ===
        // A user flags an asset (or a whole group subtree) as an export root; its dependency
        // closure then ships even with reachability pruning on. Stored centrally on the project
        // (export_roots.xml, committed) and saved immediately on toggle - deliberate, rare, and
        // auditable in one place.

        [[nodiscard]] bool IsInstanceExportRoot(const Guid& id) const;
        // Row-level: a directly-flagged instance OR a flagged group row (the corner-dot badge).
        [[nodiscard]] bool IsRowExportRoot(i32 position);
        // Row-level favorite (instances only; groups are never favorites) - the gold corner dot.
        [[nodiscard]] bool IsRowFavorite(i32 position);
        [[nodiscard]] bool IsGroupExportRoot(content::Group* group) const;

        void ToggleInstanceExportRoot(const Guid& id);
        void ToggleGroupExportRoot(content::Group* group);

        // Persist + surface + refresh after a flag toggle. A failed save reverts the in-memory
        // change so the badge never claims a state that isn't on disk.
        void AfterExportRootChange(bool nowRoot, StringView name, bool isGroup);

        void CreateGroupIn(content::Group* parent);

        // === actions ===

        void DuplicateInstance(const Guid& id);

        // === inline rename ===

        // NameLabel commit handler: route to the instance or group apply.
        void ApplyRename(NameLabel* label, StringView newName);

        void ApplyRenameInstance(const Guid& id, StringView name);

        void ApplyRenameGroup(content::Group* group, StringView name);

        // Begin the in-place edit of a content-area row (menu Rename path is DOUBLE-deferred
        // through the mutation queue - Sedulous lesson: BeginEdit's SetFocus must land AFTER
        // the menu's ClosePopup/PopFocus restored focus, one queue drain is not enough).
        void StartRename(i32 position);

        void StartRenameDeferred(i32 position);

        // In-place edit of a group's TREE row (the background/tree menu path).
        void StartRenameGroupInTree(content::Group* group);

        void StartRenameGroupInTreeDeferred(content::Group* group);

        void ConfirmDelete(Array<Guid> ids);

        // Runs from the mutation queue: close pages, delete, log, refresh.
        void DeleteInstances(const Array<Guid>& ids);

        // F2 = inline rename, Delete = confirmed delete - dispatched by ListView/GridView
        // before their own navigation keys.
        void OnRowKeyDown(ui::SelectionModel* selection, i32 position, ui::KeyEventArgs& e);

        void ConfirmDeleteGroup(content::Group* group);

        // Runs from the mutation queue: close every page under the group, delete the whole
        // subtree, navigate the selection out of the dead branch.
        void DeleteGroupNow(content::Group* group);

        static void CountInstances(content::Group* group, usize& count);

        static void CollectInstanceIds(content::Group* group, Array<Guid>& out);

        // A delete may have taken the default scene with it - clear the manifest reference
        // instead of leaving a dangling guid (the player would fail with "unresolved").
        void ClearDefaultSceneIfGone();

        static void AppendCount(String& out, usize value);

        [[nodiscard]] content::Instance* Resolve(const Guid& id);

        draconic::editor::EditorContext* m_context;  // borrowed
        draconic::editor::EditorCookService* m_cook; // borrowed (app-owned)
        draconic::editor::EditorJobService* m_jobs = nullptr;
        RefPtr<ui::TreeView> m_tree;
        RefPtr<ui::ListView> m_list;
        RefPtr<ui::GridView> m_grid;
        RefPtr<ui::EditText> m_filterEdit;
        RefPtr<ui::toolkit::BreadcrumbBar> m_breadcrumb;
        RefPtr<ui::ToggleButton> m_listToggle;
        RefPtr<ui::ToggleButton> m_gridToggle;
        UniquePtr<TreeAdapter> m_treeAdapter;
        UniquePtr<ListAdapter> m_listAdapter;
        UniquePtr<GridAdapter> m_gridAdapter;
        Array<GroupNode> m_groups;                 // pre-order snapshot (nodeId = index)
        Array<Row> m_rows;                         // content area: subgroups then instances
        Array<content::Group*> m_breadcrumbGroups; // segment index -> group
        content::Group* m_selectedGroup = nullptr;
        String m_filter;
        bool m_gridMode = false;
        u64 m_cookRevision = ~0ull;
    };

    DRACONIC_DEFINE_OBJECT(AssetsView, "draconic::editor::app")
}
