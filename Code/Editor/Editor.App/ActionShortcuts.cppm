// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :action_shortcuts partition.
//
// ActionShortcuts: the editor's keys generated from the action registry. Every action with an
// effective chord (the user's override or the declaration's default) and every alternate chord is
// bound in the GUI's keymap to a command named by the action's id, and the window's root handles
// each such command with the registry's Execute, so a chord and a menu click do the same thing.
// The keymap runs a binding of a focused widget's own context before one with none, so a focused
// text field keeps Ctrl+Z for its own undo. On every registry change (a registration, a rebind)
// the old bindings and handlers are removed and everything is bound again.
module;
#include "Core/Prelude.h"

export module editor.app:action_shortcuts;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class ActionShortcuts
    {
    public:
        /// The bindings go in `keymap`; `handler` (the window's root, on every focus path) runs them.
        ActionShortcuts(IAllocator& allocator, gui::Keymap& keymap, gui::Node& handler, EditorActionRegistry& actions)
            : m_allocator(allocator), m_keymap(&keymap), m_handler(&handler), m_actions(&actions), m_bound(allocator), m_handlers(allocator)
        {
            ActionShortcuts* self = this;
            m_changed = MakeUnique<gui::Effect>(m_allocator, [self]()
                                                {
                                                    self->m_actions->TrackChanges();
                                                    self->Rebind();
                                                });
        }
        ActionShortcuts(const ActionShortcuts&) = delete;
        ActionShortcuts& operator=(const ActionShortcuts&) = delete;
        /// The commands run through this object, so destroying it removes its bindings and
        /// handlers and stops the registry calling it.
        ~ActionShortcuts()
        {
            m_changed.Reset();
            Unbind();
        }

        /// Binds every key again from the registry's current state.
        void Rebind()
        {
            Unbind();
            for (const EditorActionDeclaration& action : m_actions->Actions())
            {
                const EditorShortcut chord = m_actions->Shortcut(action.id.AsView());
                const bool main = Bind(action.id.AsView(), chord);
                const bool alternate = Bind(action.id.AsView(), action.alternateShortcut);
                if (!main && !alternate)
                    continue;
                const EditorActionRegistry* actions = m_actions;
                const String id = action.id;
                m_handlers.PushBack(m_handler->OnCommand(id.AsView(), [actions, id](gui::CommandEvent&) { (void)actions->Execute(id.AsView()); }));
            }
        }
        /// How many keys are bound (a chord and an alternate are two).
        [[nodiscard]] usize BoundCount() const noexcept { return m_bound.Size(); }

    private:
        bool Bind(StringView id, const EditorShortcut& chord)
        {
            if (!chord.IsSet() || !m_keymap->Bind(chord.stroke, id))
                return false;
            m_bound.PushBack(String(id));
            return true;
        }
        void Unbind()
        {
            // Each command once: UnbindCommand takes all its keys.
            for (usize i = 0; i < m_bound.Size(); ++i)
            {
                bool earlier = false;
                for (usize j = 0; j < i && !earlier; ++j)
                    earlier = m_bound[j] == m_bound[i];
                if (!earlier)
                    (void)m_keymap->UnbindCommand(m_bound[i].AsView());
            }
            m_bound.Clear();
            for (u32 token : m_handlers)
                (void)m_handler->RemoveCommandHandler(token);
            m_handlers.Clear();
        }

        IAllocator& m_allocator;
        gui::Keymap* m_keymap;
        gui::Node* m_handler;
        EditorActionRegistry* m_actions;
        Array<String> m_bound;   // a command per key bound
        Array<u32> m_handlers;   // the handler's command tokens
        UniquePtr<gui::Effect> m_changed; // rebinds when the registry changes
    };
}
