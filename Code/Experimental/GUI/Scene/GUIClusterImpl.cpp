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

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    SceneRoot* Node::GetSceneRoot() { return core::Cast<SceneRoot>(GetRootNode()); }

    bool Node::AreMnemonicsShown()
    {
        SceneRoot* root = GetSceneRoot();
        return root != nullptr && root->GetEventDispatcher()->AreMnemonicsShown();
    }

    const PartStyle& UINode::GetPartStyle(core::StringView part) const
    {
        for (const NamedPartStyle& p : m_partStyles)
            if (p.Name.AsView() == part)
                return p.Style;
        static const PartStyle kNone{}; // here, not in the interface: one per process in a shared build
        return kNone;
    }

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
                 // Availability on the active root's focus path (the window the user is in).
                 const bool available = root->GetContext().IsCommandAvailable(commandId.AsView());
                 const Command* command = root->GetContext().GetCommands().Find(commandId.AsView());
                 const bool checked = command != nullptr && command->IsChecked();
                 Untracked([&] { OnBoundCommandState(available, checked); });
             });
        // A second follows what the control shows of it: its label and icon, and its keys (E5).
        Bind([this]()
             {
                 const core::String commandId(m_command.Get());
                 SceneRoot* root = ObserveSceneRoot();
                 if (commandId.Size() == 0 || root == nullptr)
                     return;
                 root->GetContext().GetCommands().TrackChanges();
                 const Command* command = root->GetContext().GetCommands().Find(commandId.AsView());
                 const core::String label(command != nullptr ? command->GetLabel() : commandId.AsView());
                 const core::String icon(command != nullptr ? command->GetIcon() : core::StringView{});
                 // The keys that run it here: the binding matched in this node's own contexts.
                 const Array<KeyContextLayer> contexts = root->KeyContextsAt(this);
                 const core::String shortcut = root->GetContext().GetKeymap().ShortcutText(commandId.AsView(), contexts.AsSpan());
                 Untracked([&] { OnBoundCommandText(label.AsView(), icon.AsView(), shortcut.AsView()); });
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
            root->GetContext().GetCommands().NotifyChanged();
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

    void Node::NotifyRemovingChild(Node& child)
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->NotifySubtreeRemoved(child);
    }

    bool Node::RouteEvent(Event& event)
    {
        // The path, target first, held for the walk: a handler may remove nodes from it.
        Array<RefPtr<Node>> path(MemoryAllocator());
        for (Node* n = this; n != nullptr; n = n->m_parent)
        {
            path.PushBack(RefPtr<Node>(n));
            if (n->m_routeStop)
                break; // a modal popup: what holds it is inert
        }
        Node* const root = GetRootNode();
        // A node removed from the tree during the walk takes no further part in it.
        const auto inTree = [root](Node& n) { return n.GetRootNode() == root; };
        event.Target = this;
        event.Handled = false;
        event.Phase = EventPhase::Capture;
        for (usize i = path.Size(); i-- > 1;)
        {
            if (!inTree(*path[i]))
                continue;
            path[i]->DeliverRouted(event);
            if (event.Handled)
                return true;
        }
        event.Phase = EventPhase::Target;
        if (inTree(*this))
            DeliverRouted(event);
        event.Phase = EventPhase::Bubble;
        // Past an open popup, its owner and what holds that hear the event in their listeners, not
        // their own handlers: an event in a popup is not one on what opened it (E5).
        bool pastPopup = IsInTopLayer();
        for (usize i = 1; i < path.Size() && !event.Handled; ++i)
        {
            if (inTree(*path[i]))
                path[i]->DeliverRouted(event, !pastPopup);
            pastPopup = pastPopup || path[i]->IsInTopLayer();
        }
        return event.Handled;
    }
}
