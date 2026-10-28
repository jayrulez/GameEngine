// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_action_tools partition.
//
// The MCP bridge over the editor's ACTIONS (Documentation/Specs/editor-gui.md, 5.1, layer 15) -
// one generic surface for everything a user can do by command: action_list (every declaration
// with its state over the active page), action_state (one) and action_execute (through the
// registry, the one funnel). An agent never waits for a human: action_execute runs UNATTENDED -
// every modal window the action would open is refused by the GUI's dispatcher and answered as
// cancelled (the accident-preventing answer), and the result names what was suppressed, with the
// note that the action then most likely did nothing and a dedicated tool or the user is the way
// forward (ezEngine's contract).
module;
#include "Core/Prelude.h"

export module editor.app:mcp_action_tools;

import foundation.core;
import foundation.gui;
import foundation.json;
import foundation.mcp;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    /// The live editor the action tools see: the registry through the context, and the GUI's
    /// dispatcher whose modal windows an unattended call refuses (null = nothing to refuse, a
    /// headless test).
    struct ActionToolSeams
    {
        editor::EditorContext* context = nullptr;
        foundation::gui::EventDispatcher* dialogs = nullptr;
    };

    /// The unattended scope: while alive, every modal window opened through `dialogs` is refused
    /// (answered as cancelled) and recorded by its title. Installs the dispatcher's modal
    /// interceptor and restores what was there (scopes nest).
    class UnattendedDialogs
    {
    public:
        explicit UnattendedDialogs(foundation::gui::EventDispatcher* dialogs) : m_dialogs(dialogs)
        {
            if (m_dialogs == nullptr)
                return;
            m_previous = m_dialogs->TakeModalInterceptor();
            UnattendedDialogs* self = this;
            m_dialogs->SetModalInterceptor([self](foundation::gui::Node& window)
                                           {
                                               const auto* titled = Cast<foundation::gui::Window>(&window);
                                               self->m_suppressed.PushBack(titled != nullptr ? String(titled->GetTitle()) : String(u8"(untitled)"));
                                               return false;
                                           });
        }
        ~UnattendedDialogs()
        {
            if (m_dialogs != nullptr)
                m_dialogs->SetModalInterceptor(Move(m_previous));
        }
        UnattendedDialogs(const UnattendedDialogs&) = delete;
        UnattendedDialogs& operator=(const UnattendedDialogs&) = delete;

        [[nodiscard]] Span<const String> Suppressed() const noexcept
        {
            return Span<const String>{m_suppressed.Data(), m_suppressed.Size()};
        }

    private:
        foundation::gui::EventDispatcher* m_dialogs;
        Function<bool(foundation::gui::Node&)> m_previous;
        Array<String> m_suppressed;
    };

    /// The number of tools RegisterActionTools registers (action_list / action_state /
    /// action_execute); a tripwire like kPageToolCount.
    inline constexpr usize kActionToolCount = 3;

    void RegisterActionTools(foundation::mcp::McpServer& server, ActionToolSeams seams);
}
