// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :editable_label partition
//
// EditableLabel: a label that becomes a field in place (E6 of Specs/gui-lab-evolution.md), a
// layer's name or a file's in a list. A double-click, or F2 while it is focused, edits it: a text
// field over its text, all of it selected. Return commits, as does focus leaving; Escape cancels.
// A validator may refuse what was typed: Return then keeps it editing (the field marked
// `.invalid`), and focus leaving puts the old text back. A committed change reports `change`.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:editable_label;

import foundation.core; // Function, Move, String, RefPtr
import :rect;
import :event;
import :draw_context;
import :label;
import :text_field;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class EditableLabel : public Label
    {
        RTTI_OBJECT(EditableLabel, Label)
    public:
        EditableLabel()
        {
            SetTag(core::StringView(u8"editablelabel"));
            SetTabFocusable(true);
        }

        /// Starts editing (as a double-click or F2 does).
        void BeginEdit()
        {
            if (m_editing || !IsTreeEnabled())
                return;
            EnsureEditor();
            m_editing = true;
            m_editor->SetText(GetText());
            m_editor->RemoveClass(core::StringView(u8"invalid"));
            m_editor->SetDisplayed(true);
            m_editor->GetModel().SelectAll();
            m_editor->RequestFocus();
            RequestLayout();
            Invalidate();
        }
        /// Ends editing: commits what was typed (if the validator takes it) or puts it back.
        /// False when the validator refused and it is still editing.
        bool EndEdit(bool commit)
        {
            if (!m_editing)
                return true;
            const core::String typed(m_editor->GetText());
            if (commit && m_validator && !m_validator(typed.AsView()))
            {
                m_editor->AddClass(core::StringView(u8"invalid"));
                return false;
            }
            m_editing = false;
            const bool hadFocus = m_editor->IsFocused();
            m_editor->SetDisplayed(false);
            if (hadFocus)
                RequestFocus(); // the label keeps the focus the field had
            if (commit && typed.AsView() != GetText())
            {
                SetText(typed.AsView());
                if (m_onCommitted)
                    m_onCommitted(typed.AsView());
                SendEvent(Event(EventType::Changed, this));
            }
            Invalidate();
            return true;
        }
        [[nodiscard]] bool IsEditing() const noexcept { return m_editing; }
        /// The field it edits in (null until it first edits).
        [[nodiscard]] TextField* GetEditor() const noexcept { return m_editor.Get(); }

        /// Decides whether typed text may be committed (false: it stays editing).
        void SetValidator(core::Function<bool(core::StringView)> validator) { m_validator = core::Move(validator); }
        /// A change the user committed.
        void SetOnTextCommitted(core::Function<void(core::StringView)> callback) { m_onCommitted = core::Move(callback); }

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left && event.ClickCount == 2 && event.Target == this)
            {
                BeginEdit();
                event.Handled = true;
            }
        }
        void OnKeyDown(const KeyEvent& event) override
        {
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (event.Target == this && key == KeyCode::F2 && event.Modifiers == 0)
            {
                BeginEdit();
                event.Handled = true;
            }
            else if (m_editing && event.Target == m_editor.Get() && key == KeyCode::Escape)
            {
                (void)EndEdit(false);
                event.Handled = true;
            }
        }
        // While it edits, the field draws the text; the label draws its box alone.
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (!m_editing)
                Label::OnDraw(ctx, localBounds);
        }
        // Its field writes in its font and colour.
        void OnTextStyleChange() override
        {
            Label::OnTextStyleChange();
            if (m_editor)
                ShareTextStyle(*m_editor);
        }
        // The field lies over its whole box.
        void OnLayout() override
        {
            if (m_editor)
                m_editor->Arrange(Rect{0.0f, 0.0f, GetSize().x, GetSize().y});
        }

    private:
        void EnsureEditor()
        {
            if (m_editor)
                return;
            m_editor = core::MakeRef<TextField>(MemoryAllocator());
            m_editor->AddClass(core::StringView(u8"editor"));
            ShareTextStyle(*m_editor);
            EditableLabel* self = this;
            m_editor->SetOnSubmit([self](core::StringView) { (void)self->EndEdit(true); });
            (void)m_editor->AddEventListener(EventType::FocusLost, [self](const Event&)
            {
                // Focus went elsewhere: committed if it may be, else put back.
                if (self->m_editing && !self->EndEdit(true))
                    (void)self->EndEdit(false);
            });
            AddChild(m_editor.Get());
        }

        core::RefPtr<TextField> m_editor;
        bool m_editing = false;
        core::Function<bool(core::StringView)> m_validator;
        core::Function<void(core::StringView)> m_onCommitted;
    };
}
