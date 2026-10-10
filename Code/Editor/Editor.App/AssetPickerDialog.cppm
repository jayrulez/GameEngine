// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :asset_picker_dialog partition.
//
// AssetPickerDialog: a modal, READ-ONLY mirror of the asset browser for resource-ref picking -
// group tree on the left, matching instances on the right, filter across all groups. Replaces
// the flat context-menu picker (which grew unusable once same-named assets lived in different
// groups). Deliberately not the browser itself: no rename (Sedulous's picker made slow-clicks
// start renames - the recorded annoyance), no delete, no cook actions.
//
//   - only instances whose type is in `assetTypeNames` are listed
//   - FAVORITES (pinned via right-click here or anywhere EditorContext favorites reach) sort
//     first with a * prefix, regardless of the selected group
//   - double-click or [Select] confirms; [Clear] picks "none"; [Cancel]/Escape dismisses
//
// The result is delivered through OnPicked(guid) (nil = cleared), fired BEFORE the dialog
// closes itself.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:asset_picker_dialog;

import foundation.core;
import foundation.content;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.fonts; // TextAlignment (a tile name centred)
import editor.core;
import :editor_icons;
import :view_mode_toggles;
import :asset_filter;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;
    namespace content = foundation::content;

    class AssetPickerDialog final : public ui::Dialog
    {
        RTTI_OBJECT(AssetPickerDialog, ui::Dialog)
    public:
        /// The pick result: an instance id, or nil for [Clear]. Fired once, before close.
        Function<void(const Guid&)> OnPicked;

        AssetPickerDialog(editor::EditorContext& context, Array<String> assetTypeNames)
            : ui::Dialog(u8"Select asset"), m_context(&context), m_typeNames(Move(assetTypeNames))
        {
            MinWidth.SetValue(520.0f);
            MinHeight.SetValue(380.0f);
            MaxWidth.SetValue(640.0f);
            MaxHeight.SetValue(460.0f);

            auto column = MakeRef<ui::FlexLayout>(MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            m_filterEdit = MakeRef<ui::EditText>(MemoryAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter all groups...");
            {
                AssetPickerDialog* self = this;
                m_filterEdit->OnTextChanged.Add(
                    [self](ui::EditText* edit)
                    {
                        self->m_filter = String(edit->Text());
                        self->RebuildList();
                    });
            }
            {
                // The filter, and the List / Grid toggles beside it.
                auto header = MakeRef<ui::FlexLayout>(MemoryAllocator());
                header->Direction = ui::Orientation::Horizontal;
                header->Spacing = 6;
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                grow.AlignSelf = ui::Align::Center;
                header->AddView(m_filterEdit.Get(), grow);
                m_viewToggles = MakeRef<ViewModeToggles>(MemoryAllocator());
                AssetPickerDialog* self = this;
                m_viewToggles->OnModeChanged = [self](bool grid) { self->SetGridMode(grid); };
                ui::LayoutStyle center;
                center.AlignSelf = ui::Align::Center;
                header->AddView(m_viewToggles.Get(), center);
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(header.Get(), lp);
            }
            {
                m_hint = MakeRef<ui::Label>(MemoryAllocator());
                m_hint->FontSize.SetValue(12.0f);
                m_hint->TextColor.SetValue(Optional<Color>(Color{1.0f, 0.75f, 0.4f, 1.0f}));
                m_hint->Visibility = ui::VisibilityValue::Gone;
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(m_hint.Get(), lp);
            }

            auto split = MakeRef<ui::toolkit::SplitView>(MemoryAllocator());
            split->SetSplitRatio(0.32f);
            m_treeAdapter = MakeUnique<TreeAdapter>(MemoryAllocator(), *this);
            m_tree = MakeRef<ui::TreeView>(MemoryAllocator());
            m_tree->SetItemHeight(20.0f);
            m_tree->SetAdapter(m_treeAdapter.Get());
            {
                AssetPickerDialog* self = this;
                m_tree->OnItemClick.Add(
                    [self](ui::TreeView::ItemClickInfo info)
                    {
                        if (info.NodeId >= 0 &&
                            info.NodeId < static_cast<i32>(self->m_groups.Size()))
                        {
                            self->m_selectedGroup =
                                self->m_groups[static_cast<usize>(info.NodeId)].group;
                            self->RebuildList();
                        }
                    });
            }
            m_listAdapter = MakeUnique<ListAdapter>(MemoryAllocator(), *this);
            m_list = MakeRef<ui::ListView>(MemoryAllocator());
            m_list->ItemHeight.SetValue(28.0f); // rows carry a 24px thumbnail, not a 14px glyph
            m_list->SetAdapter(m_listAdapter.Get());
            {
                AssetPickerDialog* self = this;
                m_list->OnItemClicked.Add(
                    [self](i32 position, i32 clickCount, f32, f32)
                    {
                        if (clickCount >= 2)
                        {
                            self->ConfirmAt(position);
                        }
                    });
                m_list->OnItemRightClicked.Add([self](i32 position, f32 x, f32 y)
                                               { self->ShowRowMenu(self->m_list.Get(), position, x, y); });
            }
            // The grid: the same rows as tiles, a larger thumbnail over the name.
            m_gridAdapter = MakeUnique<GridAdapter>(MemoryAllocator(), *this);
            m_grid = MakeRef<ui::GridView>(MemoryAllocator());
            m_grid->CellWidth.SetValue(96.0f);
            m_grid->CellHeight.SetValue(84.0f);
            m_grid->Visibility = ui::VisibilityValue::Gone;
            m_grid->SetAdapter(m_gridAdapter.Get());
            {
                AssetPickerDialog* self = this;
                m_grid->OnItemClicked.Add(
                    [self](i32 position, i32 clickCount, f32, f32)
                    {
                        if (clickCount >= 2)
                        {
                            self->ConfirmAt(position);
                        }
                    });
                m_grid->OnItemRightClicked.Add([self](i32 position, f32 x, f32 y)
                                               { self->ShowRowMenu(self->m_grid.Get(), position, x, y); });
            }
            auto results = MakeRef<ui::FlexLayout>(MemoryAllocator());
            results->Direction = ui::Orientation::Vertical;
            {
                ui::LayoutStyle fill;
                fill.Width = ui::SizeSpec::Match();
                fill.FlexGrow = 1.0f;
                results->AddView(m_list.Get(), fill);
                results->AddView(m_grid.Get(), fill);
            }
            split->SetPanes(m_tree.Get(), results.Get());
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                column->AddView(split.Get(), lp);
            }
            SetContent(column.Get());

            {
                AssetPickerDialog* self = this;
                ui::Button* select = AddButton(u8"Select", ui::DialogResult::None);
                select->OnClick.Add([self](ui::ButtonBase*)
                                    { self->ConfirmAt(self->SelectedPosition()); });
                ui::Button* clear = AddButton(u8"Clear", ui::DialogResult::None);
                clear->OnClick.Add(
                    [self](ui::ButtonBase*)
                    {
                        if (self->OnPicked)
                        {
                            self->OnPicked(Guid{});
                        }
                        self->Close(ui::DialogResult::OK);
                    });
                AddButton(u8"Cancel", ui::DialogResult::Cancel);
            }

            RebuildModel();
            ApplySavedViewMode();
            // A thumbnail finishing while the dialog is open swaps into its row at once (Get
            // only queues a missing one; this is how its arrival is heard).
            m_thumbnailListener = m_context->AddThumbnailListener(
                [self = this](const Guid& id) { self->RefreshThumbnail(id); });
        }

        ~AssetPickerDialog() override
        {
            m_context->RemoveThumbnailListener(m_thumbnailListener);
            m_tree->SetAdapter(nullptr);
            m_list->SetAdapter(nullptr);
            m_grid->SetAdapter(nullptr);
        }

        /// A thumbnail finished for `id`: rebind its row, list and grid alike (no-op when the
        /// dialog does not list it).
        void RefreshThumbnail(const Guid& id);

        /// Show the rows as a list or as a grid of tiles; `persist` remembers it for the project.
        void SetGridMode(bool grid, bool persist = true);
        [[nodiscard]] bool IsGridMode() const noexcept { return m_gridMode; }
        /// The List / Grid toggles beside the filter.
        [[nodiscard]] ViewModeToggles* ViewToggles() const noexcept { return m_viewToggles.Get(); }
        /// The rows the dialog offers (instance ids), in the order both views show them.
        [[nodiscard]] Span<const Guid> Rows() const noexcept
        {
            return Span<const Guid>(m_rows.Data(), m_rows.Size());
        }
        [[nodiscard]] ui::ListView* RowList() const noexcept { return m_list.Get(); }
        [[nodiscard]] ui::GridView* RowGrid() const noexcept { return m_grid.Get(); }
        /// Filter as if `text` were typed into the filter field (names, or a guid or its start).
        void SetFilter(StringView text);
        /// The line under the filter: why a guid typed there lists nothing (empty when it does).
        [[nodiscard]] StringView FilterHint() const noexcept
        {
            return m_hint->Text.Value().AsView();
        }

    private:
        struct GroupNode
        {
            content::Group* group = nullptr;
            i32 depth = 0;
            Array<i32> children;
        };

        class TreeAdapter final : public ui::ITreeAdapter
        {
        public:
            explicit TreeAdapter(AssetPickerDialog& owner) : m_owner(&owner) {}
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
                auto row = MakeRef<ui::FlexLayout>(m_owner->MemoryAllocator());
                auto label = MakeRef<ui::Label>(m_owner->MemoryAllocator());
                label->FontSize.SetValue(12.0f);
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                row->AddView(label.Get(), grow);
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 nodeId, i32 depth, bool) override
            {
                auto* row = Cast<ui::FlexLayout>(view);
                if (row == nullptr || row->ChildCount() == 0 || !InRange(nodeId))
                {
                    return;
                }
                auto* label = Cast<ui::Label>(row->GetChildAt(0));
                const StringView name = Node(nodeId).group->Name();
                label->SetText(name.IsEmpty() ? StringView(u8"Content") : name);
                // Left-pad past the expander-chevron column; ContentInset(depth) tracks the tree's
                // IndentWidth so the padding can never drift from the chevron and overlap the text.
                row->Padding = ui::Thickness{m_owner->m_tree->ContentInset(depth), 0, 0, 0};
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
            AssetPickerDialog* m_owner;
        };

        class ListAdapter final : public ui::ListAdapterBase
        {
        public:
            explicit ListAdapter(AssetPickerDialog& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_rows.Size());
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<ui::FlexLayout>(m_owner->MemoryAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6;
                row->Padding = ui::Thickness{4, 2};
                auto iconView = MakeRef<ui::DrawableView>(m_owner->MemoryAllocator());
                iconView->DesiredWidth.SetValue(Optional<f32>(24.0f));
                iconView->DesiredHeight.SetValue(Optional<f32>(24.0f));
                row->AddView(iconView.Get());
                auto label = MakeRef<ui::Label>(m_owner->MemoryAllocator());
                label->FontSize.SetValue(12.0f);
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                row->AddView(label.Get(), grow);
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 position) override
            {
                auto* row = Cast<ui::FlexLayout>(view);
                if (row == nullptr || row->ChildCount() < 2 || position < 0 ||
                    position >= static_cast<i32>(m_owner->m_rows.Size()))
                {
                    return;
                }
                auto* iconView = Cast<ui::DrawableView>(row->GetChildAt(0));
                auto* label = Cast<ui::Label>(row->GetChildAt(1));
                if (iconView == nullptr || label == nullptr)
                {
                    return;
                }
                content::Instance* instance =
                    m_owner->Resolve(m_owner->m_rows[static_cast<usize>(position)]);
                if (instance == nullptr)
                {
                    return;
                }
                // Thumbnail wins; the type icon shows until one exists (the browser-grid
                // pattern). Get() also SCHEDULES a missing thumbnail - it appears on the
                // next rebind (scroll/filter/reopen); rows are passive re-queriers, like
                // the inspector's picker slots.
                iconView->Drawable = m_owner->IconFor(*instance);
                const bool favorite = m_owner->m_context->IsFavorite(instance->Id());
                String text;
                if (favorite)
                {
                    text.Append(u8"* ");
                }
                // Full path keeps same-named assets across groups distinguishable.
                text.Append(instance->Path());
                text.Append(u8"   -   ");
                text.Append(instance->TypeName());
                label->SetText(text.AsView());
                label->TextColor.SetValue(Optional<Color>(
                    favorite ? Color{1.0f, 0.85f, 0.45f, 1.0f} : Color{0.85f, 0.85f, 0.85f, 1.0f}));
            }

        private:
            AssetPickerDialog* m_owner;
        };

        // A tile: the thumbnail (or the type icon) centred over the name, which truncates.
        class GridAdapter final : public ui::ListAdapterBase
        {
        public:
            explicit GridAdapter(AssetPickerDialog& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_rows.Size());
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto tile = MakeRef<ui::FlexLayout>(m_owner->MemoryAllocator());
                tile->Direction = ui::Orientation::Vertical;
                tile->Padding = ui::Thickness{4, 4};
                auto iconRow = MakeRef<ui::FlexLayout>(m_owner->MemoryAllocator());
                iconRow->Direction = ui::Orientation::Horizontal;
                iconRow->JustifyContent = ui::Justify::Center;
                // Centred on the cross axis too, or the icon stretches to the row's height.
                iconRow->AlignItems = ui::Align::Center;
                auto iconView = MakeRef<ui::DrawableView>(m_owner->MemoryAllocator());
                iconView->DesiredWidth.SetValue(Optional<f32>(48.0f));
                iconView->DesiredHeight.SetValue(Optional<f32>(48.0f));
                iconRow->AddView(iconView.Get());
                auto label = MakeRef<ui::Label>(m_owner->MemoryAllocator());
                label->FontSize.SetValue(12.0f);
                label->HAlign.SetValue(foundation::fonts::TextAlignment::Center);
                label->Ellipsis.SetValue(true);
                {
                    ui::LayoutStyle grow;
                    grow.FlexGrow = 1.0f;
                    grow.Width = ui::SizeSpec::Match();
                    tile->AddView(iconRow.Get(), grow);
                }
                {
                    ui::LayoutStyle lp;
                    lp.Width = ui::SizeSpec::Match();
                    lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(18));
                    tile->AddView(label.Get(), lp);
                }
                return RefPtr<ui::View>(tile.Get());
            }
            void BindView(ui::View* view, i32 position) override
            {
                auto* tile = Cast<ui::FlexLayout>(view);
                if (tile == nullptr || tile->ChildCount() < 2 || position < 0 ||
                    position >= static_cast<i32>(m_owner->m_rows.Size()))
                {
                    return;
                }
                auto* iconRow = Cast<ui::FlexLayout>(tile->GetChildAt(0));
                auto* label = Cast<ui::Label>(tile->GetChildAt(1));
                if (iconRow == nullptr || iconRow->ChildCount() < 1 || label == nullptr)
                {
                    return;
                }
                auto* iconView = Cast<ui::DrawableView>(iconRow->GetChildAt(0));
                content::Instance* instance =
                    m_owner->Resolve(m_owner->m_rows[static_cast<usize>(position)]);
                if (iconView == nullptr || instance == nullptr)
                {
                    return;
                }
                iconView->Drawable = m_owner->IconFor(*instance);
                // A tile has room for the name alone; its tooltip carries the path and type the
                // list row shows.
                const bool favorite = m_owner->m_context->IsFavorite(instance->Id());
                label->SetText(instance->Name());
                label->TextColor.SetValue(Optional<Color>(
                    favorite ? Color{1.0f, 0.85f, 0.45f, 1.0f} : Color{0.85f, 0.85f, 0.85f, 1.0f}));
                String tip(instance->Path());
                tip.Append(u8"   -   ");
                tip.Append(instance->TypeName());
                tile->TooltipText = Move(tip);
            }

        private:
            AssetPickerDialog* m_owner;
        };

        // === model ===

        /// The thumbnail, or the type icon until one exists (the browser's pattern). The
        /// thumbnail service's Get() also SCHEDULES a missing one; it appears on the next rebind
        /// (scroll, filter, reopen): rows are passive re-queriers, like the inspector's slots.
        [[nodiscard]] ui::DrawablePtr IconFor(content::Instance& instance);

        /// The selected row in whichever view shows, or -1.
        [[nodiscard]] i32 SelectedPosition() const;

        /// Read the project's saved picker view mode and apply it (without saving it again).
        void ApplySavedViewMode();

        [[nodiscard]] bool TypeMatches(content::Instance& instance) const;

        i32 AddGroupNode(content::Group* group, i32 depth);

        void RebuildModel();

        void RebuildList();

        void CollectGroup(content::Group& group, bool recurse);

        void CollectFiltered(content::Group* group);

        [[nodiscard]] content::Instance* Resolve(const Guid& id);

        // === actions ===

        void ConfirmAt(i32 position);

        void ShowRowMenu(ui::View* source, i32 position, f32 x, f32 y);

        editor::EditorContext* m_context; // borrowed
        Array<String> m_typeNames;
        RefPtr<ui::TreeView> m_tree;
        RefPtr<ui::ListView> m_list;
        RefPtr<ui::GridView> m_grid;
        RefPtr<ViewModeToggles> m_viewToggles;
        RefPtr<ui::EditText> m_filterEdit;
        RefPtr<ui::Label> m_hint; // a guid's asset this slot does not take, said under the filter
        UniquePtr<TreeAdapter> m_treeAdapter;
        UniquePtr<ListAdapter> m_listAdapter;
        UniquePtr<GridAdapter> m_gridAdapter;
        bool m_gridMode = false;
        u64 m_thumbnailListener = 0;
        Array<GroupNode> m_groups;
        Array<Guid> m_rows;
        content::Group* m_selectedGroup = nullptr;
        String m_filter;
    };

    RTTI_DEFINE_OBJECT(AssetPickerDialog, "rtti::editor::editor::app")
}
