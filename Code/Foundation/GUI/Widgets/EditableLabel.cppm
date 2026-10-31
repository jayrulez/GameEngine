// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :editable_label partition
//
// EditableLabel: a label that turns into a text field in place, such as a layer name or a file
// name in a list. A double-click, or F2 while the label is focused, starts editing: a text field
// covers the text with all of it selected. Return or moving focus away commits; Escape cancels.
// A multiline label (a comment, say) edits in a text area instead. There Return breaks a line,
// Ctrl+Return commits, and the label's box grows and shrinks with the lines typed. Opening the
// field's context menu (cut, copy, paste) does not end the edit. A validator may refuse the typed
// text: Return then leaves the label editing with the field marked `.invalid`, and moving focus
// away restores the old text. A committed change sends a `change` event.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:editable_label;

import foundation.core; // Function, Move, String, RefPtr
import :rect;
import :event;
import :draw_context;
import :text; // TextWhiteSpace
import :label;
import :text_edit_base;
import :text_field;
import :text_area;
import :menu; // the field's context menu

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
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

        /// Chooses whether the label edits in a text area (Return breaks a line, Ctrl+Return
        /// commits) rather than a single-line field. A multiline label keeps its line breaks and
        /// wraps (white-space: pre-wrap, unless a style sheet sets otherwise) and aligns its text to
        /// the top, where the text area puts it, so a box shorter than the text cuts off the end.
        /// Ignored while the label is editing.
        void SetMultiline(bool multiline)
        {
            if (multiline == m_multiline || m_editing)
                return;
            m_multiline = multiline;
            {
                StyleWriteScope defaults(*this);
                SetWhiteSpace(multiline ? TextWhiteSpace::PreWrap : TextWhiteSpace::Normal);
                SetTextAlignment(GetTextAlignH(), multiline ? TextVAlign::Top : TextVAlign::Middle);
            }
            if (m_editor)
            {
                RemoveChild(m_editor.Get());
                m_editor = nullptr;
            }
        }
        [[nodiscard]] bool IsMultiline() const noexcept { return m_multiline; }

        /// Starts editing, as a double-click or F2 does.
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
        /// Ends editing. With `commit`, the typed text replaces the label's text if the validator
        /// accepts it; without, the old text stays. Returns false when the validator refused the
        /// text, in which case the label is still editing.
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
                RequestFocus(); // the label takes over the focus the field had
            RequestLayout(); // the box is sized by the label's own text again
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
        /// Returns the field it edits in (a text area when multiline), or null before the first edit.
        [[nodiscard]] TextEditBase* GetEditor() const noexcept { return m_editor.Get(); }

        /// Sets the check that decides whether typed text may be committed. When it returns false
        /// the label stays editing.
        void SetValidator(core::Function<bool(core::StringView)> validator) { m_validator = core::Move(validator); }
        /// Sets the callback that receives each change the user commits.
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
        // While editing, the field draws the text and the label draws nothing of its own.
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (!m_editing)
                Label::OnDraw(ctx, localBounds);
        }
        // The field uses the label's font and colour.
        void OnTextStyleChange() override
        {
            Label::OnTextStyleChange();
            if (m_editor)
                ShareTextStyle(*m_editor);
        }
        // While a text area is editing, the label measures the typed text, so it grows with the lines.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2 available) override
        {
            if (m_editing && m_multiline)
                return static_cast<const TextArea*>(m_editor.Get())->MeasureText(available.x);
            return Label::MeasureContent(available);
        }
        // The field is placed so its text lies where the label's text was: the field's content box
        // matches the label's, and the field's padding and border extend past the label's box.
        // Fitting the whole field inside the label's box would leave a one-line label no room for
        // the typed text once the field's padding is taken.
        void OnLayout() override
        {
            if (!m_editor)
                return;
            const Rect content = GetContentBounds();
            const Thickness chrome = m_editor->GetChrome();
            m_editor->Arrange(Rect{content.x - chrome.Left, content.y - chrome.Top, content.width + chrome.TotalHorizontal(),
                                   content.height + chrome.TotalVertical()});
        }

    private:
        void EnsureEditor()
        {
            if (m_editor)
                return;
            EditableLabel* self = this;
            if (m_multiline)
            {
                core::RefPtr<TextArea> area = core::MakeRef<TextArea>(MemoryAllocator());
                area->SetOnTextChanging([self](core::StringView) { self->RequestLayout(); });
                m_editor = area;
            }
            else
                m_editor = core::MakeRef<TextField>(MemoryAllocator());
            m_editor->AddClass(core::StringView(u8"editor"));
            ShareTextStyle(*m_editor);
            m_editor->SetOnSubmit([self](core::StringView) { (void)self->EndEdit(true); });
            (void)m_editor->AddEventListener(EventType::FocusLost, [self](const Event&)
            {
                // Focus went elsewhere: commit if the validator allows, otherwise restore the old
                // text. Focus moving to the field's own context menu does not count, since the menu
                // gives focus back when it closes (after a cut, for example).
                if (const Menu* menu = self->m_editor->GetContextMenu(); menu != nullptr && menu->IsOpen())
                    return;
                if (self->m_editing && !self->EndEdit(true))
                    (void)self->EndEdit(false);
            });
            AddChild(m_editor.Get());
        }

        core::RefPtr<TextEditBase> m_editor;
        bool m_editing = false;
        bool m_multiline = false;
        core::Function<bool(core::StringView)> m_validator;
        core::Function<void(core::StringView)> m_onCommitted;
    };
}
