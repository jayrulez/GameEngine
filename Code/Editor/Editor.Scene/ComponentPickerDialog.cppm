// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :component_picker_dialog partition.
//
// ComponentPickerDialog: the inspector's Add Component, a picker like the asset and entity
// pickers rather than a menu of category submenus (user 2026-10-10; PaperKid's "organize the
// new-component menu"). A search field at the top, focused as it opens; below it the categories,
// open, each with its components, narrowed as you type to the components whose name, or whose
// category's, holds the text. The first match is selected, so typing a name and pressing Enter
// adds it; a double-click or [Add] adds the selected one. The caller hands in the choices (the
// components the entity does not have yet, by their authored names and categories) and hears the
// pick through OnPicked, fired before the dialog closes. Under the list, the selected component's
// description (its type's "description" attribute, beside its displayName and category).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:component_picker_dialog;

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core; // TextContains (the editor's filter fields)

using namespace foundation::core;

export namespace editor
{
    namespace ui = foundation::ui;

    /// A component the picker offers: its authored name and category, and its type.
    struct ComponentChoice
    {
        String label;
        String category;
        const TypeInfo* type = nullptr;
        String description; // what it does for the entity (its type's "description"); may be empty
    };

    class ComponentPickerDialog final : public ui::Dialog
    {
        RTTI_OBJECT(ComponentPickerDialog, ui::Dialog)
    public:
        /// The component picked. Fired once, before close.
        Function<void(const TypeInfo*)> OnPicked;

        /// `choices` in the order to show them (by category, then name).
        explicit ComponentPickerDialog(Array<ComponentChoice> choices)
            : ui::Dialog(u8"Add Component"), m_choices(Move(choices))
        {
            // A fixed size: the list narrows as you type, the dialog does not.
            MinWidth.SetValue(380.0f);
            MaxWidth.SetValue(380.0f);
            MinHeight.SetValue(460.0f);
            MaxHeight.SetValue(460.0f);

            auto column = MakeRef<ui::FlexLayout>(MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;
            ComponentPickerDialog* self = this;

            m_filterEdit = MakeRef<ui::EditText>(MemoryAllocator());
            m_filterEdit->SetPlaceholder(u8"Search components...");
            m_filterEdit->OnTextChanged.Add(
                [self](ui::EditText* edit)
                {
                    self->m_filter = String(edit->Text());
                    self->RebuildTree();
                });
            m_filterEdit->OnSubmit.Add([self](ui::EditText*) { self->Pick(); }); // Enter adds the match
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(m_filterEdit.Get(), lp);
            }

            m_adapter = MakeUnique<Adapter>(MemoryAllocator(), *this);
            m_tree = MakeRef<ui::TreeView>(MemoryAllocator());
            m_tree->SetItemHeight(22.0f);
            m_tree->SetAdapter(m_adapter.Get());
            m_tree->OnItemClick.Add(
                [self](ui::TreeView::ItemClickInfo info)
                {
                    if (info.NodeId < 0 || info.NodeId >= static_cast<i32>(self->m_nodes.Size()) ||
                        self->m_nodes[static_cast<usize>(info.NodeId)].choice < 0)
                    {
                        return; // a category heading
                    }
                    self->m_selected = info.NodeId;
                    self->ShowDescription();
                    if (info.ClickCount >= 2)
                    {
                        self->Pick();
                    }
                });
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                column->AddView(m_tree.Get(), lp);
            }
            m_emptyNote = MakeRef<ui::Label>(MemoryAllocator(), StringView(u8"No component matches."));
            m_emptyNote->FontSize.SetValue(11.0f);
            m_emptyNote->TextColor.SetValue(Optional<Color>(Color{0.62f, 0.62f, 0.62f, 1.0f}));
            column->AddView(m_emptyNote.Get());
            // The selected component, said in a sentence under the list.
            m_description = MakeRef<ui::Label>(MemoryAllocator(), StringView(u8""));
            m_description->FontSize.SetValue(11.5f);
            m_description->WordWrap.SetValue(true);
            m_description->TextColor.SetValue(Optional<Color>(Color{0.75f, 0.75f, 0.75f, 1.0f}));
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(34.0f)); // two lines, steady as it changes
                column->AddView(m_description.Get(), lp);
            }
            SetContent(column.Get());

            ui::Button* add = AddButton(u8"Add", ui::DialogResult::None);
            add->OnClick.Add([self](ui::ButtonBase*) { self->Pick(); });
            AddButton(u8"Cancel", ui::DialogResult::Cancel);

            RebuildTree();
        }

        ~ComponentPickerDialog() override { m_tree->SetAdapter(nullptr); }

        /// Add the selected component (none selected: nothing happens).
        void Pick()
        {
            const TypeInfo* type = SelectedType();
            if (type == nullptr)
            {
                return;
            }
            if (OnPicked)
            {
                OnPicked(type);
            }
            Close(ui::DialogResult::OK);
        }

        /// For tests: the search field, the components shown, and the one Add would add.
        [[nodiscard]] ui::EditText& FilterField() const noexcept { return *m_filterEdit; }
        [[nodiscard]] usize ShownComponentCount() const
        {
            usize count = 0;
            for (const Node& node : m_nodes)
            {
                count += node.choice >= 0 ? 1u : 0u;
            }
            return count;
        }
        /// The description shown for the selected component.
        [[nodiscard]] StringView DescriptionText() const { return m_description->Text.Value().AsView(); }
        [[nodiscard]] const TypeInfo* SelectedType() const
        {
            if (m_selected < 0 || m_selected >= static_cast<i32>(m_nodes.Size()))
            {
                return nullptr;
            }
            const i32 choice = m_nodes[static_cast<usize>(m_selected)].choice;
            return choice >= 0 ? m_choices[static_cast<usize>(choice)].type : nullptr;
        }

    private:
        /// A category heading (choice < 0, its components below) or a component.
        struct Node
        {
            i32 choice = -1;
            String text;
            Array<i32> children;
        };

        class Adapter final : public ui::ITreeAdapter
        {
        public:
            explicit Adapter(ComponentPickerDialog& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 RootCount() const override { return static_cast<i32>(m_owner->m_roots.Size()); }
            [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
            {
                if (nodeId == -1)
                {
                    return RootCount();
                }
                return InRange(nodeId) ? static_cast<i32>(m_owner->m_nodes[static_cast<usize>(nodeId)].children.Size())
                                       : 0;
            }
            [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
            {
                if (parentId == -1)
                {
                    return (childIndex >= 0 && childIndex < RootCount()) ? m_owner->m_roots[static_cast<usize>(childIndex)]
                                                                         : -1;
                }
                if (!InRange(parentId))
                {
                    return -1;
                }
                const Array<i32>& kids = m_owner->m_nodes[static_cast<usize>(parentId)].children;
                return (childIndex >= 0 && childIndex < static_cast<i32>(kids.Size())) ? kids[static_cast<usize>(childIndex)]
                                                                                        : -1;
            }
            [[nodiscard]] i32 GetDepth(i32 nodeId) const override
            {
                return InRange(nodeId) && m_owner->m_nodes[static_cast<usize>(nodeId)].choice >= 0 ? 1 : 0;
            }
            [[nodiscard]] bool HasChildren(i32 nodeId) const override { return GetChildCount(nodeId) > 0; }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<ui::FlexLayout>(m_owner->MemoryAllocator());
                auto label = MakeRef<ui::Label>(m_owner->MemoryAllocator());
                label->FontSize.SetValue(Optional<f32>{12.0f});
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                grow.AlignSelf = ui::Align::Center;
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
                const Node& node = m_owner->m_nodes[static_cast<usize>(nodeId)];
                label->SetText(node.text.AsView());
                // Headings quieter than the components they hold.
                label->TextColor.SetValue(node.choice < 0 ? Optional<Color>(Color{0.62f, 0.62f, 0.62f, 1.0f})
                                                          : Optional<Color>());
                row->Padding = ui::Thickness{m_owner->m_tree->ContentInset(depth), 0, 0, 0};
            }

        private:
            [[nodiscard]] bool InRange(i32 nodeId) const
            {
                return nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_nodes.Size());
            }
            ComponentPickerDialog* m_owner;
        };

        void RebuildTree()
        {
            m_nodes.Clear();
            m_roots.Clear();
            m_selected = -1;
            // The choices arrive grouped by category: a heading opens each run of them that has a
            // component showing. A category's name finds all of it.
            i32 heading = -1;
            StringView headingName;
            for (usize i = 0; i < m_choices.Size(); ++i)
            {
                const ComponentChoice& choice = m_choices[i];
                if (!TextContains(choice.label.AsView(), m_filter.AsView()) &&
                    !TextContains(choice.category.AsView(), m_filter.AsView()))
                {
                    continue;
                }
                if (heading < 0 || choice.category.AsView() != headingName)
                {
                    Node node;
                    node.text = choice.category;
                    heading = static_cast<i32>(m_nodes.Size());
                    headingName = choice.category.AsView();
                    m_nodes.PushBack(Move(node));
                    m_roots.PushBack(heading);
                }
                Node item;
                item.choice = static_cast<i32>(i);
                item.text = choice.label;
                const i32 id = static_cast<i32>(m_nodes.Size());
                m_nodes.PushBack(Move(item));
                m_nodes[static_cast<usize>(heading)].children.PushBack(id);
                if (m_selected < 0)
                {
                    m_selected = id; // the first match: Enter adds it
                }
            }
            m_emptyNote->Visibility = m_nodes.IsEmpty() ? ui::Visibility::Visible : ui::Visibility::Gone;
            if (ui::FlattenedTreeAdapter* flat = m_tree->FlatAdapter())
            {
                for (i32 root : m_roots)
                {
                    flat->Expand(root);
                }
                flat->RebuildVisibleList();
                for (i32 pos = 0; pos < flat->ItemCount(); ++pos)
                {
                    if (flat->GetNodeId(pos) == m_selected)
                    {
                        m_tree->Selection().Select(pos);
                        m_tree->InternalListView()->ScrollToPosition(pos);
                        break;
                    }
                }
            }
            ShowDescription();
        }

        void ShowDescription()
        {
            const i32 choice = (m_selected >= 0 && m_selected < static_cast<i32>(m_nodes.Size()))
                                   ? m_nodes[static_cast<usize>(m_selected)].choice
                                   : -1;
            m_description->SetText(choice >= 0 ? m_choices[static_cast<usize>(choice)].description.AsView()
                                               : StringView(u8""));
        }

        Array<ComponentChoice> m_choices;
        RefPtr<ui::EditText> m_filterEdit;
        RefPtr<ui::TreeView> m_tree;
        RefPtr<ui::Label> m_emptyNote;
        RefPtr<ui::Label> m_description;
        UniquePtr<Adapter> m_adapter;
        Array<Node> m_nodes;
        Array<i32> m_roots;
        i32 m_selected = -1;
        String m_filter;
    };

    RTTI_DEFINE_OBJECT(ComponentPickerDialog, "rtti::editor::editor.scene")
}
