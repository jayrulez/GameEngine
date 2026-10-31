// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :page_action_bar partition
//
// PageActionBar: a page's bar bound to the editor's actions over that page. The GUI's commands keep
// one state each for the whole window, but a page's Save, Undo or Simulate acts on and shows its
// own page, so two pages side by side each have their own. Each button is therefore bound to an
// action id: a click executes the action with the page as its subject, and Sync, once a frame,
// gives the button the action's label and keys (its tooltip), whether it is enabled and checked
// over the page, and why it is disabled, each only when it changed.
module;
#include "Core/Prelude.h"

export module editor.app:page_action_bar;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class PageActionBar
    {
    public:
        PageActionBar(IAllocator& allocator, editor::EditorActionRegistry& actions, editor::EditorPage& page)
            : m_actions(&actions), m_page(&page), m_bound(allocator)
        {
        }
        PageActionBar(const PageActionBar&) = delete;
        PageActionBar& operator=(const PageActionBar&) = delete;

        /// `button` runs action `id` over the page from now on, and Sync shows the action's state on
        /// it. Null binds nothing (a bar's markup without that button).
        void Bind(gui::ButtonBase* button, StringView id)
        {
            if (button == nullptr)
                return;
            editor::EditorActionRegistry* actions = m_actions;
            editor::EditorPage* page = m_page;
            const String action(id);
            button->SetOnClick([actions, page, action]() { (void)actions->Execute(action.AsView(), page); });
            Bound bound;
            bound.button = button;
            bound.id = action;
            m_bound.PushBack(Move(bound));
            Show(m_bound.Back());
        }
        /// Updates each bound button to its action's current state over the page.
        void Sync()
        {
            for (Bound& bound : m_bound)
                Show(bound);
        }
        [[nodiscard]] usize Count() const noexcept { return m_bound.Size(); }

    private:
        struct Bound
        {
            gui::ButtonBase* button = nullptr; // the page's, which outlives its bar
            String id;
            String label;
            String keys;
            String reason; // why it is disabled, its tooltip's last line
            bool enabled = false;
            bool checked = false;
            bool shown = false;
        };
        void Show(Bound& bound)
        {
            const editor::EditorActionDeclaration* action = m_actions->Find(bound.id.AsView());
            const String label = action != nullptr ? action->label : String{};
            const String keys = editor::FormatShortcut(m_actions->Shortcut(bound.id.AsView()));
            const bool enabled = action != nullptr && m_actions->IsEnabled(bound.id.AsView(), m_page);
            const bool checked = action != nullptr && m_actions->IsChecked(bound.id.AsView(), m_page);
            if (!bound.shown || label != bound.label || keys != bound.keys)
            {
                bound.button->OnBoundCommandText(label.AsView(), StringView{}, keys.AsView());
                bound.label = label;
                bound.keys = keys;
            }
            const String reason = enabled ? String{} : m_actions->DisabledReason(bound.id.AsView(), m_page);
            if (!bound.shown || enabled != bound.enabled || checked != bound.checked || reason != bound.reason)
            {
                gui::BoundCommandState state;
                state.Available = enabled;
                state.Checked = checked;
                state.Reason = reason.AsView();
                bound.button->OnBoundCommandState(state);
                bound.enabled = enabled;
                bound.checked = checked;
                bound.reason = reason;
            }
            bound.shown = true;
        }

        editor::EditorActionRegistry* m_actions;
        editor::EditorPage* m_page;
        Array<Bound> m_bound;
    };
}
