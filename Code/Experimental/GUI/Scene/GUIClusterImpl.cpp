// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit for experimental.gui.
//
// Holds Node method bodies that call into the EventDispatcher. They cannot be inline in
// the :node interface partition without making it import :event_dispatcher, which imports
// :node back (a module-partition cycle). An implementation unit sees the whole module (it
// implicitly imports the primary interface) and is outside the interface dependency graph
// - the same pattern as foundation.ui's UIClusterImpl.cpp.

module;
#include "Core/Prelude.h"

module experimental.gui;

namespace core = foundation::core;

namespace experimental::gui
{
    SceneRoot* Node::GetSceneRoot() { return core::Cast<SceneRoot>(GetRootNode()); }

    void Node::SetCommand(StringView id)
    {
        m_command.Set(core::String(id));
        if (m_commandBound)
            return;
        m_commandBound = true;
        // One binding follows the id, the GUI the control is in, focus, the command's state and its
        // handlers: whichever changes, the control's state follows.
        Bind([this]()
             {
                 const core::String commandId(m_command.Get());
                 SceneRoot* root = ObserveSceneRoot();
                 if (commandId.Size() == 0 || root == nullptr)
                     return;
                 const bool available = root->IsCommandAvailable(commandId.AsView());
                 const Command* command = root->GetCommands().Find(commandId.AsView());
                 const bool checked = command != nullptr && command->IsChecked();
                 Untracked([&] { OnBoundCommandState(available, checked); });
             });
    }

    bool Node::RunBoundCommand()
    {
        SceneRoot* root = HasCommand() ? GetSceneRoot() : nullptr;
        return root != nullptr && root->RunCommand(GetCommand());
    }

    void Node::NotifyCommandsChanged()
    {
        if (SceneRoot* root = GetSceneRoot())
            root->GetCommands().NotifyChanged();
    }

    void Node::RequestFocus()
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->SetFocusNode(this);
    }

    void Node::ReleaseFocus()
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            if (dispatcher->GetFocusNode() == this)
                dispatcher->SetFocusNode(nullptr);
    }
}
