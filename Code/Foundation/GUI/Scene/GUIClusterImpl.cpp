// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit for foundation.gui.
//
// Holds Node method bodies that call into the EventDispatcher. They cannot be inline in
// the :node interface partition without making it import :event_dispatcher, which imports
// :node back (a module-partition cycle). An implementation unit sees the whole module (it
// implicitly imports the primary interface) and is outside the interface dependency graph.

module;
#include "Core/Prelude.h"

module foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

namespace foundation::gui
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
        static const PartStyle kNone{}; // defined here, not in the interface, so a shared build has one per process
        return kNone;
    }

    void Node::SetCommand(StringView id)
    {
        m_command.Set(core::String(id));
        if (m_commandBound)
            return;
        m_commandBound = true;
        // The first binding follows the command id, the GUI the control is in, the focus, the
        // command's state and its handlers. Whichever changes, the control's state is updated.
        Bind([this]()
             {
                 const core::String commandId(m_command.Get());
                 SceneRoot* root = ObserveSceneRoot();
                 if (commandId.Size() == 0 || root == nullptr)
                     return;
                 // Availability is decided on the active root's focus path (the window the user is
                 // in).
                 const bool available = root->GetContext().IsCommandAvailable(commandId.AsView());
                 const Command* command = root->GetContext().GetCommands().Find(commandId.AsView());
                 BoundCommandState state;
                 state.Available = available;
                 state.Checked = command != nullptr && command->IsChecked();
                 const core::String reason(command != nullptr && !command->IsEnabled() ? command->GetDisabledReason() : core::StringView{});
                 state.Reason = reason.AsView();
                 Untracked([&] { OnBoundCommandState(state); });
             });
        // The second binding follows what the control shows of the command: its label, its icon
        // and its keys.
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
                 // The keys that run the command here: the binding that matches in this node's own
                 // contexts.
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

    void Node::Leave()
    {
        if (m_leaving || m_parent == nullptr)
            return;
        EventDispatcher* dispatcher = GetEventDispatcher();
        if (dispatcher == nullptr)
        {
            RemoveFromParent(); // in no GUI, so there is no exit to wait for and nothing walking the tree
            return;
        }
        m_leaving = true;
        if (m_enterPhase != 0)
        {
            m_enterPhase = 0; // the entrance is cut short, and the exit starts from where it is
            MarkStyleStateDirty(StateEntering);
        }
        // Take the node out of input now, as for a removed subtree (hover, press, capture, focus,
        // its popups), so a second Back while it fades does not reach it.
        dispatcher->NotifySubtreeRemoved(*this);
        MarkStyleStateDirty(StateLeaving);
        Invalidate();
        // A sheet plays the exit. In a GUI without a style pass, or for a node that is not a widget
        // (sheets style only widgets), there is no exit, so the node goes at the GUI's next drain,
        // never inside the event that sent it away.
        if (!GetRootNode()->HasStylePass() || Cast<UIWidget>(this) == nullptr)
            FinishLeaving();
    }

    void Node::FinishLeaving()
    {
        if (!m_leaving || m_leaveQueued)
            return;
        MutationQueue* queue = GetMutationQueue();
        if (queue == nullptr)
        {
            RemoveFromParent();
            return;
        }
        m_leaveQueued = true;
        // Removal never happens inside the style pass's walk or an animation's tick, but at the
        // GUI's next drain. The node goes then only if it is still leaving; a node removed or taken
        // back meanwhile is no longer leaving.
        RefPtr<Node> self(this);
        queue->Enqueue([self]()
                       {
                           if (self->m_leaving)
                               self->RemoveFromParent();
                       });
    }

    void Node::Enter()
    {
        if (m_leaving || m_parent == nullptr)
            return;
        if (GetEventDispatcher() == nullptr || !GetRootNode()->HasStylePass() || Cast<UIWidget>(this) == nullptr)
        {
            m_enterPhase = 0;
            SendEvent(Event(EventType::Entered, this)); // no entrance can play here
            return;
        }
        m_enterPhase = 1;
        m_entranceStart = true;
        MarkStyleStateDirty(StateEntering);
        Invalidate();
    }

    void Node::MotionSettled()
    {
        // The style pass has not applied the node's state yet (an earlier transition ended first).
        // It calls this again once it has.
        if (IsStyleDirty())
            return;
        if (m_leaving)
        {
            FinishLeaving();
            return;
        }
        if (m_enterPhase == 0 || m_enterQueued)
            return;
        const auto advance = [](Node& node)
        {
            if (node.m_enterPhase == 1)
            {
                node.m_enterPhase = 2; // :entering ends, and the node moves to its own style through its transitions
                node.MarkStyleStateDirty(StateEntering);
            }
            else if (node.m_enterPhase == 2)
            {
                node.m_enterPhase = 0;
                node.SendEvent(Event(EventType::Entered, &node));
            }
        };
        MutationQueue* queue = GetMutationQueue();
        if (queue == nullptr)
        {
            advance(*this);
            return;
        }
        // Advance at the GUI's next drain, not inside the style pass's walk or an animation's tick,
        // and only if the entrance is still at this phase then.
        m_enterQueued = true;
        RefPtr<Node> self(this);
        const u8 phase = m_enterPhase;
        queue->Enqueue([self, phase, advance]()
                       {
                           self->m_enterQueued = false;
                           if (self->m_enterPhase == phase)
                               advance(*self);
                       });
    }

    void Node::SetInert(bool inert)
    {
        if (inert == m_inert)
            return;
        m_inert = inert;
        if (inert)
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->NotifySubtreeRemoved(*this); // an inert subtree holds no input state
    }

    void Node::NotifyRemovingChild(Node& child)
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->NotifySubtreeRemoved(child);
    }

    bool Node::RouteEvent(Event& event)
    {
        // The path, target first, with references held for the walk, since a handler may remove
        // nodes from it.
        Array<RefPtr<Node>> path(MemoryAllocator());
        for (Node* n = this; n != nullptr; n = n->m_parent)
        {
            path.PushBack(RefPtr<Node>(n));
            if (n->m_routeStop)
                break; // a modal popup: its ancestors are inert
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
        // Beyond an open popup, its owner and the owner's ancestors receive the event in their
        // listeners but not in their own handlers, since an event in a popup is not an event on the
        // node that opened it.
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
