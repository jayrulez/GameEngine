// Draconic::EditorApp - :asset_picker_dialog partition.
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:asset_picker_dialog;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :editor_icons;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;
    namespace content = draconic::content;

    class AssetPickerDialog final : public ui::Dialog
    {
        DRACONIC_OBJECT(AssetPickerDialog, ui::Dialog)
    public:
        /// The pick result: an instance id, or nil for [Clear]. Fired once, before close.
        Function<void(const Guid&)> OnPicked;

        AssetPickerDialog(draconic::editor::EditorContext& context, Array<String> assetTypeNames)
            : ui::Dialog(u8"Select asset"), m_context(&context), m_typeNames(Move(assetTypeNames))
        {
            MinWidth.SetValue(520.0f);
            MinHeight.SetValue(380.0f);
            MaxWidth.SetValue(640.0f);
            MaxHeight.SetValue(460.0f);

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            m_filterEdit = MakeRef<ui::EditText>(DefaultAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter all groups...");
            {
                AssetPickerDialog* self = this;
                m_filterEdit->OnTextChanged.Add(
                    [self](ui::EditText* edit)
                    {
                        self->m_filter = String(edit->Text());
                        self->RebuildList();
                    });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_filterEdit.Get(), lp);
            }

            auto split = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
            split->SetSplitRatio(0.32f);
            m_treeAdapter = MakeUnique<TreeAdapter>(DefaultAllocator(), *this);
            m_tree = MakeRef<ui::TreeView>(DefaultAllocator());
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
            m_listAdapter = MakeUnique<ListAdapter>(DefaultAllocator(), *this);
            m_list = MakeRef<ui::ListView>(DefaultAllocator());
            m_list->ItemHeight.SetValue(20.0f);
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
                                               { self->ShowRowMenu(position, x, y); });
            }
            split->SetPanes(m_tree.Get(), m_list.Get());
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                column->AddView(split.Get(), lp);
            }
            SetContent(column.Get());

            {
                AssetPickerDialog* self = this;
                ui::Button* select = AddButton(u8"Select", ui::DialogResult::None);
                select->OnClick.Add([self](ui::ButtonBase*)
                                    { self->ConfirmAt(self->m_list->Selection.FirstSelected()); });
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
        }

        ~AssetPickerDialog() override
        {
            m_tree->SetAdapter(nullptr);
            m_list->SetAdapter(nullptr);
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
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                auto label = MakeRef<ui::Label>(DefaultAllocator());
                label->FontSize.SetValue(12.0f);
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
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
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6;
                row->Padding = ui::Thickness{4, 2};
                auto iconView = MakeRef<ui::DrawableView>(DefaultAllocator());
                iconView->DesiredWidth.SetValue(Optional<f32>(14.0f));
                iconView->DesiredHeight.SetValue(Optional<f32>(14.0f));
                row->AddView(iconView.Get());
                auto label = MakeRef<ui::Label>(DefaultAllocator());
                label->FontSize.SetValue(12.0f);
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
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
                iconView->Drawable =
                    ui::DrawablePtr(EditorIcons::Get().ForAssetType(instance->TypeName()));
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

        // === model ===

        [[nodiscard]] bool TypeMatches(content::Instance& instance) const;

        i32 AddGroupNode(content::Group* group, i32 depth);

        void RebuildModel();

        void RebuildList();

        void CollectGroup(content::Group& group, bool recurse);

        void CollectFiltered(content::Group* group);

        [[nodiscard]] static bool MatchesFilter(StringView name, StringView filter);

        [[nodiscard]] content::Instance* Resolve(const Guid& id);

        // === actions ===

        void ConfirmAt(i32 position);

        void ShowRowMenu(i32 position, f32 x, f32 y);

        draconic::editor::EditorContext* m_context; // borrowed
        Array<String> m_typeNames;
        RefPtr<ui::TreeView> m_tree;
        RefPtr<ui::ListView> m_list;
        RefPtr<ui::EditText> m_filterEdit;
        UniquePtr<TreeAdapter> m_treeAdapter;
        UniquePtr<ListAdapter> m_listAdapter;
        Array<GroupNode> m_groups;
        Array<Guid> m_rows;
        content::Group* m_selectedGroup = nullptr;
        String m_filter;
    };

    DRACONIC_DEFINE_OBJECT(AssetPickerDialog, "draconic::editor::app")
}
