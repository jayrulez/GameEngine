// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :collision_matrix_editor partition.
//
// The physics block's collision groups as a grid, with a row and a column per group. The column
// headers are the groups' numbers (each named in its tooltip), the rows show numbers and names, and
// a check box sits where row and column meet on and below the diagonal, so each pair has one box
// saying whether the two collide. A click writes the pair through the groups' source (both ways,
// one undo step). The grid's row for this editor is the source's `matrix` (hinted
// "CollisionMatrix"), beside its `groups` list, whose rows give each group's name; the editor reads
// the masks from the list's rows.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:collision_matrix_editor;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;

export namespace editor
{
    class CollisionMatrixEditor final : public foundation::gui::PropertyEditor
    {
        RTTI_OBJECT(CollisionMatrixEditor, foundation::gui::PropertyEditor)
    public:
        static constexpr StringView kHint = u8"CollisionMatrix";
        static constexpr f32 kCell = 20.0f;
        static constexpr f32 kNameWidth = 120.0f;

        CollisionMatrixEditor() : m_names(MemoryAllocator()), m_boxes(MemoryAllocator())
        {
            AddClass(u8"collision-matrix");
            EditLayoutStyle([](foundation::gui::LayoutStyle& s)
            {
                s.Direction = foundation::gui::FlexDirection::Column;
                s.RowGap = 2.0f;
            });
        }

        static void Register(foundation::gui::PropertyEditorRegistry& registry)
        {
            registry.RegisterHint(kHint, [](IAllocator& allocator) -> RefPtr<foundation::gui::PropertyEditor>
                                  { return MakeRef<CollisionMatrixEditor>(allocator); }, true);
        }

        /// How many groups it shows.
        [[nodiscard]] usize Count() const noexcept { return m_names.Size(); }
        /// The box for the pair (`row`, `column`), `column` <= `row`; null: none.
        [[nodiscard]] foundation::gui::CheckBox* Box(usize row, usize column) const
        {
            if (column > row || row >= m_names.Size())
                return nullptr;
            return m_boxes[row * (row + 1) / 2 + column];
        }

    protected:
        void OnBind(const foundation::gui::PropertyInfo& info, const foundation::gui::PropertyValue& value) override
        {
            (void)info;
            const usize count = static_cast<usize>(Max<i64>(value.Value.AsInt(), 0));
            Array<String> names(MemoryAllocator());
            for (usize i = 0; i < count; ++i)
                names.PushBack(String(Read(Format(u8"{}.name", i).AsView(), foundation::gui::PropertyKind::String).Value.AsString()));
            bool same = names.Size() == m_names.Size();
            for (usize i = 0; same && i < names.Size(); ++i)
                same = names[i] == m_names[i];
            if (!same)
                Build(foundation::core::Move(names));
            // Sets each box from its pair's current state (a row's mask; the source keeps it both
            // ways).
            m_binding = true;
            for (usize row = 0; row < count; ++row)
            {
                const u64 mask = static_cast<u64>(Read(Format(u8"{}.collides", row).AsView(), foundation::gui::PropertyKind::Flags).Value.AsInt());
                for (usize column = 0; column <= row; ++column)
                    if (foundation::gui::CheckBox* box = Box(row, column))
                    {
                        const bool on = (mask & (u64{1} << column)) != 0;
                        if (box->IsChecked() != on)
                            box->SetChecked(on);
                        box->SetEnabled(!IsReadOnly());
                    }
            }
            m_binding = false;
        }

    private:
        // The groups list's path beside its own: "…/matrix" reads "…/groups.<i>.<row>".
        [[nodiscard]] String GroupPath(StringView rest) const
        {
            const StringView own = GetPath();
            usize slash = own.Size();
            for (usize i = 0; i < own.Size(); ++i)
                if (own[i] == u8'/')
                    slash = i;
            const StringView prefix = slash < own.Size() ? own.SubStr(0, slash + 1) : StringView{};
            return Format(u8"{}groups.{}", prefix, rest);
        }
        [[nodiscard]] foundation::gui::PropertyValue Read(StringView rest, foundation::gui::PropertyKind kind) const
        {
            foundation::gui::IPropertyEditHost* host = GetHost();
            return host != nullptr ? host->ReadPropertyValue(GroupPath(rest).AsView(), kind) : foundation::gui::PropertyValue{};
        }
        // Writes the pair through the row's `collides` (the source writes both ways).
        void Write(usize row, usize column, bool collides)
        {
            foundation::gui::IPropertyEditHost* host = GetHost();
            if (m_binding || host == nullptr || IsReadOnly())
                return;
            const String path = GroupPath(Format(u8"{}.collides", row).AsView());
            foundation::gui::PropertyWrite write;
            write.Value = foundation::gui::Variant(static_cast<i64>(collides ? u64{1} << column : 0));
            write.Mask = u64{1} << column;
            host->BeginPropertyEdit(path.AsView());
            host->CommitPropertyEdit(path.AsView(), write);
        }

        // Builds the headers and one row per group, with a box at each pair on and below the
        // diagonal.
        void Build(Array<String> names)
        {
            while (ChildCount() > 0)
                RemoveChild(GetChildAt(ChildCount() - 1));
            m_boxes.Clear();
            m_names = foundation::core::Move(names);
            const usize count = m_names.Size();
            const auto cell = [](foundation::gui::UIWidget& widget, f32 width)
            {
                widget.EditLayoutStyle([width](foundation::gui::LayoutStyle& s)
                {
                    s.Width = foundation::gui::LayoutLength::Px(width);
                    s.Shrink = 0.0f;
                });
            };
            const auto row = [this]() -> RefPtr<foundation::gui::UIWidget>
            {
                RefPtr<foundation::gui::UIWidget> line = MakeRef<foundation::gui::UIWidget>(MemoryAllocator());
                line->AddClass(u8"collision-matrix-row");
                line->EditLayoutStyle([](foundation::gui::LayoutStyle& s)
                {
                    s.Direction = foundation::gui::FlexDirection::Row;
                    s.AlignItems = foundation::gui::Align::Center;
                    s.Height = foundation::gui::LayoutLength::Px(kCell);
                });
                AddChild(line.Get());
                return line;
            };
            RefPtr<foundation::gui::UIWidget> header = row();
            RefPtr<foundation::gui::Label> corner = MakeRef<foundation::gui::Label>(MemoryAllocator());
            cell(*corner, kNameWidth);
            header->AddChild(corner.Get());
            for (usize column = 0; column < count; ++column)
            {
                RefPtr<foundation::gui::Label> number = MakeRef<foundation::gui::Label>(MemoryAllocator());
                number->AddClass(u8"collision-matrix-column");
                number->SetText(Format(u8"{}", column).AsView());
                number->SetTooltip(m_names[column].AsView());
                cell(*number, kCell);
                header->AddChild(number.Get());
            }
            CollisionMatrixEditor* self = this;
            for (usize r = 0; r < count; ++r)
            {
                RefPtr<foundation::gui::UIWidget> line = row();
                RefPtr<foundation::gui::Label> name = MakeRef<foundation::gui::Label>(MemoryAllocator());
                name->AddClass(u8"collision-matrix-name");
                name->SetText(Format(u8"{} {}", r, m_names[r]).AsView());
                cell(*name, kNameWidth);
                line->AddChild(name.Get());
                for (usize column = 0; column <= r; ++column)
                {
                    RefPtr<foundation::gui::CheckBox> box = MakeRef<foundation::gui::CheckBox>(MemoryAllocator());
                    box->SetTooltip(Format(u8"{} and {} collide", m_names[r], m_names[column]).AsView());
                    box->SetOnToggled([self, r, column](bool on) { self->Write(r, column, on); });
                    cell(*box, kCell);
                    line->AddChild(box.Get());
                    m_boxes.PushBack(box.Get());
                }
            }
        }

        Array<String> m_names;                      // the groups, as last built
        Array<foundation::gui::CheckBox*> m_boxes;  // the lower triangle's boxes (its children) by row
        bool m_binding = false;                     // set while showing the masks, not on a click
    };

    RTTI_DEFINE_OBJECT(CollisionMatrixEditor, "rtti::editor::editor")
}
