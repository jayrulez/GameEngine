// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scene_node partition
//
// SceneNode: the root of a widget tree AND its coordinator (eepp Scene::SceneNode). A Node
// subclass that sits at the top; it owns the ActionManager and the MutationQueue, and its
// Update ticks running actions then drains deferred tree edits. Drawing is inherited from
// Node::Draw (walks the subtree). UISceneNode (CSS/stylesheet root) lands with the UI phase.
//
// Nodes reach the coordinator through Node's virtual GetActionManager()/GetMutationQueue(),
// which walk up the parent chain; SceneNode overrides them to return its own.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:scene_node;

import foundation.core; // Duration
import :node;
import :element_types;
import :action_manager;
import :mutation_queue;
import :event_dispatcher;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class SceneNode : public Node
    {
        RTTI_OBJECT(SceneNode, Node)
    public:
        // The element table is this GUI's own (MemoryAllocator is valid in a constructor).
        SceneNode() : m_elements(core::MakeRef<ElementTable>(MemoryAllocator())) {}
        ~SceneNode() override { m_elements->Close(); }

        // Advance the tree by `elapsed`: tick actions, run the per-frame hook, then drain
        // deferred tree edits (e.g. queued Node::Close removals).
        void Update(core::Duration elapsed)
        {
            m_actionManager.Update(elapsed);
            OnUpdate(elapsed);
            m_mutationQueue.Drain();
        }

        [[nodiscard]] ActionManager* GetActionManager() override { return &m_actionManager; }
        [[nodiscard]] MutationQueue* GetMutationQueue() override { return &m_mutationQueue; }
        [[nodiscard]] EventDispatcher* GetEventDispatcher() override { return &m_eventDispatcher; }
        [[nodiscard]] ElementTable* GetElementTable() override { return m_elements.Get(); }
        [[nodiscard]] const ElementSet* GetElementSet() const override { return &m_elementSet; }
        /// What this GUI's markup and scripts may create (every registered element by default).
        void SetElementSet(ElementSet set) { m_elementSet = core::Move(set); }

        // Per-frame hook for subclasses (layout, timers, ...).
        virtual void OnUpdate(core::Duration elapsed) { (void)elapsed; }

    private:
        ActionManager m_actionManager;
        MutationQueue m_mutationQueue;
        EventDispatcher m_eventDispatcher{this}; // root = this SceneNode
        RefPtr<ElementTable> m_elements;          // what weak references to this GUI's nodes resolve through
        ElementSet m_elementSet = ElementSet::All();
    };
}
